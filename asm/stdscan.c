/* ----------------------------------------------------------------------- *
 *
 *   Copyright 1996-2024 The NASM Authors - All Rights Reserved
 *   See the file AUTHORS included with the NASM distribution for
 *   the specific copyright holders.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following
 *   conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *     CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *     MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *     DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *     CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *     NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *     OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *     EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ----------------------------------------------------------------------- */

#include "compiler.h"

#include "nctype.h"

#include "nasm.h"
#include "nasmlib.h"
#include "error.h"
#include "quote.h"
#include "stdscan.h"
#include "insns.h"

/*
 * Standard scanner routine used by parser.c and some output
 * formats. It keeps a succession of temporary-storage strings in
 * stdscan_tempstorage, which can be cleared using stdscan_reset.
 */
enum stdscan_scan_state {
    ss_init                    /* Normal scanner state */
};

struct token_stack {
    struct token_stack *prev;
    struct tokenval tv;
};

struct stdscan_state {
    char *bufptr;
    struct token_stack *pushback;
    enum stdscan_scan_state sstate;
};

static struct stdscan_state scan;
static char **stdscan_tempstorage = NULL;
static int stdscan_tempsize = 0, stdscan_templen = 0;
#define STDSCAN_TEMP_DELTA 256

static void *stdscan_alloc(size_t bytes);

void stdscan_set(const struct stdscan_state *state)
{
    scan = *state;
}

const struct stdscan_state *stdscan_get(void)
{
    struct stdscan_state *save = stdscan_alloc(sizeof(struct stdscan_state));
    *save = scan;
    return save;
}

char *stdscan_tell(void)
{
    return scan.bufptr;
}

static void stdscan_pop(void)
{
    nasm_free(stdscan_tempstorage[--stdscan_templen]);
}

static void stdscan_pushback_pop(void)
{
    struct token_stack *ps;

    ps = scan.pushback->prev;
    nasm_free(scan.pushback);
    scan.pushback = ps;
}

void stdscan_reset(char *buffer)
{
    while (stdscan_templen > 0)
        stdscan_pop();

    while (scan.pushback)
        stdscan_pushback_pop();

    scan.bufptr   = buffer;
    scan.sstate   = ss_init;
}

/*
 * Unimportant cleanup is done to avoid confusing people who are trying
 * to debug real memory leaks
 */
void stdscan_cleanup(void)
{
    stdscan_reset(NULL);
    nasm_free(stdscan_tempstorage);
}

static void *stdscan_alloc(size_t bytes)
{
    void *buf = nasm_malloc(bytes);
    if (stdscan_templen >= stdscan_tempsize) {
        stdscan_tempsize += STDSCAN_TEMP_DELTA;
        stdscan_tempstorage = nasm_realloc(stdscan_tempstorage,
                                           stdscan_tempsize *
                                           sizeof(char *));
    }
    stdscan_tempstorage[stdscan_templen++] = buf;

    return buf;
}

static char *stdscan_copy(const char *p, int len)
{
    char *text = stdscan_alloc(len+1);
    memcpy(text, p, len);
    text[len] = '\0';

    return text;
}

void stdscan_pushback(const struct tokenval *tv)
{
    struct token_stack *ts;

    nasm_new(ts);
    ts->tv = *tv;
    ts->prev = scan.pushback;
    scan.pushback = ts;
}

/*
 * a token is enclosed with braces. proper token type will be assigned
 * accordingly with the token flag.
 */
static int stdscan_handle_brace(struct tokenval *tv)
{
    if (!(tv->t_flag & TFLAG_BRC_ANY)) {
        /* invalid token is put inside braces */
        nasm_nonfatal("`{%s}' is not a valid token", tv->t_charptr);
        tv->t_type = TOKEN_INVALID;
    } else if (tv->t_flag & TFLAG_BRC_OPT) {
        if (is_reg_class(OPMASKREG, tv->t_integer)) {
            /* within braces, opmask register is now used as a mask */
            tv->t_type = TOKEN_OPMASK;
        }
    }

    return tv->t_type;
}

/*
 * Parse a braced token
 */

static int stdscan_parse_braces(struct tokenval *tv)
{
    int token_len;
    char *r;

    r = scan.bufptr = nasm_skip_spaces(++scan.bufptr);

    /*
     * read the entire buffer to advance the buffer pointer
     * {rn-sae}, {rd-sae}, {ru-sae}, {rz-sae} contain '-' in tokens.
     */
    while (nasm_isbrcchar(*scan.bufptr))
        scan.bufptr++;

    token_len = scan.bufptr - r;

    /* ... copy only up to DECOLEN_MAX-1 characters */
    if (token_len <= MAX_KEYWORD)
        tv->t_charptr = stdscan_copy(r, token_len);

    scan.bufptr = nasm_skip_spaces(scan.bufptr);
    /* if brace is not closed properly or token is too long  */
    if (*scan.bufptr != '}') {
        nasm_nonfatal("unterminated braces at end of line");
        return tv->t_type = TOKEN_INVALID;
    }
    scan.bufptr++;       /* skip closing brace */

    if (token_len > MAX_KEYWORD) {
        nasm_nonfatal("`{%.*s}' is not a valid token", token_len, r);
        return tv->t_type = TOKEN_INVALID;
    }

    /* handle tokens inside braces */
    nasm_token_hash(tv->t_charptr, tv);
    return stdscan_handle_brace(tv);
}

static int stdscan_token(struct tokenval *tv);

int stdscan(void *private_data, struct tokenval *tv)
{
    int i;

    (void)private_data;         /* Don't warn that this parameter is unused */


    if (unlikely(scan.pushback)) {
        *tv = scan.pushback->tv;
        stdscan_pushback_pop();
        return tv->t_type;
    }

    nasm_zero(*tv);

    scan.bufptr = nasm_skip_spaces(scan.bufptr);
    tv->t_start = scan.bufptr;

    if (!*scan.bufptr)
        return tv->t_type = TOKEN_EOS;

    i = stdscan_token(tv);
    tv->t_len = scan.bufptr - tv->t_start;

    return i;
}

static int stdscan_token(struct tokenval *tv)
{
    const char *r;

    /* we have a token; either an id, a number, operator or char */
    if (nasm_isidstart(*scan.bufptr) ||
        (*scan.bufptr == '$' && nasm_isidstart(scan.bufptr[1]))) {
        /* now we've got an identifier */
        bool is_sym = false;
        int token_type;

        if (*scan.bufptr == '$') {
            is_sym = true;
            scan.bufptr++;
        }

        r = scan.bufptr++;
        /* read the entire buffer to advance the buffer pointer but... */
        while (nasm_isidchar(*scan.bufptr))
            scan.bufptr++;

        /* ... copy only up to IDLEN_MAX-1 characters */
        tv->t_charptr = stdscan_copy(r, scan.bufptr - r < IDLEN_MAX ?
                                     scan.bufptr - r : IDLEN_MAX - 1);

        if (is_sym || scan.bufptr - r > MAX_KEYWORD)
            return tv->t_type = TOKEN_ID;       /* bypass all other checks */

        token_type = nasm_token_hash(tv->t_charptr, tv);
        if (unlikely(tv->t_flag & TFLAG_WARN)) {
            /*! ptr [on] non-NASM keyword used in other assemblers
             *!  warns about keywords used in other assemblers that
             *!  might indicate a mistake in the source code.
             *!  Currently only the MASM \c{PTR} keyword is
             *!  recognized. If (limited) MASM compatibility is
             *!  desired, the \c{%use masm} macro package is
             *!  available, see \k{pkg_masm}; however, carefully note
             *!  the caveats listed.
             */
            nasm_warn(WARN_PTR, "`%s' is not a NASM keyword",
                       tv->t_charptr);
        }

        if (likely(!(tv->t_flag & TFLAG_BRC))) {
            /* most of the tokens fall into this case */
            return token_type;
        } else {
            return tv->t_type = TOKEN_ID;
        }
    } else if (*scan.bufptr == '$' && !nasm_isnumchar(scan.bufptr[1])) {
        /*
         * It's a $ sign with no following hex number; this must
         * mean it's a Here token ($), evaluating to the current
         * assembly location, or a Base token ($$), evaluating to
         * the base of the current segment.
         */
        scan.bufptr++;
        if (*scan.bufptr == '$') {
            scan.bufptr++;
            return tv->t_type = TOKEN_BASE;
        }
        return tv->t_type = TOKEN_HERE;
    } else if (nasm_isnumstart(*scan.bufptr)) {   /* now we've got a number */
        bool rn_error;
        bool is_hex = false;
        bool is_float = false;
        bool has_e = false;
        char c;

        r = scan.bufptr;

        if (*scan.bufptr == '$') {
            scan.bufptr++;
            is_hex = true;
        }

        for (;;) {
            c = *scan.bufptr++;

            if (!is_hex && (c == 'e' || c == 'E')) {
                has_e = true;
                if (*scan.bufptr == '+' || *scan.bufptr == '-') {
                    /*
                     * e can only be followed by +/- if it is either a
                     * prefixed hex number or a floating-point number
                     */
                    is_float = true;
                    scan.bufptr++;
                }
            } else if (c == 'H' || c == 'h' || c == 'X' || c == 'x') {
                is_hex = true;
            } else if (c == 'P' || c == 'p') {
                is_float = true;
                if (*scan.bufptr == '+' || *scan.bufptr == '-')
                    scan.bufptr++;
            } else if (nasm_isnumchar(c))
                ; /* just advance */
            else if (c == '.')
                is_float = true;
            else
                break;
        }
        scan.bufptr--;       /* Point to first character beyond number */

        if (has_e && !is_hex) {
            /* 1e13 is floating-point, but 1e13h is not */
            is_float = true;
        }

        if (is_float) {
            tv->t_charptr = stdscan_copy(r, scan.bufptr - r);
            return tv->t_type = TOKEN_FLOAT;
        } else {
            r = stdscan_copy(r, scan.bufptr - r);
            tv->t_integer = readnum(r, &rn_error);
            stdscan_pop();
            if (rn_error) {
                /* some malformation occurred */
                return tv->t_type = TOKEN_ERRNUM;
            }
            tv->t_charptr = NULL;
            return tv->t_type = TOKEN_NUM;
        }
    } else if (*scan.bufptr == '\'' || *scan.bufptr == '"' ||
               *scan.bufptr == '`') {
        /* a quoted string */
        char start_quote = *scan.bufptr;
        tv->t_charptr = scan.bufptr;
        tv->t_inttwo = nasm_unquote(tv->t_charptr, &scan.bufptr);
        if (*scan.bufptr != start_quote)
            return tv->t_type = TOKEN_ERRSTR;
        scan.bufptr++;       /* Skip final quote */
        return tv->t_type = TOKEN_STR;
    } else if (*scan.bufptr == '{') {
        return stdscan_parse_braces(tv);
        /* now we've got a decorator */
    } else if (*scan.bufptr == ';') {
        /* a comment has happened - stay */
        return tv->t_type = TOKEN_EOS;
    } else if (scan.bufptr[0] == '>' && scan.bufptr[1] == '>') {
        if (scan.bufptr[2] == '>') {
            scan.bufptr += 3;
            return tv->t_type = TOKEN_SAR;
        } else {
            scan.bufptr += 2;
            return tv->t_type = TOKEN_SHR;
        }
    } else if (scan.bufptr[0] == '<' && scan.bufptr[1] == '<') {
        scan.bufptr += scan.bufptr[2] == '<' ? 3 : 2;
        return tv->t_type = TOKEN_SHL;
    } else if (scan.bufptr[0] == '/' && scan.bufptr[1] == '/') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_SDIV;
    } else if (scan.bufptr[0] == '%' && scan.bufptr[1] == '%') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_SMOD;
    } else if (scan.bufptr[0] == '=' && scan.bufptr[1] == '=') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_EQ;
    } else if (scan.bufptr[0] == '<' && scan.bufptr[1] == '>') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_NE;
    } else if (scan.bufptr[0] == '!' && scan.bufptr[1] == '=') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_NE;
    } else if (scan.bufptr[0] == '<' && scan.bufptr[1] == '=') {
        if (scan.bufptr[2] == '>') {
            scan.bufptr += 3;
            return tv->t_type = TOKEN_LEG;
        } else {
            scan.bufptr += 2;
            return tv->t_type = TOKEN_LE;
        }
    } else if (scan.bufptr[0] == '>' && scan.bufptr[1] == '=') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_GE;
    } else if (scan.bufptr[0] == '&' && scan.bufptr[1] == '&') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_DBL_AND;
    } else if (scan.bufptr[0] == '^' && scan.bufptr[1] == '^') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_DBL_XOR;
    } else if (scan.bufptr[0] == '|' && scan.bufptr[1] == '|') {
        scan.bufptr += 2;
        return tv->t_type = TOKEN_DBL_OR;
    } else {
        /* just an ordinary char */
        return tv->t_type = (uint8_t)(*scan.bufptr++);
    }
}
