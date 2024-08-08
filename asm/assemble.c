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

/*
 * assemble.c   code generation for the Netwide Assembler
 *
 * The entry points to this module are insn_size() for the non-final
 * passes, and assemble() for the final pass.
 */

#include "compiler.h"


#include "nasm.h"
#include "nasmlib.h"
#include "error.h"
#include "assemble.h"
#include "insns.h"
#include "tables.h"
#include "disp8.h"
#include "listing.h"
#include "dbginfo.h"

enum match_result {
    /*
     * Matching errors.  These should be sorted so that more specific
     * errors come later in the sequence.
     */
    MERR_INVALOP,
    MERR_OPSIZEMISSING,
    MERR_OPSIZEMISMATCH,
    MERR_BRNOTHERE,
    MERR_BRNUMMISMATCH,
    MERR_MASKNOTHERE,
    MERR_DECONOTHERE,
    MERR_BADZU,
    MERR_MEMZU,
    MERR_BADNF,
    MERR_REQNF,
    MERR_BADCPU,
    MERR_BADMODE,
    MERR_BADHLE,
    MERR_ENCMISMATCH,
    MERR_BADBND,
    MERR_BADREPNE,
    MERR_REGSETSIZE,
    MERR_REGSET,
    MERR_WRONGIMM,
    /*
     * Matching success; the conditional ones first
     */
    MOK_JUMP,		/* Matching OK but needs jmp_match() */
    MOK_GOOD		/* Matching unconditionally OK */
};

#define GEN_SIB(scale, index, base)                 \
        (((scale) << 6) | ((index) << 3) | ((base)))

#define GEN_MODRM(mod, reg, rm)                     \
        (((mod) << 6) | (((reg) & 7) << 3) | ((rm) & 7))

static int64_t calcsize(insn *, const struct itemplate *);
static int emit_prefixes(struct out_data *data, const insn *ins);
static void gencode(struct out_data *data, insn *ins);
static enum match_result find_match(const struct itemplate **tempp,
                                    insn *instruction);
static enum match_result matches(const struct itemplate *, const insn *);
static opflags_t regflag(const operand *);
static int32_t regval(const operand *);
static uint32_t rexflags(int, opflags_t, uint32_t);
static uint32_t op_rexflags(const operand *, uint32_t);
static uint32_t op_evexflags(const operand *, uint32_t);
static void add_asp(insn *);

static int process_ea(operand *input, int rfield, opflags_t rflags,
                      insn *ins, enum ea_type expected,
                      const char **errmsgp);

/*
 * Return the byte value of a legacy prefix (possibly depending on context)
 * Returns one of the following values if the prefix has no byte value:
 */
enum prefix_err {
    PFE_NULL  = -1,             /* No output */
    PFE_MULTI = -2,             /* Multibyte output (VEX, EVEX, REX2) */
    PFE_ERR   = -3,             /* Invalid prefix use */
    PFE_WHAT  = -4              /* Not a valid prefix (internal error) */
};

static int prefix_byte(enum prefixes pfx, const int bits);

/*
 * Return any of REX_[BXR]1 corresponding to non-GPR registers by
 * masking them with the REX_[BXR]V flags.
 */
static inline uint32_t rex_highvec(uint32_t rexflags)
{
    return rexflags & (rexflags >> 4) & REX_BXR1;
}

/* Get the pointer to an operand if it exits */
static inline struct operand *get_operand(insn *ins, unsigned int n)
{
    if (n >= (unsigned int)ins->operands)
        return NULL;
    else
        return &ins->oprs[n];
}

static inline bool absolute_op(const struct operand *o)
{
    return o && o->segment == NO_SEG && o->wrt == NO_SEG &&
        !(o->opflags & OPFLAG_RELATIVE);
}

static int has_prefix(const insn * ins, enum prefix_pos pos, int prefix)
{
    return ins->prefixes[pos] == prefix;
}

static int assert_no_prefix(insn * ins, enum prefix_pos pos)
{
    if (ins->prefixes[pos]) {
        nasm_nonfatal("invalid %s prefix", prefix_name(ins->prefixes[pos]));
        return -1;
    }
    return 0;
}

static const char *size_name(int size)
{
    switch (size) {
    case 1:
        return "byte";
    case 2:
        return "word";
    case 4:
        return "dword";
    case 8:
        return "qword";
    case 10:
        return "tword";
    case 16:
        return "oword";
    case 32:
        return "yword";
    case 64:
        return "zword";
    default:
        return "???";
    }
}

static void warn_overflow(int size, const char *prefix, const char *suffix)
{
    if (!prefix)
        prefix = "";
    if (!suffix)
        suffix = "";

    nasm_warn(ERR_PASS2 | WARN_NUMBER_OVERFLOW,
              "%s%s%s exceeds bounds",
              prefix, size_name(size), suffix);
}

static void warn_overflow_const(int64_t data, int size)
{
    if (overflow_general(data, size))
        warn_overflow(size, NULL, NULL);
}

static void warn_overflow_out(int64_t data, int size, enum out_flags flags)
{
    bool err;
    const char *prefix;

    if (flags & OUT_SIGNED) {
        prefix = "signed ";
        err = overflow_signed(data, size);
    } else if (flags & OUT_UNSIGNED) {
        prefix = "unsigned ";
        err = overflow_unsigned(data, size);
    } else {
        prefix = NULL;
        err = overflow_general(data, size);
    }

    if (err)
        warn_overflow(size, prefix, NULL);
}

/*
 * Collect macro-related debug information, if applicable.
 */
static void debug_macro_out(const struct out_data *data)
{
    struct debug_macro_addr *addr;
    uint64_t start = data->loc.offset;
    uint64_t end  = start + data->size;

    addr = debug_macro_get_addr(data->loc.segment);
    while (addr) {
        if (!addr->len)
            addr->start = start;
        addr->len = end - addr->start;
        addr = addr->up;
    }
}

/*
 * This routine wrappers the real output format's output routine,
 * in order to pass a copy of the data off to the listing file
 * generator at the same time, flatten unnecessary relocations,
 * and verify backend compatibility.
 */
/*
 * This warning is currently issued by backends, but in the future
 * this code should be centralized.
 *
 *!zeroing [on] \c{RES}\e{x} in initialized section becomes zero
 *!  a \c{RES}\e{x} directive was used in a section which contains
 *!  initialized data, and the output format does not support
 *!  this. Instead, this will be replaced with explicit zero
 *!  content, which may produce a large output file.
 */
static void out(struct out_data *data)
{
    static struct last_debug_info {
        struct src_location where;
        int32_t segment;
    } dbg;
    union {
        uint8_t b[8];
        uint64_t q;
    } xdata;
    size_t asize, amax;
    uint64_t zeropad = 0;
    int64_t addrval;
    int32_t fixseg;             /* Segment for which to produce fixed data */

    if (!data->size)
        return;                 /* Nothing to do */

    /*
     * Convert addresses to RAWDATA if possible
     * XXX: not all backends want this for global symbols!!!!
     */
    switch (data->type) {
    case OUT_ADDRESS:
        addrval = data->toffset;
        fixseg = NO_SEG;        /* Absolute address is fixed data */
        goto address;

    case OUT_RELADDR:
        addrval = data->toffset - data->relbase;
        fixseg = data->loc.segment; /* Our own segment is fixed data */
        goto address;

    address:
        nasm_assert(data->size <= 8);
        asize = data->size;
        amax = ofmt->maxbits >> 3; /* Maximum address size in bytes */
        if (data->tsegment == fixseg && data->twrt == NO_SEG) {
            if (!(ofmt->flags & OFMT_KEEP_ADDR)) {
                if (asize >= (size_t)(data->bits >> 3)) {
                    /* Support address space wrapping for low-bit modes */
                    data->flags &= ~OUT_SIGNMASK;
                }
                warn_overflow_out(addrval, asize, data->flags);
                xdata.q = cpu_to_le64(addrval);
                data->data = xdata.b;
                data->type = OUT_RAWDATA;
                asize = amax = 0;   /* No longer an address */
            }
        } else {
            /*!
             *!reloc-abs-byte [off] 8-bit absolute section-crossing relocation
             *!  warns that an 8-bit absolute relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-abs-word [off] 16-bit absolute section-crossing relocation
             *!  warns that a 16-bit absolute relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-abs-dword [off] 32-bit absolute section-crossing relocation
             *!  warns that a 32-bit absolute relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-abs-qword [off] 64-bit absolute section-crossing relocation
             *!  warns that a 64-bit absolute relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-rel-byte [off] 8-bit relative section-crossing relocation
             *!  warns that an 8-bit relative relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-rel-word [off] 16-bit relative section-crossing relocation
             *!  warns that a 16-bit relative relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-rel-dword [off] 32-bit relative section-crossing relocation
             *!  warns that a 32-bit relative relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            /*!
             *!reloc-rel-qword [off] 64-bit relative section-crossing relocation
             *!  warns that an 64-bit relative relocation that could
             *!  not be resolved at assembly time was generated in
             *!  the output format.
             *!
             *!  This is usually normal, but may not be handled by all
             *!  possible target environments
             */
            int warn;
            const char *type;

            switch (data->type) {
            case OUT_ADDRESS:
                type = "absolute";
                switch (asize) {
                case 1: warn = WARN_RELOC_ABS_BYTE; break;
                case 2: warn = WARN_RELOC_ABS_WORD; break;
                case 4: warn = WARN_RELOC_ABS_DWORD; break;
                case 8: warn = WARN_RELOC_ABS_QWORD; break;
                default: panic();
                }
                break;
            case OUT_RELADDR:
                type = "relative";
                switch (asize) {
                case 1: warn = WARN_RELOC_REL_BYTE; break;
                case 2: warn = WARN_RELOC_REL_WORD; break;
                case 4: warn = WARN_RELOC_REL_DWORD; break;
                case 8: warn = WARN_RELOC_REL_QWORD; break;
                default: panic();
                }
                break;
            default:
                warn = 0;
            }

            if (warn) {
                nasm_warn(warn, "%u-bit %s section-crossing relocation",
                          (unsigned int)(asize << 3), type);
            }
        }
        break;

    case OUT_SEGMENT:
        nasm_assert(data->size <= 8);
        asize = data->size;
        amax = 2;
        break;

    default:
        asize = amax = 0;       /* Not an address */
        break;
    }

    /*
     * If the source location or output segment has changed,
     * let the debug backend know. Some backends really don't
     * like being given a NULL filename as can happen if we
     * use -Lb and expand a macro, so filter out that case.
     */
    data->where = src_where();
    if (data->where.filename &&
        (!src_location_same(data->where, dbg.where) |
         (data->loc.segment != dbg.segment))) {
        dbg.where   = data->where;
        dbg.segment = data->loc.segment;
        dfmt->linenum(dbg.where.filename, dbg.where.lineno, data->loc.segment);
    }

    if (asize > amax) {
        if (data->type == OUT_RELADDR || (data->flags & OUT_SIGNED)) {
            nasm_nonfatal("%u-bit signed relocation unsupported by output format %s",
                          (unsigned int)(asize << 3), ofmt->shortname);
        } else {
            /*!
             *!zext-reloc [on] relocation zero-extended to match output format
             *!  warns that a relocation has been zero-extended due
             *!  to limitations in the output format.
             */
            nasm_warn(WARN_ZEXT_RELOC,
                       "%u-bit %s relocation zero-extended from %u bits",
                       (unsigned int)(asize << 3),
                       data->type == OUT_SEGMENT ? "segment" : "unsigned",
                       (unsigned int)(amax << 3));
        }
        zeropad = data->size - amax;
        data->size = amax;
    }
    lfmt->output(data);

    if (likely(data->loc.segment != NO_SEG)) {
        /*
         * Collect macro-related information for the debugger, if applicable
         */
        if (debug_current_macro)
            debug_macro_out(data);

        ofmt->output(data);
    } else {
        /* Outputting to ABSOLUTE section - only reserve is permitted */
        if (data->type != OUT_RESERVE)
            nasm_nonfatal("attempt to assemble code in [ABSOLUTE] space");
        /* No need to push to the backend */
    }

    data->loc.offset  += data->size;
    data->insoffs     += data->size;

    if (zeropad) {
        data->type         = OUT_ZERODATA;
        data->size         = zeropad;
        lfmt->output(data);
        ofmt->output(data);
        data->loc.offset  += zeropad;
        data->insoffs     += zeropad;
        data->size        += zeropad;  /* Restore original size value */
    }
}

static inline void out_rawdata(struct out_data *data, const void *rawdata,
                               size_t size)
{
    data->type = OUT_RAWDATA;
    data->data = rawdata;
    data->size = size;
    out(data);
}

static void out_rawbyte(struct out_data *data, uint8_t byte)
{
    data->type = OUT_RAWDATA;
    data->data = &byte;
    data->size = 1;
    out(data);
}

static void out_rawword(struct out_data *data, uint16_t value)
{
    uint16_t buf = cpu_to_le16(value);
    data->type = OUT_RAWDATA;
    data->data = &buf;
    data->size = 2;
    out(data);
}

static void out_rawdword(struct out_data *data, uint32_t value)
{
    uint32_t buf = cpu_to_le32(value);
    data->type = OUT_RAWDATA;
    data->data = &buf;
    data->size = 4;
    out(data);
}

static inline void out_reserve(struct out_data *data, uint64_t size)
{
    data->type = OUT_RESERVE;
    data->size = size;
    out(data);
}

static void out_segment(struct out_data *data, const struct operand *opx)
{
    if (opx->opflags & OPFLAG_RELATIVE)
        nasm_nonfatal("segment references cannot be relative");

    data->type      = OUT_SEGMENT;
    data->flags     = OUT_UNSIGNED;
    data->size      = 2;
    data->toffset   = opx->offset;
    data->tsegment  = ofmt->segbase(opx->segment | 1);
    data->twrt      = opx->wrt;
    out(data);
}

static void out_imm(struct out_data *data, const struct operand *opx,
                    int size, enum out_flags sign)
{
    if (opx->segment != NO_SEG && (opx->segment & 1)) {
        /*
         * This is actually a segment reference, but eval() has
         * already called ofmt->segbase() for us.  Sigh.
         */
        if (size < 2)
            nasm_nonfatal("segment reference must be 16 bits");

        data->type = OUT_SEGMENT;
    } else {
        data->type = (opx->opflags & OPFLAG_RELATIVE)
            ? OUT_RELADDR : OUT_ADDRESS;
    }
    data->flags    = sign;
    data->toffset  = opx->offset;
    data->tsegment = opx->segment;
    data->twrt     = opx->wrt;
    /*
     * XXX: improve this if at some point in the future we can
     * distinguish the subtrahend in expressions like [foo - bar]
     * where bar is a symbol in the current segment.  However, at the
     * current point, if OPFLAG_RELATIVE is set that subtraction has
     * already occurred.
     */
    data->relbase = 0;
    data->size = size;
    out(data);
}

static void out_reladdr(struct out_data *data, const struct operand *opx,
                        int size)
{
    if (opx->opflags & OPFLAG_RELATIVE)
        nasm_nonfatal("invalid use of self-relative expression");

    data->type     = OUT_RELADDR;
    data->flags    = OUT_SIGNED;
    data->size     = size;
    data->toffset  = opx->offset;
    data->tsegment = opx->segment;
    data->twrt     = opx->wrt;
    data->relbase  = data->loc.offset + (data->inslen - data->insoffs);
    out(data);
}

static bool jmp_match(insn * ins, const struct itemplate *temp)
{
    int64_t isize;
    const uint8_t *code = temp->code;
    uint8_t c = code[0];
    bool is_byte;
    const struct operand * const op0 = get_operand(ins, 0);

    if (op0->type & STRICT)
        return false;

    switch (c) {
    case 0370:
        if (ins->opt & OPTIM_NO_Jcc_RELAX)
            return false;
        break;
    case 0371:
        if (ins->opt & OPTIM_NO_JMP_RELAX)
            return false;
        break;
    default:
        return false;
    }

    isize = calcsize(ins, temp);

    if (op0->opflags & OPFLAG_UNKNOWN)
        /* Be optimistic in pass 1 */
        return true;

    if (op0->segment != ins->loc.segment)
        return false;

    isize = op0->offset - ins->loc.offset - isize; /* isize is delta */
    is_byte = (isize >= -128 && isize <= 127); /* is it byte size? */

    if (is_byte && c == 0371 && ins->prefixes[PPS_REP] == P_BND) {
        /* jmp short (opcode eb) cannot be used with bnd prefix. */
        ins->prefixes[PPS_REP] = P_none;
        /*!
         *!prefix-bnd [on] invalid \c{BND} prefix
         *!=bnd
         *!  warns about ineffective use of the \c{BND} prefix when the
         *!  \c{JMP} instruction is converted to the \c{SHORT} form.
         *!  This should be extremely rare since the short \c{JMP} only
         *!  is applicable to jumps inside the same module, but if
         *!  it is legitimate, it may be necessary to use
         *!  \c{bnd jmp dword}.
         */
        nasm_warn(WARN_PREFIX_BND|ERR_PASS2 ,
                   "jmp short does not init bnd regs - bnd prefix dropped");
    }

    return is_byte;
}

static inline int64_t merge_resb(insn *ins, int64_t isize)
{
    int nbytes = resb_bytes(ins->opcode);

    if (likely(!nbytes))
        return isize;

    if (isize != nbytes * ins->oprs[0].offset)
        return isize;           /* Has prefixes of some sort */

    ins->oprs[0].offset *= ins->times;
    isize *= ins->times;
    ins->times = 1;
    return isize;
}

/* This must be handle non-power-of-2 alignment values */
static inline size_t pad_bytes(size_t len, size_t align)
{
    size_t partial = len % align;
    return partial ? align - partial : 0;
}

static void out_eops(struct out_data *data, const extop *e)
{
    while (e) {
        size_t dup = e->dup;

        switch (e->type) {
        case EOT_NOTHING:
            break;

        case EOT_EXTOP:
            while (dup--)
                out_eops(data, e->val.subexpr);
            break;

        case EOT_DB_NUMBER:
            if (e->elem > 8) {
                nasm_nonfatal("integer supplied as %d-bit data",
                              e->elem << 3);
            } else {
                while (dup--) {
                    data->insoffs = 0;
                    data->inslen = data->size = e->elem;
                    data->tsegment = e->val.num.segment;
                    data->toffset  = e->val.num.offset;
                    data->twrt = e->val.num.wrt;
                    data->relbase = 0;
                    if (e->val.num.segment != NO_SEG &&
                        (e->val.num.segment & 1)) {
                        data->type  = OUT_SEGMENT;
                        data->flags = OUT_UNSIGNED;
                    } else {
                        data->type = e->val.num.relative
                            ? OUT_RELADDR : OUT_ADDRESS;
                        data->flags = OUT_WRAP;
                    }
                    out(data);
                }
            }
            break;

        case EOT_DB_FLOAT:
        case EOT_DB_STRING:
        case EOT_DB_STRING_FREE:
        {
            size_t pad, len;

            pad = pad_bytes(e->val.string.len, e->elem);
            len = e->val.string.len + pad;

            while (dup--) {
                data->insoffs = 0;
                data->inslen = len;
                out_rawdata(data, e->val.string.data, e->val.string.len);
                if (pad)
                    out_rawdata(data, zero_buffer, pad);
            }
            break;
        }

        case EOT_DB_RESERVE:
            data->insoffs = 0;
            data->inslen = dup * e->elem;
            out_reserve(data, data->inslen);
            break;
        }

        e = e->next;
    }
}

/* This is totally just a wild guess what is reasonable... */
#define INCBIN_MAX_BUF (ZERO_BUF_SIZE * 16)

int64_t assemble(insn *instruction)
{
    struct out_data data;
    const struct itemplate *temp;
    enum match_result m;
    const int64_t start = instruction->loc.offset;
    const int bits = instruction->bits;

    if (instruction->opcode == I_none)
        return 0;

    nasm_zero(data);
    data.loc   = instruction->loc;
    data.bits  = instruction->bits;

    if (opcode_is_db(instruction->opcode)) {
        out_eops(&data, instruction->eops);
    } else if (instruction->opcode == I_INCBIN) {
        const char *fname = instruction->eops->val.string.data;
        FILE *fp;
        size_t t = instruction->times; /* INCBIN handles TIMES by itself */
        off_t base = 0;
        off_t len;
        const void *map = NULL;
        char *buf = NULL;
        size_t blk = 0;         /* Buffered I/O block size */
        size_t m = 0;           /* Bytes last read */

        if (!t)
            goto done;

        fp = nasm_open_read(fname, NF_BINARY|NF_FORMAP);
        if (!fp) {
            nasm_nonfatal("`incbin': unable to open file `%s'",
                          fname);
            goto done;
        }

        len = nasm_file_size(fp);

        if (len == (off_t)-1) {
            nasm_nonfatal("`incbin': unable to get length of file `%s'",
                          fname);
            goto close_done;
        }

        if (instruction->eops->next) {
            base = instruction->eops->next->val.num.offset;
            if (base >= len) {
                len = 0;
            } else {
                len -= base;
                if (instruction->eops->next->next &&
                    len > (off_t)instruction->eops->next->next->val.num.offset)
                    len = (off_t)instruction->eops->next->next->val.num.offset;
            }
        }

        lfmt->set_offset(data.loc.offset);
        lfmt->uplevel(LIST_INCBIN, len);

        if (!len)
            goto end_incbin;

        /* Try to map file data */
        map = nasm_map_file(fp, base, len);
        if (!map) {
            blk = len < (off_t)INCBIN_MAX_BUF ? (size_t)len : INCBIN_MAX_BUF;
            buf = nasm_malloc(blk);
        }

        while (t--) {
            /*
             * Consider these irrelevant for INCBIN, since it is fully
             * possible that these might be (way) bigger than an int
             * can hold; there is, however, no reason to widen these
             * types just for INCBIN.  data.inslen == 0 signals to the
             * backend that these fields are meaningless, if at all
             * needed.
             */
            data.insoffs = 0;
            data.inslen = 0;

            if (map) {
                out_rawdata(&data, map, len);
            } else if ((off_t)m == len) {
                out_rawdata(&data, buf, len);
            } else {
                off_t l = len;

                if (fseeko(fp, base, SEEK_SET) < 0 || ferror(fp)) {
                    nasm_nonfatal("`incbin': unable to seek on file `%s'",
                                  fname);
                    goto end_incbin;
                }
                while (l > 0) {
                    m = fread(buf, 1, l < (off_t)blk ? (size_t)l : blk, fp);
                    if (!m || feof(fp)) {
                        /*
                         * This shouldn't happen unless the file
                         * actually changes while we are reading
                         * it.
                         */
                        nasm_nonfatal("`incbin': unexpected EOF while"
                                      " reading file `%s'", fname);
                        goto end_incbin;
                    }
                    out_rawdata(&data, buf, m);
                    l -= m;
                }
            }
        }
    end_incbin:
        lfmt->downlevel(LIST_INCBIN);
        if (instruction->times > 1) {
            lfmt->uplevel(LIST_TIMES, instruction->times);
            lfmt->downlevel(LIST_TIMES);
        }
        if (ferror(fp)) {
            nasm_nonfatal("`incbin': error while"
                          " reading file `%s'", fname);
        }
    close_done:
        if (buf)
            nasm_free(buf);
        if (map)
            nasm_unmap_file(map, len);
        fclose(fp);
    done:
        instruction->times = 1; /* Tell the upper layer not to iterate */
        ;
    } else {
        /* "Real" instruction */

        /* Check to see if we need an address-size prefix */
        add_asp(instruction);

        m = find_match(&temp, instruction);

        if (m == MOK_GOOD) {
            /* Matches! */
            if (unlikely(itemp_has(temp, IF_OBSOLETE))) {
                errflags warning;
                const char *whathappened;
                const char *validity;
                bool never = itemp_has(temp, IF_NEVER);

                /*
                 * If IF_OBSOLETE is set, warn the user. Different
                 * warning classes for "obsolete but valid for this
                 * specific CPU" and "obsolete and gone."
                 *
                 *!obsolete-removed [on] instruction obsolete and removed on the target CPU
                 *!  warns for an instruction which has been removed
                 *!  from the architecture, and is no longer included
                 *!  in the CPU definition given in the \c{[CPU]}
                 *!  directive, for example \c{POP CS}, the opcode for
                 *!  which, \c{0Fh}, instead is an opcode prefix on
                 *!  CPUs newer than the first generation 8086.
                 *
                 *!obsolete-nop [on] instruction obsolete and is a noop on the target CPU
                 *!  warns for an instruction which has been removed
                 *!  from the architecture, but has been architecturally
                 *!  defined to be a noop for future CPUs.
                 *
                 *!obsolete-valid [on] instruction obsolete but valid on the target CPU
                 *!  warns for an instruction which has been removed
                 *!  from the architecture, but is still valid on the
                 *!  specific CPU given in the \c{CPU} directive. Code
                 *!  using these instructions is most likely not
                 *!  forward compatible.
                 */

                whathappened = never ? "never implemented" : "obsolete";

                if (!never && !iflag_cmp_cpu_level(&insns_flags[temp->iflag_idx], &cpu)) {
                    warning = WARN_OBSOLETE_VALID;
                    validity = "but valid on";
                } else if (itemp_has(temp, IF_NOP)) {
                    warning = WARN_OBSOLETE_NOP;
                    validity = "and is a noop on";
                } else {
                    warning = WARN_OBSOLETE_REMOVED;
                    validity = never ? "and invalid on" : "and removed from";
                }

                nasm_warn(warning, "instruction %s %s the target CPU",
                          whathappened, validity);
            }

            data.inslen = calcsize(instruction, temp);
            nasm_assert(data.inslen >= 0);
            data.inslen = merge_resb(instruction, data.inslen);

            data.insoffs = 0;
            gencode(&data, instruction);
            if (unlikely(data.insoffs != data.inslen)) {
                nasm_nonfatal("instruction length changed during code generation: %u -> %u",
                           (unsigned int)data.insoffs, (unsigned int)data.inslen);
            }
        } else {
            /* No match */
            switch (m) {
            case MERR_OPSIZEMISSING:
                nasm_nonfatal("operation size not specified");
                break;
            case MERR_OPSIZEMISMATCH:
                nasm_nonfatal("mismatch in operand sizes");
                break;
            case MERR_BRNOTHERE:
                nasm_nonfatal("broadcast not permitted on this operand");
                break;
            case MERR_BRNUMMISMATCH:
                nasm_nonfatal("mismatch in the number of broadcasting elements");
                break;
            case MERR_MASKNOTHERE:
                nasm_nonfatal("mask not permitted on this operand");
                break;
            case MERR_DECONOTHERE:
                nasm_nonfatal("unsupported mode decorator for instruction");
                break;
            case MERR_BADCPU:
                nasm_nonfatal("no instruction for this cpu level");
                break;
            case MERR_BADMODE:
                nasm_nonfatal("instruction not supported in %d-bit mode", bits);
                break;
            case MERR_ENCMISMATCH:
                if (!instruction->prefixes[PPS_REX]) {
                    nasm_nonfatal("instruction not encodable without explicit prefix");
                } else {
                    nasm_nonfatal("instruction not encodable with %s prefix",
                                  prefix_name(instruction->prefixes[PPS_REX]));
                }
                break;
            case MERR_BADBND:
            case MERR_BADREPNE:
                nasm_nonfatal("%s prefix is not allowed",
                              prefix_name(instruction->prefixes[PPS_REP]));
                break;
            case MERR_REGSETSIZE:
                nasm_nonfatal("invalid register set size");
                break;
            case MERR_REGSET:
                nasm_nonfatal("register set not valid for operand");
                break;
            case MERR_WRONGIMM:
                nasm_nonfatal("operand/operator invalid for this instruction");
                break;
            case MERR_BADZU:
                nasm_nonfatal("{zu} not applicable to this instruction");
                break;
            case MERR_MEMZU:
                nasm_nonfatal("{zu} invalid for non-register destination");
                break;
            case MERR_BADNF:
                nasm_nonfatal("{nf} not available for this instruction");
                break;
            case MERR_REQNF:
                nasm_nonfatal("{nf} required for this instruction");
                break;
            default:
                nasm_nonfatal("invalid combination of opcode and operands");
                break;
            }

            instruction->times = 1; /* Avoid repeated error messages */
        }
    }
    return data.loc.offset - start;
}

static int32_t eops_typeinfo(const extop *e)
{
    int32_t typeinfo = 0;

    while (e) {
        switch (e->type) {
        case EOT_NOTHING:
            break;

        case EOT_EXTOP:
            typeinfo |= eops_typeinfo(e->val.subexpr);
            break;

        case EOT_DB_FLOAT:
            switch (e->elem) {
            case  1: typeinfo |= TY_BYTE;  break;
            case  2: typeinfo |= TY_WORD;  break;
            case  4: typeinfo |= TY_FLOAT; break;
            case  8: typeinfo |= TY_QWORD; break; /* double? */
            case 10: typeinfo |= TY_TBYTE; break; /* long double? */
            case 16: typeinfo |= TY_YWORD; break;
            case 32: typeinfo |= TY_ZWORD; break;
            default: break;
            }
            break;

        default:
            switch (e->elem) {
            case  1: typeinfo |= TY_BYTE;  break;
            case  2: typeinfo |= TY_WORD;  break;
            case  4: typeinfo |= TY_DWORD; break;
            case  8: typeinfo |= TY_QWORD; break;
            case 10: typeinfo |= TY_TBYTE; break;
            case 16: typeinfo |= TY_YWORD; break;
            case 32: typeinfo |= TY_ZWORD; break;
            default: break;
            }
            break;
        }
        e = e->next;
    }

    return typeinfo;
}

static inline void debug_set_db_type(insn *instruction)
{

    int32_t typeinfo = TYS_ELEMENTS(instruction->operands);

    typeinfo |= eops_typeinfo(instruction->eops);
    dfmt->debug_typevalue(typeinfo);
}

static void debug_set_type(insn *instruction)
{
    int32_t typeinfo;

    if (opcode_is_resb(instruction->opcode)) {
        typeinfo = TYS_ELEMENTS(instruction->oprs[0].offset);

        switch (instruction->opcode) {
        case I_RESB:
            typeinfo |= TY_BYTE;
            break;
        case I_RESW:
            typeinfo |= TY_WORD;
            break;
        case I_RESD:
            typeinfo |= TY_DWORD;
            break;
        case I_RESQ:
            typeinfo |= TY_QWORD;
            break;
        case I_REST:
            typeinfo |= TY_TBYTE;
            break;
        case I_RESO:
            typeinfo |= TY_OWORD;
            break;
        case I_RESY:
            typeinfo |= TY_YWORD;
            break;
        case I_RESZ:
            typeinfo |= TY_ZWORD;
            break;
        default:
            panic();
        }
    } else {
        typeinfo = TY_LABEL;
    }

    dfmt->debug_typevalue(typeinfo);
}


/* Proecess an EQU directive */
static void define_equ(insn * instruction)
{
    if (!instruction->label) {
        nasm_nonfatal("EQU not preceded by label");
    } else if (instruction->operands == 1 &&
               (instruction->oprs[0].type & IMMEDIATE) &&
               instruction->oprs[0].wrt == NO_SEG) {
        define_label(instruction->label,
                     instruction->oprs[0].segment,
                     instruction->oprs[0].offset, false);
    } else if (instruction->operands == 2
               && (instruction->oprs[0].type & IMMEDIATE)
               && (instruction->oprs[0].type & COLON)
               && instruction->oprs[0].segment == NO_SEG
               && instruction->oprs[0].wrt == NO_SEG
               && (instruction->oprs[1].type & IMMEDIATE)
               && instruction->oprs[1].segment == NO_SEG
               && instruction->oprs[1].wrt == NO_SEG) {
        define_label(instruction->label,
                     instruction->oprs[0].offset | SEG_ABS,
                     instruction->oprs[1].offset, false);
    } else {
        nasm_nonfatal("bad syntax for EQU");
    }
}

static int64_t len_extops(const extop *e)
{
    int64_t isize = 0;
    size_t pad;

    while (e) {
        switch (e->type) {
        case EOT_NOTHING:
            break;

        case EOT_EXTOP:
            isize += e->dup * len_extops(e->val.subexpr);
            break;

        case EOT_DB_STRING:
        case EOT_DB_STRING_FREE:
        case EOT_DB_FLOAT:
            pad = pad_bytes(e->val.string.len, e->elem);
            isize += e->dup * (e->val.string.len + pad);
            break;

        case EOT_DB_NUMBER:
            warn_overflow_const(e->val.num.offset, e->elem);
            isize += e->dup * e->elem;
            break;

        case EOT_DB_RESERVE:
            isize += e->dup * e->elem;
            break;
        }

        e = e->next;
    }

    return isize;
}

int64_t insn_size(insn *instruction)
{
    const struct itemplate *temp;
    enum match_result m;
    int64_t isize = 0;

    if (instruction->opcode == I_none) {
        return 0;
    } else if (instruction->opcode == I_EQU) {
        define_equ(instruction);
        return 0;
    } else if (opcode_is_db(instruction->opcode)) {
        isize = len_extops(instruction->eops);
        debug_set_db_type(instruction);
        return isize;
    } else if (instruction->opcode == I_INCBIN) {
        const extop *e = instruction->eops;
        const char *fname = e->val.string.data;
        off_t len;

        len = nasm_file_size_by_path(fname);
        if (len == (off_t)-1) {
            nasm_nonfatal("`incbin': unable to get length of file `%s'",
                          fname);
            return 0;
        }

        e = e->next;
        if (e) {
            if (len <= (off_t)e->val.num.offset) {
                len = 0;
            } else {
                len -= e->val.num.offset;
                e = e->next;
                if (e && len > (off_t)e->val.num.offset) {
                    len = (off_t)e->val.num.offset;
                }
            }
        }

        len *= instruction->times;
        instruction->times = 1; /* Tell the upper layer to not iterate */

        return len;
    } else {
        /* Normal instruction, or RESx */

        /* Check to see if we need an address-size prefix */
        add_asp(instruction);

        m = find_match(&temp, instruction);
        if (m != MOK_GOOD)
            return -1;              /* No match */

        isize = calcsize(instruction, temp);
        debug_set_type(instruction);
        isize = merge_resb(instruction, isize);

        return isize;
    }
}

static void bad_hle_warn(const insn * ins, uint8_t hleok)
{
    enum prefixes rep_pfx = ins->prefixes[PPS_REP];
    enum whatwarn { w_none, w_lock, w_inval } ww;
    static const enum whatwarn warn[2][4] =
    {
        { w_inval, w_inval, w_none, w_lock }, /* XACQUIRE */
        { w_inval, w_none,  w_none, w_lock }, /* XRELEASE */
    };
    unsigned int n;

    n = (unsigned int)rep_pfx - P_XACQUIRE;
    if (n > 1)
        return;                 /* Not XACQUIRE/XRELEASE */

    ww = warn[n][hleok];
    if (!is_class(MEMORY, ins->oprs[0].type))
        ww = w_inval;           /* HLE requires operand 0 to be memory */

    /*!
     *!prefix-hle [on] invalid HLE prefix
     *!=hle
     *!  warns about invalid use of the HLE \c{XACQUIRE} or \c{XRELEASE}
     *!  prefixes.
     */
    switch (ww) {
    case w_none:
        break;

    case w_lock:
        if (ins->prefixes[PPS_LOCK] != P_LOCK) {
            nasm_warn(WARN_PREFIX_HLE|ERR_PASS2,
                       "%s with this instruction requires lock",
                       prefix_name(rep_pfx));
        }
        break;

    case w_inval:
        nasm_warn(WARN_PREFIX_HLE|ERR_PASS2,
                   "%s invalid with this instruction",
                   prefix_name(rep_pfx));
        break;
    }
}

static int ea_evex_err(decoflags_t deco, unsigned int vlen, const char *why)
{
    const char *what = deco & ER
        ? "embedded rounding" : "suppress all exceptions";
    nasm_nonfatal("%s not possible for %d-bit vectors%s",
                  what, 128 << vlen, why);
    return -1;
}

/* Handle EVEX flags at the time of EA processing */
static int ea_evex_flags(insn *ins, const struct operand *opy)
{
    /* EVEX.b1 : evex_brerop contains the operand position */
    const struct operand *op_er_sae = ins->evex_brerop;
    const decoflags_t deco = op_er_sae ? op_er_sae->decoflags : 0;

    if (deco & (ER | SAE)) {
        if (!itemp_has(ins->itemp, IF_LIG)) {
            const unsigned int vlen = (ins->evex & EVEX_P2RC) >> 29;
            const char *why = "";

            switch (vlen) {
            case 2:
                /* 512 bits, no special encoding */
                break;
            case 1:
                /* 256 bits, encodable in AVX 10.2 if mod=3 */
                if (!iflag_test(&cpu, IF_AVX10_2)) {
                    why = " without AVX 10.2";
                } else {
                    ins->evex ^= EVEX_P1U;
                    break;
                }
                /* else fall through */
            default:
                /* Not encodable */
                return ea_evex_err(deco, vlen, why);
            }
        }

        ins->evex &= ~EVEX_P2RC;
        ins->evex ^= EVEX_P2B;
        if (op_er_sae->decoflags & ER) {
            /* set EVEX.RC (rounding control) */
            ins->evex ^= ((ins->evex_rm - BRC_RN) << 29) & EVEX_P2RC;
        }
    } else if (opy->decoflags & BRDCAST_MASK) {
        /* set EVEX.b but don't EVEX.L/RC */
        ins->evex ^= EVEX_P2B;
    }

    return 0;
}

static int ea_evex_post_check(const insn *ins)
{
    const struct operand *op_er_sae = ins->evex_brerop;
    const decoflags_t deco = op_er_sae ? op_er_sae->decoflags : 0;

    if ((deco & (ER | SAE)) && !itemp_has(ins->itemp, IF_LIG)) {
        const unsigned int vlen = (ins->evex & EVEX_P2RC) >> 29;

        if (vlen == 1 && (ins->ea.modrm >> 6) != 3) {
            return ea_evex_err(deco, vlen, " when accessing memory");
        }
    }

    return 0;
}

/* Common construct */
#define case3(x) case (x): case (x)+1: case (x)+2
#define case4(x) case3(x): case (x)+3

static int64_t calcsize(insn *ins, const struct itemplate * const temp)
{
    const int bits = ins->bits;
    const uint8_t *codes = temp->code;
    int64_t length = 0;
    uint8_t c;
    int op1, op2;
    struct operand *opx, *opy;
    uint8_t opex = 0;
    enum ea_type eat;
    uint8_t hleok = 0;
    bool lockcheck = true;
    enum reg_enum mib_index = R_none;   /* For a separate index reg form */
    const char *errmsg;
    int need_byte;
    enum prefixes need_pfx;

    ins->rex     = 0;           /* Ensure REX is reset */
    ins->evex    = 0;		/* Ensure EVEX is reset */
    ins->vexreg  = 0;           /* No V register */
    ins->vex_cm  = 0;           /* No implicit map */
    ins->bits    = bits;        /* Execution mode (default asize) */
    ins->itemp   = temp;        /* Instruction template */
    eat = EA_SCALAR;            /* Expect a scalar EA */

    /* Default operand size */
    ins->op_size = bits != 16 ? 32 : 16;

    while (*codes) {
        c = *codes++;
        op1 = (c & 3) + ((opex & 1) << 2);
        opx = get_operand(ins, op1);
        op2 = ((c >> 3) & 3) + ((opex & 2) << 1);
        opy = NULL;
        opex = 0;               /* For the next iteration */

        switch (c) {
        case4(01):
            codes += c, length += c;
            break;

        case3(05):
            opex = c;
            break;

        case4(010):
            ins->rex |= op_rexflags(opx, REX_rB);
            codes++, length++;
            break;

        case4(014):
            /* this is an index reg of a split SIB operand */
            mib_index = opx->basereg;
            break;

        case4(020):
        case4(024):
            length++;
            break;

        case4(030):
            length += 2;
            break;

        case4(034):
            if (opx->type & (BITS16 | BITS32 | BITS64))
                length += (opx->type & BITS16) ? 2 : 4;
            else
                length += (bits == 16) ? 2 : 4;
            break;

        case4(040):
            length += 4;
            break;

        case4(044):
            length += ins->addr_size >> 3;
            break;

        case4(050):
            length++;
            break;

        case4(054):
            length += 8; /* MOV reg64/imm */
            break;

        case4(060):
            length += 2;
            break;

        case4(064):
            if (opx->type & (BITS16 | BITS32 | BITS64))
                length += (opx->type & BITS16) ? 2 : 4;
            else
                length += (bits == 16) ? 2 : 4;
            break;

        case4(070):
            length += 4;
            break;

        case4(074):
            length += 2;
            break;

        case 0171:
            c = *codes++;
            op2 = (op2 & ~3) | ((c >> 3) & 3);
            opy = get_operand(ins, op2);
            ins->rex |= op_rexflags(opy, REX_rR);
            length++;
            break;

        case 0172:
        case 0173:
            codes++;
            length++;
            break;

        case4(0174):
            length++;
            break;

        case4(0240):
            ins->vexreg = regval(opx);
            goto evex_common;

        case 0250:
            ins->vexreg = 0;
            goto evex_common;

        evex_common:
            ins->rex |= REX_EV;
            ins->evex = 0x62;
            ins->evex += *codes++ << 8;
            ins->evex += *codes++ << 16;
            ins->evex += *codes++ << 24;
            ins->evex_tuple = (*codes++ - 0300);
            ins->vex_cm = ((ins->evex >> 8) & 7) | (RV_EVEX << 6);
            break;

        case4(0254):
            length += 4;
            break;

        case4(0260):
            ins->vexreg = regval(opx);
            goto vex_common;

        case 0270:
            ins->vexreg = 0;
            goto vex_common;

        vex_common:
            ins->rex |= REX_V;
            ins->vex_cm  = *codes++;
            ins->vex_wlp = *codes++;
            break;

        case3(0271):
            hleok = c & 3;
            break;

        case4(0274):
            length++;
            break;

        case4(0300):
            break;

        case 0310:
        {
            enum prefixes pfx = ins->prefixes[PPS_ASIZE];

            ins->addr_size = 16;

            if (bits == 64)
                return -1;
            if (pfx) {
                if (!(pfx == P_A16 || (bits == 32 && pfx == P_ASP)))
                    return -1;
            } else {
                ins->prefixes[PPS_ASIZE] = P_A16;
            }
            break;
        }

        case 0311:
        {
            enum prefixes pfx = ins->prefixes[PPS_ASIZE];

            ins->addr_size = 32;

            if (pfx) {
                if (!(pfx == P_A32 || (bits != 32 && pfx == P_ASP)))
                    return -1;
            } else {
                ins->prefixes[PPS_ASIZE] = P_A32;
            }
            break;
        }

        case 0312:
            break;

        case 0313:
        {
            enum prefixes pfx = ins->prefixes[PPS_ASIZE];

            ins->addr_size = 64;

            if (bits != 64 || (pfx && pfx != P_A64))
                return -1;
            break;
        }

        case4(0314):
            break;

        case 0320:
        {
            /*! prefix-opsize [on] invalid operand size prefix
             *!   warns that an operand prefix (\c{o16}, \c{o32}, \c{o64},
             *!   \c{osp}) invalid for the specified instruction has been specified.
             *!   The operand prefix will be ignored by the assembler.
             */
            enum prefixes pfx = ins->prefixes[PPS_OSIZE];
            ins->op_size = 16;
            if (bits != 16 && pfx == P_OSP) {
                /* Allow osp prefix as is */
            } else if (pfx != P_none && pfx != P_O16) {
                nasm_warn(WARN_PREFIX_OPSIZE|ERR_PASS2,
                          "invalid operand size prefix %s, must be o16",
                          prefix_name(pfx));
            } else {
                ins->prefixes[PPS_OSIZE] = P_O16;
            }
            break;
        }

        case 0321:
        {
            enum prefixes pfx = ins->prefixes[PPS_OSIZE];
            ins->op_size = 32;
            if (bits == 16 && pfx == P_OSP) {
                /* Allow osp prefix as is */
            } else if (pfx != P_none && pfx != P_O32) {
                nasm_warn(WARN_PREFIX_OPSIZE|ERR_PASS2,
                          "invalid operand size prefix %s, must be o32",
                          prefix_name(pfx));
            } else {
                ins->prefixes[PPS_OSIZE] = P_O32;
            }
            break;
        }

        case 0322:
            break;

        case 0327:
            if (bits == 64)
                goto do_o64;
            break;

        case 0324:
            ins->rex |= REX_W;
            goto do_o64;

        case 0323:
        do_o64:
        {
            enum prefixes pfx = ins->prefixes[PPS_OSIZE];
            ins->op_size = 64;
            if (pfx == P_OSP) {
                /* Ignore operand size prefix */
            } else if (pfx != P_none && pfx != P_O64) {
                nasm_warn(WARN_PREFIX_OPSIZE|ERR_PASS2,
                          "invalid operand size prefix %s, must be o64",
                          prefix_name(pfx));
            } else {
                ins->prefixes[PPS_OSIZE] = P_O64;
            }
            break;
        }

        case 0325:
            ins->rex |= REX_NH;
            break;

        case 0326:
            break;

        case 0330:
            /* The actual prefixes are generated elsewhere */
            if (bits == 64 && ins->prefixes[PPS_OSIZE] == P_O64)
                ins->rex |= REX_W;
            break;

        case 0331:
            need_byte = PFE_NULL;
            need_pfx  = P_none;
            goto check_rep;

        case 0332:
            need_byte = 0xf2;
            need_pfx  = P_REPNE;
            goto check_rep;

        case 0333:
            need_byte = 0xf3;
            need_pfx  = P_REP;
            goto check_rep;

        check_rep:
        {
            const enum prefixes pfx = ins->prefixes[PPS_REP];
            const int byte = prefix_byte(pfx, bits);
            if (byte != need_byte) {
                if (pfx) {
                    nasm_warn(ERR_PASS2,
                              "%s prefix invalid for this instruction",
                              prefix_name(pfx));
                } else {
                    ins->prefixes[PPS_REP] = need_pfx;
                }
            }
            break;
        }

        case 0334:
            ins->rex |= REX_L;
            break;

        case 0335:
            break;

        case 0340:
            /*!
             *!forward [on] forward reference may have unpredictable results
             *!  warns that a forward reference is used which may have
             *!  unpredictable results, notably in a \c{RESB}-type
             *!  pseudo-instruction. These would be \i\e{critical
             *!  expressions} (see \k{crit}) but are permitted in a
             *!  handful of cases for compatibility with older
             *!  versions of NASM. This warning should be treated as a
             *!  severe programming error as the code could break at
             *!  any time for any number of reasons.
             */
            /* The bytecode ends in 0, so opx points to operand 0 */
            if (!absolute_op(opx))
                nasm_nonfatal("attempt to reserve non-constant"
                              " quantity of BSS space");
            else if (opx->opflags & OPFLAG_FORWARD)
                nasm_warn(WARN_FORWARD, "forward reference in RESx "
                           "can have unpredictable results");
            else
                length += opx->offset * resb_bytes(ins->opcode);
            break;

        case 0341:
            if (!ins->prefixes[PPS_WAIT])
                ins->prefixes[PPS_WAIT] = P_WAIT;
            break;

        case 0344:
            ins->rex |= REX_P | REX_B;
            break;

        case 0345:
            ins->rex |= REX_P | REX_X;
            break;

        case 0346:
            ins->rex |= REX_P | REX_R;
            break;

        case 0347:
            ins->rex |= REX_P | REX_W;
            break;

        case 0350:
            ins->rex |= REX_P | REX_2;
            break;

        case 0351:
            ins->rex |= REX_P | REX_2 | REX_X1;
            break;

        case3(0355):
            /* Set opmap for legacy and REX2 encodings */
            ins->vex_cm = c & 3;
            break;

        case 0360:
            break;

        case 0361:
            length++;
            break;

        case 0364:
        case 0365:
            break;

        case 0366:
        case 0367:
            length++;
            break;

        case 0370:
        case 0371:
            break;

        case 0373:
            length++;
            break;

        case 0374:
            eat = EA_XMMVSIB;
            break;

        case 0375:
            eat = EA_YMMVSIB;
            break;

        case 0376:
            eat = EA_ZMMVSIB;
            break;

        case4(0100):
        case4(0110):
        case4(0120):
        case4(0130):
        case4(0200):
        case4(0204):
        case4(0210):
        case4(0214):
        case4(0220):
        case4(0224):
        case4(0230):
        case4(0234):
            {
                int rfield;
                opflags_t rflags;

                opy = get_operand(ins, op2);

                ins->ea.rex = 0;           /* Ensure ea.REX is initially 0 */

                if (c <= 0177) {
                    /* pick rfield from operand b (opx) */
                    rflags = regflag(opx);
                    rfield = nasm_regvals[opx->basereg];
                } else {
                    rflags = 0;
                    rfield = c & 7;
                    opx = NULL;
                }

                if (itemp_has(temp, IF_MIB)) {
                    opy->eaflags |= EAF_MIB;
                    /*
                     * if a separate form of MIB (ICC style) is used,
                     * the index reg info is merged into mem operand
                     */
                    if (mib_index != R_none) {
                        opy->indexreg = mib_index;
                        opy->scale = 1;
                        opy->hintbase = mib_index;
                        opy->hinttype = EAH_NOTBASE;
                    }
                }

                /* SIB encoding required */
                if (itemp_has(temp, IF_SIB))
                    opy->eaflags |= EAF_SIB;

                /* ea_evex_flags() must come before process_ea() */
                if (ea_evex_flags(ins, opy))
                    return -1;

                if (process_ea(opy, rfield, rflags, ins, eat, &errmsg)) {
                    nasm_nonfatal("%s", errmsg);
                    return -1;
                }

                ins->rex |= ins->ea.rex;
                length += ins->ea.size;

                if (ea_evex_post_check(ins))
                    return -1;
            }
            break;

        default:
            nasm_panic("internal instruction table corrupt"
                    ": instruction code \\%o (0x%02X) given", c, c);
            break;
        }
    }

    if (ins->rex & REX_NH) {
        if (ins->rex & REX_H) {
            nasm_nonfatal("instruction cannot use high registers");
            return -1;
        }
        ins->rex &= ~REX_P;        /* Don't force REX prefix due to high reg */
    }

    switch (ins->prefixes[PPS_REX]) {
    case P_EVEX:
        if (!(ins->rex & REX_EV))
            return -1;
        break;
    case P_VEX:
    case P_VEX3:
    case P_VEX2:
        if (!(ins->rex & REX_V))
            return -1;
        break;
    case P_REX:
        if (bits != 64) {
            nasm_nonfatal("REX encoding not supported in %d-bit mode", bits);
            return -1;
        }
        if (ins->rex & (REX_V|REX_EV|REX_2))
            return -1;
        ins->rex |= REX_P;      /* Force REX prefix */
        break;
    case P_REX2:
        if (bits != 64) {
            nasm_nonfatal("REX2 encoding not supported in %d-bit mode", bits);
            return -1;
        }
        if ((ins->rex & (REX_V|REX_EV)) || rex_highvec(ins->rex))
            return -1;
        ins->rex |= REX_P | REX_2; /* Force REX2 prefix */
        break;
    default:
        break;
    }

    if (ins->rex & (REX_V | REX_EV)) {
        uint32_t bad32 = REX_BXR;

        if (ins->rex & REX_H) {
            nasm_nonfatal("cannot use high byte register in this instruction");
            return -1;
        }
        if (itemp_has(temp, IF_WW)) {
            bad32 |= REX_W;
        } else {
            ins->rex = (ins->rex & ~REX_W) | ((ins->vex_wlp >> (7-3)) & REX_W);
        }

        if (bits != 64 && ((ins->rex & bad32) || ins->vexreg > 7)) {
            nasm_nonfatal("invalid operands in non-64-bit mode");
            return -1;
        }

        if (ins->rex & REX_EV) {
            /* EVEX */
            length += 4;
        } else {
            /* VEX */
            if (ins->vexreg > 15 || (ins->rex & REX_BXR1)) {
                nasm_nonfatal("invalid high-16 register in VEX encoded instruction");
                return -1;
            }

            if (ins->vex_cm != 1 ||
                (ins->rex & (REX_B|REX_X|REX_W)) ||
                ins->prefixes[PPS_REX] == P_VEX3) {
                /* VEX3 required */
                if (ins->prefixes[PPS_REX] == P_VEX2)
                    nasm_nonfatal("instruction not encodable with {vex2} prefix");
                length += 3;
            } else {
                /* VEX2 available */
                length += 2;
            }
        }
    } else if (ins->rex & (REX_BXR1 | REX_2)) {
        /* REX2 prefix needed */
        if (bits != 64) {
            nasm_nonfatal("invalid operands in %d-bit mode", bits);
            return -1;
        }
        if (!iflag_test(&cpu, IF_APX)) {
            nasm_nonfatal("invalid operands in non-APX mode");
            return -1;
        }
        if (itemp_has(temp, IF_NOAPX) || rex_highvec(ins->rex)) {
            nasm_nonfatal("this use of registers 16-31 not supported for this instruction");
            return -1;
        }
        if (ins->rex & REX_H) {
            nasm_nonfatal("cannot use high byte register in this instruction");
            return -1;
        }

        ins->rex |= REX_2 | REX_P;
        length += 2;
    } else if (ins->rex & REX_MASK) {
        if (ins->rex & REX_H) {
            nasm_nonfatal("cannot use high byte register in this instruction");
            return -1;
        } else if (bits == 64) {
            ins->rex &= ~REX_L;
            ins->rex |= REX_P;
        } else if ((ins->rex & (REX_L|REX_W|REX_BXR)) == (REX_L|REX_R) &&
                   iflag_cpu_level_ok(&cpu, IF_X86_64)) {
            /* LOCK-as-REX.R */
            if (assert_no_prefix(ins, PPS_LOCK))
                return -1;
            lockcheck = false;  /* Already errored, no need for warning */
            ins->rex &= ~REX_P;
        } else {
            nasm_nonfatal("invalid operands in %d-bit mode", bits);
            return -1;
        }
        length++;
    }

    if (!(ins->rex & (REX_2|REX_V|REX_EV))) {
        /* Opmap legacy encoding: none, 0f, 0f 38, 0f 3a */
        length += (ins->vex_cm > 0) + (ins->vex_cm > 1);
    }

    if (lockcheck && has_prefix(ins, PPS_LOCK, P_LOCK)) {
        if ((!itemp_has(temp,IF_LOCK)  || !is_class(MEMORY, ins->oprs[0].type)) &&
            (!itemp_has(temp,IF_LOCK1) || !is_class(MEMORY, ins->oprs[1].type))) {
            /*!
             *!prefix-lock-error [on] \c{LOCK} prefix on unlockable instruction
             *!=lock
             *!  warns about \c{LOCK} prefixes on unlockable instructions.
             */
            nasm_warn(WARN_PREFIX_LOCK_ERROR|ERR_PASS2 , "instruction is not lockable");
        } else if (temp->opcode == I_XCHG) {
            /*!
             *!prefix-lock-xchg [on] superfluous \c{LOCK} prefix on \c{XCHG} instruction
             *!  warns about a \c{LOCK} prefix added to an \c{XCHG} instruction.
             *!  The \c{XCHG} instruction is \e{always} locking, and so this
             *!  prefix is not necessary; however, NASM will generate it if
             *!  explicitly provided by the user, so this warning indicates that
             *!  suboptimal code is being generated.
             */
            nasm_warn(WARN_PREFIX_LOCK_XCHG|ERR_PASS2,
                      "superfluous LOCK prefix on XCHG instruction");
        }
    }

    bad_hle_warn(ins, hleok);

    /*
     * when BND prefix is set by DEFAULT directive,
     * BND prefix is added to every appropriate instruction line
     * unless it is overridden by NOBND prefix.
     */
    if (globalbnd &&
        (itemp_has(temp, IF_BND) && !has_prefix(ins, PPS_REP, P_NOBND)))
            ins->prefixes[PPS_REP] = P_BND;

    /*
     * Add length of legacy prefixes
     */
    length += emit_prefixes(NULL, ins);

    return length;
}

/* Emit REX and REX2 prefixes, and if necessary legacy opmap bytes */
static inline void emit_rex(struct out_data *data, insn *ins)
{
    if (ins->rex_done)
        return;

    ins->rex_done = true;

    if (ins->rex & (REX_V | REX_EV))
        return;                 /* Handled elsewhere */

    if (ins->rex & REX_2) {
        uint16_t rex2 = 0x00d5;
        nasm_assert(ins->vex_cm < 2);
        rex2 |= (ins->rex & (REX_BXR0|REX_W)) << 8;
        rex2 |= (ins->rex & REX_BXR1);
        rex2 |= ins->vex_cm << 15;
        out_rawword(data, rex2);
    } else {
        uint8_t rex = ins->rex & (REX_MASK|REX_L);

        if (rex == (REX_L|REX_R)) {
            out_rawbyte(data, 0xf0);
        } else if (rex & REX_P) {
            out_rawbyte(data, rex);
        } else {
            nasm_assert(!rex);
        }

        if (ins->vex_cm) {
            out_rawbyte(data, 0x0f);
            if (ins->vex_cm > 1) {
                /* Map 2 = 0F 38, map 3 = 0F 3A */
                nasm_assert(ins->vex_cm < 4);
                out_rawbyte(data, 0x34 + (ins->vex_cm << 1));
            }
        }
    }
}

/*
 * Return the byte value of a legacy prefix (possibly depending on context)
 * Returns one of the enum prefix_err values if the prefix has no byte value.
 */
static int prefix_byte(enum prefixes pfx, const int bits)
{
    switch ((int)pfx) {
    case P_WAIT:
        return 0x9B;

    case P_LOCK:
        return 0xF0;

    case P_REPNE:
    case P_REPNZ:
    case P_XACQUIRE:
    case P_BND:
        return 0xF2;

    case P_REPE:
    case P_REPZ:
    case P_REP:
    case P_XRELEASE:
        return 0xF3;

    case R_CS:
        return 0x2E;

    case R_DS:
        return 0x3E;

    case R_ES:
        return 0x26;

    case R_FS:
        return 0x64;
        break;

    case R_GS:
        return 0x65;
        break;

    case R_SS:
        return 0x36;
        break;

    case P_A16:
        if (bits == 64)
            return PFE_ERR;
        else if (bits == 32)
            return 0x67;
        else
            return PFE_NULL;

    case P_A32:
        return (bits != 32) ? 0x67 : PFE_NULL;

    case P_ASP:
        return 0x67;

    case P_O16:
        return (bits != 16) ? 0x66 : PFE_NULL;

    case P_O32:
        return (bits == 16) ? 0x66 : PFE_NULL;

    case P_O64:
        /* Handled elsewhere via REX.W */
        return (bits == 64) ? PFE_NULL : PFE_ERR;

    case P_OSP:
        return 0x66;

    case P_REX:
    case P_VEX:
    case P_EVEX:
    case P_VEX3:
    case P_VEX2:
    case P_REX2:
        return PFE_MULTI;

    case P_NF:
    case P_NOBND:
    case P_ZU:
    case P_none:
        return PFE_NULL;

    default:
        return PFE_WHAT;
    }
}

/* Emit legacy prefixes */
static int emit_prefixes(struct out_data *data, const insn *ins)
{
    const int bits = ins->bits;
    uint8_t buf[MAXPREFIX];
    int bytes = 0;
    int j;

    for (j = 0; j < MAXPREFIX; j++) {
        const enum prefixes pfx = ins->prefixes[j];
        int c = prefix_byte(pfx, bits);

        /* Various warnings and error conditions */
        switch ((int)pfx) {
        case R_ES:
        case R_SS:
        case R_CS:
        case R_DS:
            /*!
             *!prefix-seg [on] segment prefix ignored in 64-bit mode
             *!  warns that an \c{es}, \c{cs}, \c{ss} or \c{ds} segment override
             *!  prefix has no effect in 64-bit mode. The prefix will still be
             *!  generated as requested.
             */
            if (bits == 64) {
                nasm_warn(WARN_PREFIX_SEG|ERR_PASS2,
                          "%s segment override will be ignored in 64-bit mode",
                          prefix_name(pfx));
            }
            break;

        case P_A16:
            if (bits == 64)
                nasm_nonfatal("16-bit addressing is not supported "
                              "in 64-bit mode");
            break;

        case P_A64:
            if (bits != 64)
                nasm_nonfatal("64-bit addressing is only supported "
			      "in 64-bit mode");
            break;

        case P_O64:
            if (bits != 64)
                nasm_nonfatal("64-bit operand size is only supported "
			      "in 64-bit mode");
            break;

        default:
            switch (c) {
            case 0x66:
            case 0xf2:
            case 0xf3:
                /* These prefixes are embedded in a VEX/EVEX prefix */
                if (ins->rex & (REX_V|REX_EV))
                    c = PFE_NULL;
                break;
            case PFE_ERR:
                nasm_nonfatal("invalid use of %s prefix", prefix_name(pfx));
                break;
            case PFE_WHAT:
                nasm_nonfatal("%s cannot be used as a prefix", prefix_name(pfx));
                break;
            default:
                break;
            }
        }

        if (c >= 0)
            buf[bytes++] = c;
    }

    if (data && bytes)
        out_rawdata(data, buf, bytes);

    return bytes;
}

static void gencode(struct out_data *data, insn *ins)
{
    uint8_t c;
    uint8_t bytes[4];
    int64_t size;
    int op1, op2;
    struct operand *opx, *opy;
    const uint8_t *codes = ins->itemp->code;
    uint8_t opex = 0;
    int r;
    const int bits = ins->bits;

    data->itemp = ins->itemp;
    data->bits  = ins->bits;

    ins->rex_done = false;

    emit_prefixes(data, ins);

    while (*codes) {
        c = *codes++;
        op1 = (c & 3) + ((opex & 1) << 2);
        opx = get_operand(ins, op1);
        op2 = ((c >> 3) & 3) + ((opex & 2) << 1);
        opy = NULL;
        opex = 0;                /* For the next iteration */


        switch (c) {
        case 01:
        case 02:
        case 03:
        case 04:
            emit_rex(data, ins);
            out_rawdata(data, codes, c);
            codes += c;
            break;

        case 05:
        case 06:
        case 07:
            opex = c;
            break;

        case4(010):
            emit_rex(data, ins);
            out_rawbyte(data, *codes++ + (regval(opx) & 7));
            break;

        case4(014):
            break;

        case4(020):
            out_imm(data, opx, 1, OUT_WRAP);
            break;

        case4(024):
            out_imm(data, opx, 1, OUT_UNSIGNED);
            break;

        case4(030):
            out_imm(data, opx, 2, OUT_WRAP);
            break;

        case4(034):
            if (opx->type & (BITS16 | BITS32))
                size = (opx->type & BITS16) ? 2 : 4;
            else
                size = (bits == 16) ? 2 : 4;
            out_imm(data, opx, size, OUT_WRAP);
            break;

        case4(040):
            out_imm(data, opx, 4, OUT_WRAP);
            break;

        case4(044):
            size = ins->addr_size >> 3;
            out_imm(data, opx, size, OUT_WRAP);
            break;

        case4(050):
            if (opx->segment == data->loc.segment) {
                int64_t delta = sext(opx->offset - data->loc.offset
                                     - (data->inslen - data->insoffs),
                                     ins->op_size);
                if (delta != (int8_t)delta)
                    nasm_nonfatal("short jump is out of range");
            }
            out_reladdr(data, opx, 1);
            break;

        case4(054):
            out_imm(data, opx, 8, OUT_WRAP);
            break;

        case4(060):
            out_reladdr(data, opx, 2);
            break;

        case4(064):
            if (opx->type & (BITS16 | BITS32 | BITS64))
                size = (opx->type & BITS16) ? 2 : 4;
            else
                size = (bits == 16) ? 2 : 4;

            out_reladdr(data, opx, size);
            break;

        case4(070):
            out_reladdr(data, opx, 4);
            break;

        case4(074):
            if (opx->segment == NO_SEG)
                nasm_nonfatal("value referenced by FAR is not relocatable");
            out_segment(data, opx);
            break;

        case 0171:
            c = *codes++;
            op2 = (op2 & ~3) | ((c >> 3) & 3);
            opx = &ins->oprs[op2];
            r = nasm_regvals[opx->basereg];
            c = (c & ~070) | ((r & 7) << 3);
            out_rawbyte(data, c);
            break;

        case 0172:
        {
            int mask = ins->prefixes[PPS_REX] == P_EVEX ? 7 : 15;
            const struct operand *opy;

            c = *codes++;
            opx = get_operand(ins, op1 = (c >> 3) & 7);
            opy = get_operand(ins, op2 = c & 7);
            if (!absolute_op(opy))
                nasm_nonfatal("non-absolute expression not permitted "
                              "as argument %d", op2);
            else if (opy->offset & ~mask)
                nasm_warn(ERR_PASS2|WARN_NUMBER_OVERFLOW,
                           "is4 argument exceeds bounds");
            c = opy->offset & mask;
            goto emit_is4;
         }

        case 0173:
            c = *codes++;
            opx = get_operand(ins, op1 = c >> 4);
            c &= 15;
            goto emit_is4;

        case4(0174):
            c = 0;
        emit_is4:
            r = nasm_regvals[opx->basereg];
            out_rawbyte(data, (r << 4) | ((r & 0x10) >> 1) | c);
            break;

        case4(0254):
            if (absolute_op(opx) &&
                (int32_t)opx->offset != (int64_t)opx->offset) {
                nasm_warn(ERR_PASS2|WARN_NUMBER_OVERFLOW,
                           "signed dword immediate exceeds bounds");
            }
            out_imm(data, opx, 4, OUT_SIGNED);
            break;

        case4(0240):
        case 0250:
            codes += 4;
            ins->evex ^= op_evexflags(&ins->oprs[0], EVEX_P2Z|EVEX_P2AAA);
            ins->evex ^= (ins->rex & REX_BXR0) << 13; /* R0 X0 B0 */
            if (ins->rex & REX_B1)
                ins->evex ^= (ins->rex & REX_BV) ? EVEX_P0X  : EVEX_P0BP;
            if (ins->rex & REX_X1)
                ins->evex ^= (ins->rex & REX_XV) ? EVEX_P2VP : EVEX_P1XP;
            ins->evex ^= (ins->rex & REX_R1)   >> (14-12);
            ins->evex ^= (ins->rex & REX_W)    << (23-3);
            ins->evex ^= (ins->vexreg & 15) << 19;
            ins->evex ^= (ins->vexreg & 16) << (27 - 4);
            /* Set NF if this is EVEX-encoded */
            if (ins->prefixes[PPS_NF] == P_NF) {
                if (itemp_has(ins->itemp, IF_NF_E))
                    ins->evex ^= EVEX_P2NF;
            }
            out_rawdword(data, ins->evex);
            break;

        case4(0260):
        case 0270:
            codes += 2;
            if (ins->vex_cm != 1 ||
                (ins->rex & (REX_W|REX_X|REX_B)) ||
                ins->prefixes[PPS_REX] == P_VEX3) {
                bytes[0] = (ins->vex_cm >> 6) ? 0x8f : 0xc4;
                bytes[1] = (ins->vex_cm & 31) |
                    ((~ins->rex & REX_BXR0) << 5);
                bytes[2] = ((ins->rex & REX_W) << (7-3)) |
                    ((~ins->vexreg & 15) << 3) |
                    (ins->vex_wlp & 07);
                out_rawdata(data, bytes, 3);
            } else {
                bytes[0] = 0xc5;
                bytes[1] = ((~ins->rex & REX_R) << (7-2)) |
                    ((~ins->vexreg & 15) << 3) |
                    (ins->vex_wlp & 07);
                out_rawdata(data, bytes, 2);
            }
            break;

        case 0271:
        case 0272:
        case 0273:
            break;

        case4(0274):
        {
            uint64_t uv, um;
            int s;

            if (absolute_op(opx)) {
                s = ins->op_size;

                um = (uint64_t)2 << (s-1);
                uv = opx->offset;

                if (uv > 127 && uv < (uint64_t)-128 &&
                    (uv < um-128 || uv > um-1)) {
                    /*
                     * If this wasn't explicitly byte-sized, warn as though we
                     * had fallen through to the imm16/32/64 case.
                     */
                    nasm_warn(ERR_PASS2|WARN_NUMBER_OVERFLOW,
                               "%s exceeds bounds",
                               (opx->type & BITS8) ? "signed byte" :
                               s == 16 ? "word" :
                               s == 32 ? "dword" :
                               "signed dword");
                }

                /* Output as a raw byte to avoid byte overflow check */
                out_rawbyte(data, (uint8_t)uv);
            } else {
                out_imm(data, opx, 1, OUT_WRAP); /* XXX: OUT_SIGNED? */
            }
            break;
        }

        case4(0300):
            break;

        case 0310:
        case 0311:
            break;

        case 0312:
            break;

        case 0313:
            break;

        case4(0314):
            break;

        case 0320:
        case 0321:
            break;

        case 0322:
        case 0323:
        case 0324:
        case 0327:
            break;

        case 0325:
            break;

        case 0326:
            break;

        case 0330:
        case 0331:
        case 0332:
        case 0333:
            break;

        case 0334:
            break;

        case 0335:
            break;

        case 0336:
        case 0337:
            break;

        case 0340:
            if (opx->segment != NO_SEG)
                nasm_panic("non-constant BSS size in pass two");

            out_reserve(data, ins->oprs[0].offset * resb_bytes(ins->opcode));
            break;

        case 0341:
            break;

        case4(0344):
            break;

        case 0350:
        case 0351:
            break;

        case3(0355):
            break;

        case 0360:
            break;

        case 0361:
            out_rawbyte(data, 0x66);
            break;

        case 0364:
        case 0365:
            break;

        case 0366:
        case 0367:
            out_rawbyte(data, c - 0366 + 0x66);
            break;

        case3(0370):
            break;

        case 0373:
            out_rawbyte(data, bits == 16 ? 3 : 5);
            break;

        case 0374:
        case 0375:
        case 0376:
            break;

        case4(0100):
        case4(0110):
        case4(0120):
        case4(0130):
        case4(0200):
        case4(0204):
        case4(0210):
        case4(0214):
        case4(0220):
        case4(0224):
        case4(0230):
        case4(0234):
            {
                uint8_t *p;
                bool overflow;

                opy = get_operand(ins, op2);

                p = bytes;
                *p++ = ins->ea.modrm;
                if (ins->ea.sib_present)
                    *p++ = ins->ea.sib;
                out_rawdata(data, bytes, p - bytes);

                /*
                 * Make sure the address gets the right offset in case
                 * the line breaks in the .lst file (BR 1197827)
                 */
                switch (ins->ea.bytes) {
                case 0:
                    /* No displacement */
                    overflow = false;
                    break;
                case 1:
                    /* 8-bit displacement, which may be compressed */
                    out_rawbyte(data, ins->ea.disp8);
                    overflow = !ins->ea.disp8_ok;
                    break;
                default:
                    if (ins->ea.rip) {
                        out_reladdr(data, opy, ins->ea.bytes);
                        overflow = false;
                    } else {
                        unsigned int asize = ins->addr_size >> 3;

                        out_imm(data, opy, ins->ea.bytes,
                                (asize > ins->ea.bytes)
                                ? OUT_SIGNED : OUT_WRAP);

                        overflow = overflow_general(opy->offset, asize) ||
                            sext(opy->offset, ins->addr_size) !=
                            sext(opy->offset, ins->ea.bytes << 3);
                    }
                }

                if (overflow)
                    warn_overflow(ins->ea.bytes, NULL, " displacement");
            }
            break;

        default:
            nasm_panic("internal instruction table corrupt"
                    ": instruction code \\%o (0x%02X) given", c, c);
            break;
        }
    }
}

static opflags_t regflag(const operand * o)
{
    if (!is_register(o->basereg))
        nasm_panic("invalid operand passed to regflag()");
    return nasm_reg_flags[o->basereg];
}

static int32_t regval(const operand * o)
{
    /*
     * Certain instruction patterns allow an immediate to be put
     * into a register field
     */
    if (o->type & IMMEDIATE)
        return o->offset;
    if (!is_register(o->basereg))
        nasm_panic("invalid operand passed to regval()");
    return nasm_regvals[o->basereg];
}

static uint32_t op_rexflags(const operand * o, uint32_t mask)
{
    opflags_t flags;
    int val;

    if (!is_register(o->basereg))
        nasm_panic("invalid operand passed to op_rexflags()");

    flags = nasm_reg_flags[o->basereg];
    val = nasm_regvals[o->basereg];

    return rexflags(val, flags, mask);
}

static uint32_t rexflags(int val, opflags_t flags, uint32_t mask)
{
    uint32_t rex = 0;

    if (val < 0 || !is_class(REGISTER, flags)) {
        /* Not a register */
        return 0;
    }

    if (val & 8)
        rex |= REX_B|REX_X|REX_R;
    if (val & 16)
        rex |= REX_B1|REX_X1|REX_R1;

    if (flags & REG_CLASS_VECTOR) {
        rex |= REX_BV|REX_XV|REX_RV;
    } else if (is_class(REG8, flags)) {
        if (is_class(REG_HIGH, flags)) {
            /* AH, CH, DH, BH: REX/REX2/VEX/EVEX forbidden */
            rex |= REX_H;
        } else if (val >= 4) {
            /* SPL, BPL, SIL, DIL, or extended: prefix required */
            rex |= REX_P;
        }
    }

    return rex & mask;
}

static uint32_t evexflags(decoflags_t deco, uint32_t mask)
{
    uint32_t evex = 0;

    if (deco & Z)
        evex |= EVEX_P2Z;
    if (deco & OPMASK_MASK)
        evex |= (deco << 24) & EVEX_P2AAA;

    return evex & mask;
}

static uint32_t op_evexflags(const operand * o, uint32_t mask)
{
    return evexflags(o->decoflags, mask);
}

static enum match_result find_match(const struct itemplate **tempp,
                                    insn *instruction)
{
    const int bits = instruction->bits;
    const struct itemplate *temp;
    enum match_result m, merr;
    opflags_t xsizeflags[MAX_OPERANDS];
    bool opsizemissing = false;
    int i;
    int rex = instruction->prefixes[PPS_REX];

    /* Impossible encoding request? */
    if (bits != 64) {
        if (rex == P_REX || rex == P_REX2)
            return MERR_ENCMISMATCH;
    }

    for (i = 0; i < instruction->operands; i++)
        xsizeflags[i] = instruction->oprs[i].xsize;

    merr = MERR_INVALOP;

    for (temp = nasm_instructions[instruction->opcode];
         temp->opcode != I_none; temp++) {
        m = matches(temp, instruction);
        if (m == MOK_JUMP) {
            if (jmp_match(instruction, temp))
                m = MOK_GOOD;
            else
                m = MERR_INVALOP;
        } else if (m == MERR_OPSIZEMISSING && !itemp_has(temp, IF_SX)) {
            /*
             * Missing operand size and a candidate for fuzzy matching...
             */
            for (i = 0; i < temp->operands; i++) {
                if (instruction->oprs[i].bcast)
                    xsizeflags[i] |= temp->deco[i] & BRSIZE_MASK;
                else
                    xsizeflags[i] |= temp->opd[i] & SIZE_MASK;
            }
            opsizemissing = true;
        }
        if (m > merr)
            merr = m;
        if (merr == MOK_GOOD)
            goto done;
    }

    /* No match, but see if we can get a fuzzy operand size match... */
    if (!opsizemissing)
        goto done;

    for (i = 0; i < instruction->operands; i++) {
        struct operand *op = &instruction->oprs[i];
        /*
         * We ignore extrinsic operand sizes on registers, so we should
         * never try to fuzzy-match on them.  This also resolves the case
         * when we have e.g. "xmmrm128" in two different positions.
         */
        if (is_class(REGISTER, op->type))
            continue;

        /* This tests if xsizeflags[i] has more than one bit set */
        if ((xsizeflags[i] & (xsizeflags[i]-1)))
            goto done;                /* No luck */

        if (op->bcast) {
            op->decoflags |= xsizeflags[i];
            op->type |= brsize_to_size(xsizeflags[i]);
        } else {
            op->type |= xsizeflags[i]; /* Set the size */
        }
    }

    /* Try matching again... */
    for (temp = nasm_instructions[instruction->opcode];
         temp->opcode != I_none; temp++) {
        m = matches(temp, instruction);
        if (m == MOK_JUMP) {
            if (jmp_match(instruction, temp))
                m = MOK_GOOD;
            else
                m = MERR_INVALOP;
        }
        if (m > merr)
            merr = m;
        if (merr == MOK_GOOD)
            goto done;
    }

done:
    *tempp = temp;
    return merr;
}

static uint8_t get_broadcast_num(opflags_t opflags, opflags_t brsize)
{
    unsigned int opsize = (opflags & SIZE_MASK) >> SIZE_SHIFT;
    uint8_t brcast_num;

    if (brsize > BITS64)
        nasm_fatal("size of broadcasting element is greater than 64 bits");

    /*
     * The shift term is to take care of the extra BITS80 inserted
     * between BITS64 and BITS128.
     */
    brcast_num = ((opsize / (BITS64 >> SIZE_SHIFT)) * (BITS64 / brsize))
        >> (opsize > (BITS64 >> SIZE_SHIFT));

    return brcast_num;
}

static enum match_result matches(const struct itemplate * const itemp,
                                 const insn *instruction)
{
    const int bits = instruction->bits;
    bool opsizemissing = false;
    int i;
    const int oprs = instruction->operands;
    unsigned int armask, smmask;
    opflags_t arsize, smsize;
    opflags_t size[MAX_OPERANDS];

    /*
     * Check the opcode
     */
    if (itemp->opcode != instruction->opcode)
        return MERR_INVALOP;

    /*
     * Count the operands
     */
    if (itemp->operands != oprs)
        return MERR_INVALOP;

    /*
     * Is it legal?
     */
    if (unlikely(instruction->opt & OPTIM_STRICT_INSTR)) {
        if (itemp_has(itemp, IF_OPT))
            return MERR_INVALOP;
    }

    /*
     * {rex/vexn/evex} available?
     */
    switch (instruction->prefixes[PPS_REX]) {
    case P_EVEX:
        if (!itemp_has(itemp, IF_EVEX))
            return MERR_ENCMISMATCH;
        break;
    case P_VEX:
    case P_VEX3:
    case P_VEX2:
        if (!itemp_has(itemp, IF_VEX))
            return MERR_ENCMISMATCH;
        break;
    case P_REX:
        if (bits != 64 ||
            itemp_has(itemp, IF_VEX) ||
            itemp_has(itemp, IF_EVEX) ||
            itemp_has(itemp, IF_REX2))
            return MERR_ENCMISMATCH;
        break;
    case P_REX2:
        if (bits != 64 ||
            itemp_has(itemp, IF_NOAPX) ||
            itemp_has(itemp, IF_EVEX))
            return MERR_ENCMISMATCH;
        break;
    default:
        if (itemp_has(itemp, IF_EVEX)) {
            if (!iflag_test(&cpu, IF_EVEX))
                return MERR_BADCPU;
        } else if (itemp_has(itemp, IF_VEX)) {
            if (!iflag_test(&cpu, IF_VEX)) {
                return MERR_BADCPU;
            } else if (itemp_has(itemp, IF_LATEVEX)) {
                if (!iflag_test(&cpu, IF_LATEVEX) && iflag_test(&cpu, IF_EVEX))
                    return MERR_BADCPU;
            } else if (itemp_has(itemp, IF_REX2)) {
                if (bits != 64 || !iflag_test(&cpu, IF_APX))
                    return MERR_BADCPU;
            }
        }
        break;
    }

    /*
     * First, cursory operand filtering
     */
    for (i = 0; i < oprs; i++) {
        const struct operand * const op = &instruction->oprs[i];
        if (op->type & ~itemp->opd[i] & (COLON | TO))
            return MERR_INVALOP;
        if (op->iflag && !itemp_has(itemp, op->iflag))
            return MERR_WRONGIMM;
    }

    /*
     * Process size flags
     */
    switch (itemp_smask(itemp)) {
    case IF_GENBIT(IF_SB):
        arsize = BITS8;
        break;
    case IF_GENBIT(IF_SW):
        arsize = BITS16;
        break;
    case IF_GENBIT(IF_SD):
        arsize = BITS32;
        break;
    case IF_GENBIT(IF_SQ):
        arsize = BITS64;
        break;
    case IF_GENBIT(IF_SO):
        arsize = BITS128;
        break;
    case IF_GENBIT(IF_SY):
        arsize = BITS256;
        break;
    case IF_GENBIT(IF_SZ):
        arsize = BITS512;
        break;
    case IF_GENBIT(IF_ANYSIZE):
        arsize = SIZE_MASK;
        break;
    case IF_GENBIT(IF_SIZE):
        switch (bits) {
        case 16:
            arsize = BITS16;
            break;
        case 32:
            arsize = BITS32;
            break;
        case 64:
            arsize = BITS64;
            break;
        default:
            arsize = 0;
            break;
        }
        break;
    default:
        arsize = 0;
        break;
    }

    /* Flags for which the AR and SM flags apply */
    armask = arsize ? itemp_arx(itemp) : 0;
    smmask = itemp_smx(itemp);

    /* Look for any sized operands among the size-match ones */
    smsize = 0;
    for (i = 0; i < oprs; i++) {
        if (smmask & (1 << i)) {
            opflags_t osz = instruction->oprs[i].type & SIZE_MASK;
            if (osz) {
                if (smsize && smsize != osz) {
                    /* Operands need to match, and they don't */
                    return MERR_OPSIZEMISMATCH;
                }
                smsize = osz;
            }
        }
    }

    if (!smsize)
        smmask = 0;

    /* Give unsized operands a size */
    for (i = 0; i < oprs; i++) {
        if (smmask & (1 << i))
            size[i] = smsize;
        else if (armask & (1 << i))
            size[i] = arsize;
        else
            size[i] = 0;
    }

    /*
     * Check that the operand flags all match up,
     * it's a bit tricky so lets be verbose:
     *
     * 1) Find out the size of operand. If instruction
     *    doesn't have one specified -- we're trying to
     *    guess it either from template (IF_S* flag) or
     *    from code bits.
     *
     * 2) If template operand do not match the instruction OR
     *    template has an operand size specified AND this size differ
     *    from which instruction has (perhaps we got it from code bits)
     *    we are:
     *      a)  Check that only size of instruction and operand is differ
     *          other characteristics do match
     *      b)  Perhaps it's a register specified in instruction so
     *          for such a case we just mark that operand as "size
     *          missing" and this will turn on fuzzy operand size
     *          logic facility (handled by a caller)
     */
    for (i = 0; i < oprs; i++) {
        opflags_t type = instruction->oprs[i].type;
        decoflags_t deco = instruction->oprs[i].decoflags;
        decoflags_t ideco = itemp->deco[i];
        bool is_broadcast = deco & BRDCAST_MASK;
        uint8_t brcast_num = 0;
        opflags_t template_opsize, insn_opsize;

        if (!(type & SIZE_MASK))
            type |= size[i];

        insn_opsize     = type & SIZE_MASK;
        if (!is_broadcast) {
            template_opsize = itemp->opd[i] & SIZE_MASK;
        } else {
            decoflags_t deco_brsize = ideco & BRSIZE_MASK;

            if (~ideco & BRDCAST_MASK)
                return MERR_BRNOTHERE;

            /*
             * when broadcasting, the element size depends on
             * the instruction type. decorator flag should match.
             */
            if (deco_brsize) {
                template_opsize = brsize_to_size(deco_brsize);
                /* calculate the proper number : {1to<brcast_num>} */
                brcast_num = get_broadcast_num(itemp->opd[i], template_opsize);
            } else {
                template_opsize = 0;
            }
        }

        if (~ideco & deco & OPMASK_MASK)
            return MERR_MASKNOTHERE;

        if (~ideco & deco & (Z_MASK|STATICRND_MASK|SAE_MASK))
            return MERR_DECONOTHERE;

        if (itemp->opd[i] & ~type & ~(SIZE_MASK|REGSET_MASK))
            return MERR_INVALOP;

        if (~itemp->opd[i] & type & REGSET_MASK)
            return (itemp->opd[i] & REGSET_MASK)
                ? MERR_REGSETSIZE : MERR_REGSET;

        if (template_opsize) {
            if (template_opsize != insn_opsize) {
                if (insn_opsize) {
                    return MERR_INVALOP;
                } else if (!is_class(REGISTER, type)) {
                    /*
                     * Note: we don't honor extrinsic operand sizes for registers,
                     * so "missing operand size" for a register should be
                     * considered a wildcard match rather than an error.
                     */
                    opsizemissing = true;
                } else if (is_class(REG_HIGH, type) &&
                           instruction->prefixes[PPS_REX]) {
                    return MERR_ENCMISMATCH;
                }
            } else if (is_broadcast &&
                       (brcast_num !=
                        (2U << ((deco & BRNUM_MASK) >> BRNUM_SHIFT)))) {
                /*
                 * broadcasting opsize matches but the number of repeated memory
                 * element does not match.
                 * if 64b double precision float is broadcasted to ymm (256b),
                 * broadcasting decorator must be {1to4}.
                 */
                return MERR_BRNUMMISMATCH;
            }
        }
   }

    if (opsizemissing)
        return MERR_OPSIZEMISSING;

    /*
     * Check operand sizes
     */
    for (i = 0; i < oprs; i++) {
        if (!(itemp->opd[i] & SIZE_MASK) &&
            (instruction->oprs[i].type & SIZE_MASK & ~size[i]))
            return MERR_OPSIZEMISMATCH;
    }

    /*
     * Check template is okay at the set cpu level
     */
    if (iflag_cmp_cpu_level(&insns_flags[itemp->iflag_idx], &cpu) > 0)
        return MERR_BADCPU;

    /*
     * Verify the appropriate long mode flag.
     */
    if (itemp_has(itemp, (bits == 64 ? IF_NOLONG : IF_LONG)))
        return MERR_BADMODE;

    /*
     * If we have a HLE prefix, look for the NOHLE flag
     */
    if (itemp_has(itemp, IF_NOHLE) &&
        (has_prefix(instruction, PPS_REP, P_XACQUIRE) ||
         has_prefix(instruction, PPS_REP, P_XRELEASE)))
        return MERR_BADHLE;

    /*
     * {nf} or {zu} prefixes used? Must be permitted.
     */
    if (has_prefix(instruction, PPS_NF, P_NF)) {
        if (!itemp_has(itemp, IF_NF) &&
            (itemp_has(itemp, IF_FL) ||
             (instruction->opt & OPTIM_STRICT_INSTR)))
            return MERR_BADNF;
    } else if (itemp_has(itemp, IF_NF_R)) {
        return MERR_REQNF;
    }

    if (has_prefix(instruction, PPS_ZU, P_ZU)) {
        if (!itemp_has(itemp, IF_ZU))
            return MERR_BADZU;
        else if (!(instruction->oprs[0].type & REGISTER))
            return MERR_MEMZU;
    }

    /*
     * Check if special handling needed for Jumps
     */
    if ((itemp->code[0] & ~1) == 0370)
        return MOK_JUMP;

    /*
     * Check if BND prefix is allowed.
     * Other 0xF2 (REPNE/REPNZ) prefix is prohibited.
     */
    if (!itemp_has(itemp, IF_BND) &&
        (has_prefix(instruction, PPS_REP, P_BND) ||
         has_prefix(instruction, PPS_REP, P_NOBND)))
        return MERR_BADBND;
    else if (itemp_has(itemp, IF_BND) &&
             (has_prefix(instruction, PPS_REP, P_REPNE) ||
              has_prefix(instruction, PPS_REP, P_REPNZ)))
        return MERR_BADREPNE;

    return MOK_GOOD;
}

/*
 * Select the mod part of modr/m for an memory operand with displacement.
 * zerook should be clear for the forbidden BP encodings; such instructions
 * must be coded with a +0 displacement.
 *
 * 0 = no displacement
 * 1 = 8-bit displacment
 * 2 = 16/32-bit displacement
 */
static unsigned int memory_mod(const int eaflags, insn *ins, int64_t o,
                               bool known, bool zerook)
{
    struct ea_data * const output = &ins->ea;

    /* Explicitly requested by user */
    if (eaflags & EAF_WORDOFFS) {
        return 2;
    }

    output->disp8_shift = get_disp8_shift(ins);
    output->disp8 = (int8_t)(o >> output->disp8_shift);
    output->disp8_ok = known &&
        ((int64_t)output->disp8 << output->disp8_shift)
        == sext(o, ins->addr_size);

    /* Explicitly requested by user */
    if (eaflags & EAF_BYTEOFFS) {
        return 1;
    }

    /* Not knowable at this time */
    if (!known)
        return 2;

    /* No displacement needed? */
    if (o == 0 && zerook)
        return 0;

    /* 8-bit displacement possible? */
    return output->disp8_ok ? 1 : 2;
}

static int process_ea(operand *input, int rfield, opflags_t rflags,
                      insn *ins, enum ea_type expected,
                      const char **errmsgp)
{
    struct ea_data * const output = &ins->ea;
    const bool long_mode = ins->bits == 64;
    const int addrbits = ins->addr_size;
    const int eaflags = input->eaflags;
    const char *errmsg = NULL;

    errmsg = NULL;

    nasm_zero(*output);
    output->type    = EA_SCALAR;

    /* REX flags for the rfield operand */
    output->rex     |= rexflags(rfield, rflags, REX_rR);

    if (is_class(REGISTER, input->type)) {
        /*
         * It's a direct register.
         */
        if (!is_register(input->basereg))
            goto err;

        if (!is_reg_class(REG_EA, input->basereg))
            goto err;

        /* broadcasting is not available with a direct register operand. */
        if (input->decoflags & BRDCAST_MASK) {
            errmsg = "broadcast not allowed with register operand";
            goto err;
        }

        output->rex         |= op_rexflags(input, REX_rB);
        output->sib_present = false;    /* no SIB necessary */
        output->bytes       = 0;        /* no offset necessary either */
        output->modrm       = GEN_MODRM(3, rfield, nasm_regvals[input->basereg]);
    } else {
        /*
         * It's a memory reference.
         */

        /* Embedded rounding or SAE is not available with a mem ref operand. */
        if (input->decoflags & (ER | SAE)) {
            errmsg = "embedded rounding is available only with "
                "register-register operations";
            goto err;
        }

        if (input->basereg == -1 &&
            (input->indexreg == -1 || input->scale == 0)) {
            /*
             * It's a pure offset. If it is an IMMEDIATE, it is a pattern
             * in insns.dat which allows an immediate to be used as a memory
             * address, in which case apply the default REL/ABS.
             */
            if (long_mode) {
                if (is_class(IMMEDIATE, input->type)) {
                    if (!(input->eaflags & EAF_ABS) &&
                        ((input->eaflags & EAF_REL) || globalrel))
                        input->type |= IP_REL;
                }
                if ((input->type & IP_REL) == IP_REL) {
                    /*!
                     *!ea-absolute [on] absolute address cannot be RIP-relative
                     *!  warns that an address that is inherently absolute cannot
                     *!  be generated with RIP-relative encoding using \c{REL},
                     *!  see \k{REL & ABS}.
                     */
                    if (input->segment == NO_SEG ||
                        (input->opflags & OPFLAG_RELATIVE)) {
                        nasm_warn(WARN_EA_ABSOLUTE|ERR_PASS2,
                                  "absolute address can not be RIP-relative");
                        input->type &= ~IP_REL;
                        input->type |= MEMORY;
                    }
                }
            }

            if (long_mode && !(IP_REL & ~input->type) && (eaflags & EAF_SIB)) {
                errmsg = "instruction requires SIB encoding, cannot be RIP-relative";
                goto err;
            }

            if ((eaflags & EAF_BYTEOFFS) ||
                ((eaflags & EAF_WORDOFFS) &&
                 input->disp_size != (addrbits != 16 ? 32 : 16))) {
                /*!
                 *!ea-dispsize [on] displacement size ignored on absolute address
                 *!  warns that NASM does not support generating displacements for
                 *!  inherently absolute addresses that do not match the address size
                 *!  of the instruction.
                 */
                nasm_warn(WARN_EA_DISPSIZE, "displacement size ignored on absolute address");
            }

            if ((eaflags & EAF_SIB) ||
                (long_mode && (~input->type & IP_REL))) {
                output->sib_present = true;
                output->sib         = GEN_SIB(0, 4, 5);
                output->bytes       = 4;
                output->modrm       = GEN_MODRM(0, rfield, 4);
                output->rip         = false;
            } else {
                output->sib_present = false;
                output->bytes       = (addrbits != 16 ? 4 : 2);
                output->modrm       = GEN_MODRM(0, rfield,
                                                (addrbits != 16 ? 5 : 6));
                output->rip         = long_mode;
            }
        } else {
            /*
             * It's an indirection.
             */
            int i = input->indexreg, b = input->basereg, s = input->scale;
            const bool known =
                !(input->opflags & OPFLAG_UNKNOWN) &&
                (input->segment == NO_SEG);
            int hb = input->hintbase, ht = input->hinttype;
            int t, it, bt;              /* register numbers */
            opflags_t x, ix, bx;        /* register flags */

            if (s == 0)
                i = -1;         /* make this easy, at least */

            if (is_register(i)) {
                it = nasm_regvals[i];
                ix = nasm_reg_flags[i];
            } else {
                it = -1;
                ix = 0;
            }

            if (is_register(b)) {
                bt = nasm_regvals[b];
                bx = nasm_reg_flags[b];
            } else {
                bt = -1;
                bx = 0;
            }

            /* if either one are a vector register... */
            if ((ix|bx) & (XMMREG|YMMREG|ZMMREG) & ~REG_EA) {
                opflags_t sok = BITS32 | BITS64;
                int32_t o = input->offset;
                int mod, scale, index, base;

                /*
                 * For a vector SIB, one has to be a vector and the other,
                 * if present, a GPR.  The vector must be the index operand.
                 */
                if (it == -1 || (bx & (XMMREG|YMMREG|ZMMREG) & ~REG_EA)) {
                    if (s == 0)
                        s = 1;
                    else if (s != 1)
                        goto err;

                    t = bt, bt = it, it = t;
                    x = bx, bx = ix, ix = x;
                }

                if (bt != -1) {
                    if (REG_GPR & ~bx)
                        goto err;
                    if (!(REG64 & ~bx) || !(REG32 & ~bx))
                        sok &= bx;
                    else
                        goto err;
                }

                /*
                 * While we're here, ensure the user didn't specify
                 * WORD or QWORD
                 */
                if (input->disp_size == 16 || input->disp_size == 64)
                    goto err;

                if (addrbits == 16 ||
                    (addrbits == 32 && !(sok & BITS32)) ||
                    (addrbits == 64 && !(sok & BITS64)))
                    goto err;

                output->type = ((ix & ZMMREG & ~REG_EA) ? EA_ZMMVSIB
                                : ((ix & YMMREG & ~REG_EA)
                                ? EA_YMMVSIB : EA_XMMVSIB));

                output->rex    |= rexflags(it, ix, REX_rX);
                output->rex    |= rexflags(bt, bx, REX_rB);

                index = it & 7; /* it is known to be != -1 */

                switch (s) {
                case 1:
                    scale = 0;
                    break;
                case 2:
                    scale = 1;
                    break;
                case 4:
                    scale = 2;
                    break;
                case 8:
                    scale = 3;
                    break;
                default:   /* then what the smeg is it? */
                    goto err;    /* panic */
                }

                if (bt == -1) {
                    base = 5;
                    mod = 0;
                } else {
                    base = bt & 7;
                    mod = memory_mod(eaflags, ins, o, known,
                                     base != REG_NUM_EBP);
                }

                output->sib_present = true;
                output->bytes       = (bt == -1 || mod == 2 ? 4 : mod);
                output->modrm       = GEN_MODRM(mod, rfield, 4);
                output->sib         = GEN_SIB(scale, index, base);
            } else if ((ix|bx) & (BITS32|BITS64)) {
                /*
                 * it must be a 32/64-bit memory reference. Firstly we have
                 * to check that all registers involved are type E/Rxx.
                 */
                opflags_t sok = BITS32 | BITS64;
                int32_t o = input->offset;

                if (it != -1) {
                    if (!(REG64 & ~ix) || !(REG32 & ~ix))
                        sok &= ix;
                    else
                        goto err;
                }

                if (bt != -1) {
                    if (REG_GPR & ~bx)
                        goto err; /* Invalid register */
                    if (~sok & bx & SIZE_MASK)
                        goto err; /* Invalid size */
                    sok &= bx;
                }

                /*
                 * While we're here, ensure the user didn't specify
                 * WORD or QWORD
                 */
                if (input->disp_size == 16 || input->disp_size == 64)
                    goto err;

                if (addrbits == 16 ||
                    (addrbits == 32 && !(sok & BITS32)) ||
                    (addrbits == 64 && !(sok & BITS64)))
                    goto err;

                /* now reorganize base/index */
                if (s == 1 && bt != it && bt != -1 && it != -1 &&
                    ((hb == b && ht == EAH_NOTBASE) ||
                     (hb == i && ht == EAH_MAKEBASE))) {
                    /* swap if hints say so */
                    t = bt, bt = it, it = t;
                    x = bx, bx = ix, ix = x;
                }

                if (bt == -1 && s == 1 && !(hb == i && ht == EAH_NOTBASE)) {
                    /* make single reg base, unless hint */
                    bt = it, bx = ix, it = -1, ix = 0;
                }
                if (eaflags & EAF_MIB) {
                    /* MIB/split-SIB encoding */
                    if (it == -1 && (hb == b && ht == EAH_NOTBASE)) {
                        /*
                         * make a single reg index [reg*1].
                         * gas uses this form for an explicit index register.
                         */
                        it = bt, ix = bx, bt = -1, bx = 0, s = 1;
                    }
                    if ((ht == EAH_SUMMED) && bt == -1) {
                        /* separate once summed index into [base, index] */
                        bt = it, bx = ix, s--;
                    }
                } else {
                    if (((s == 2 && it != REG_NUM_ESP &&
                          (!(eaflags & EAF_TIMESTWO) || (ht == EAH_SUMMED))) ||
                         s == 3 || s == 5 || s == 9) && bt == -1) {
                        /* convert 3*EAX to EAX+2*EAX */
                        bt = it, bx = ix, s--;
                    }
                    if (it == -1 && (bt & 7) != REG_NUM_ESP &&
                        (eaflags & EAF_TIMESTWO) &&
                        (hb == b && ht == EAH_NOTBASE)) {
                        /*
                         * convert [NOSPLIT EAX*1]
                         * to sib format with 0x0 displacement - [EAX*1+0].
                         */
                        it = bt, ix = bx, bt = -1, bx = 0, s = 1;
                    }
                }
                if (s == 1 && it == REG_NUM_ESP) {
                    /* swap ESP into base if scale is 1 */
                    t = it, it = bt, bt = t;
                    x = ix, ix = bx, bx = x;
                }
                if (it == REG_NUM_ESP ||
                    (s != 1 && s != 2 && s != 4 && s != 8 && it != -1))
                    goto err;        /* wrong, for various reasons */

                output->rex |= rexflags(it, ix, REX_rX);
                output->rex |= rexflags(bt, bx, REX_rB);

                if (it == -1 && (bt & 7) != REG_NUM_ESP && !(eaflags & EAF_SIB)) {
                    /* no SIB needed */
                    int mod, rm;

                    if (bt == -1) {
                        rm = 5;
                        mod = 0;
                    } else {
                        rm = bt & 7;
                        mod = memory_mod(eaflags, ins, o, known,
                                         rm != REG_NUM_EBP);
                    }

                    output->sib_present = false;
                    output->bytes       = (bt == -1 || mod == 2 ? 4 : mod);
                    output->modrm       = GEN_MODRM(mod, rfield, rm);
                } else {
                    /* we need a SIB */
                    int mod, scale, index, base;

                    if (it == -1)
                        index = 4, s = 1;
                    else
                        index = (it & 7);

                    switch (s) {
                    case 1:
                        scale = 0;
                        break;
                    case 2:
                        scale = 1;
                        break;
                    case 4:
                        scale = 2;
                        break;
                    case 8:
                        scale = 3;
                        break;
                    default:   /* then what the smeg is it? */
                        goto err;    /* panic */
                    }

                    if (bt == -1) {
                        base = 5;
                        mod = 0;
                    } else {
                        base = (bt & 7);
                        mod = memory_mod(eaflags, ins, o, known,
                                         base != REG_NUM_EBP);
                    }

                    output->sib_present = true;
                    output->bytes       = (bt == -1 || mod == 2 ? 4 : mod);
                    output->modrm       = GEN_MODRM(mod, rfield, 4);
                    output->sib         = GEN_SIB(scale, index, base);
                }
            } else {            /* it's 16-bit */
                int mod, rm;
                int16_t o = input->offset;

                /* check for 64-bit long mode */
                if (addrbits == 64)
                    goto err;

                /* check all registers are BX, BP, SI or DI */
                if ((b != -1 && b != R_BP && b != R_BX && b != R_SI && b != R_DI) ||
                    (i != -1 && i != R_BP && i != R_BX && i != R_SI && i != R_DI))
                    goto err;

                /* ensure the user didn't specify DWORD/QWORD */
                if (input->disp_size == 32 || input->disp_size == 64)
                    goto err;

                if (s != 1 && i != -1)
                    goto err;        /* no can do, in 16-bit EA */
                if (b == -1 && i != -1) {
                    int tmp = b;
                    b = i;
                    i = tmp;
                }               /* swap */
                if ((b == R_SI || b == R_DI) && i != -1) {
                    int tmp = b;
                    b = i;
                    i = tmp;
                }
                /* have BX/BP as base, SI/DI index */
                if (b == i)
                    goto err;        /* shouldn't ever happen, in theory */
                if (i != -1 && b != -1 &&
                    (i == R_BP || i == R_BX || b == R_SI || b == R_DI))
                    goto err;        /* invalid combinations */
                if (b == -1)            /* pure offset: handled above */
                    goto err;        /* so if it gets to here, panic! */

                rm = -1;
                if (i != -1)
                    switch (i * 256 + b) {
                    case R_SI * 256 + R_BX:
                        rm = 0;
                        break;
                    case R_DI * 256 + R_BX:
                        rm = 1;
                        break;
                    case R_SI * 256 + R_BP:
                        rm = 2;
                        break;
                    case R_DI * 256 + R_BP:
                        rm = 3;
                        break;
                } else
                    switch (b) {
                    case R_SI:
                        rm = 4;
                        break;
                    case R_DI:
                        rm = 5;
                        break;
                    case R_BP:
                        rm = 6;
                        break;
                    case R_BX:
                        rm = 7;
                        break;
                    }
                if (rm == -1)           /* can't happen, in theory */
                    goto err;        /* so panic if it does */

                mod = memory_mod(eaflags, ins, o, known, rm != 6);
                output->sib_present = false;    /* no SIB - it's 16-bit */
                output->bytes       = mod;      /* bytes of offset needed */
                output->modrm       = GEN_MODRM(mod, rfield, rm);
            }

            if (eaflags & EAF_REL) {
                /* Explicit REL reference with indirect memory */
                nasm_warn(WARN_OTHER,
                          "indirect address displacements cannot be RIP-relative");
            }
        }
    }

    output->size = 1 + output->sib_present + output->bytes;
    /*
     * The type parsed might not match one supplied by
     * a caller. In this case exit with error and let
     * the caller to decide how critical it is.
     */
    if (output->type != expected)
        goto err_set_msg;

    return 0;

err:
    output->type = EA_INVALID;
    goto err_set_msg;

err_set_msg:
    if (!errmsg) {
        /* Default error message */
        static char invalid_address_msg[40];
        snprintf(invalid_address_msg, sizeof invalid_address_msg,
                 "invalid %d-bit effective address", addrbits);
        errmsg = invalid_address_msg;
    }
    *errmsgp = errmsg;
    return -1;
}

static void add_asp(insn *ins)
{
    const int bits = ins->bits;
    int j, valid;
    int defdisp;
    int asp_bits;

    valid = (bits == 64) ? 64|32 : 32|16;

    switch (ins->prefixes[PPS_ASIZE]) {
    case P_A16:
        valid &= 16;
        break;
    case P_A32:
        valid &= 32;
        break;
    case P_A64:
        valid &= 64;
        break;
    case P_ASP:
        valid &= (bits == 32) ? 16 : 32;
        break;
    default:
        break;
    }

    for (j = 0; j < ins->operands; j++) {
        if (is_class(MEMORY, ins->oprs[j].type)) {
            opflags_t i, b;

            /* Verify as Register */
            if (!is_register(ins->oprs[j].indexreg))
                i = 0;
            else
                i = nasm_reg_flags[ins->oprs[j].indexreg];

            /* Verify as Register */
            if (!is_register(ins->oprs[j].basereg))
                b = 0;
            else
                b = nasm_reg_flags[ins->oprs[j].basereg];

            if (ins->oprs[j].scale == 0)
                i = 0;

            if (!i && !b) {
                int ds = ins->oprs[j].disp_size;
                if ((bits != 64 && ds > 8) ||
                    (bits == 64 && ds == 16))
                    valid &= ds;
            } else {
                if (!(REG16 & ~b))
                    valid &= 16;
                if (!(REG32 & ~b))
                    valid &= 32;
                if (!(REG64 & ~b))
                    valid &= 64;

                if (!(REG16 & ~i))
                    valid &= 16;
                if (!(REG32 & ~i))
                    valid &= 32;
                if (!(REG64 & ~i))
                    valid &= 64;
            }
        }
    }

    asp_bits = (bits == 32) ? 16 : 32;

    if (valid & bits) {
        ins->addr_size = bits;
    } else if (valid & asp_bits) {
        /* Add an address size prefix */
        ins->prefixes[PPS_ASIZE] = (bits == 32) ? P_A16 : P_A32;
        ins->addr_size = asp_bits;
    } else {
        /* Impossible... */
        nasm_nonfatal("impossible combination of address sizes");
        ins->addr_size = bits; /* Error recovery */
    }

    defdisp = ins->addr_size == 16 ? 16 : 32;

    for (j = 0; j < ins->operands; j++) {
        int disp_size;

        if (!is_class(MEM_OFFS, ins->oprs[j].type))
            continue;

        disp_size = ins->oprs[j].disp_size;
        if (!disp_size)
            disp_size = defdisp;

        if (disp_size != ins->addr_size) {
            /*
             * mem_offs sizes must match the address size; if not,
             * strip the MEM_OFFS bit and match only EA instructions
             */
            ins->oprs[j].type &= ~(MEM_OFFS & ~MEMORY);
        }
    }
}

/*
 * This is the main entry point to this module, called from asm/nasm.c.
 */
void process_insn(insn *instruction)
{
    int32_t n;
    int64_t l;

    if (instruction->times <= 0) {
        if (instruction->times)
            nasm_nonfatalf(ERR_PASS2, "TIMES value %"PRId32" is negative",
                           instruction->times);
        return;
    }

    /*
     * NOTE: insn_size() can change instruction->times
     * (usually to 1) when called by merging certain operations.
     * Therefore, do NOT cache instruction->times!
     */
    instruction->loc = location;

    if (!pass_final()) {
        int64_t start = location.offset;
        for (n = 0; n < instruction->times; n++) {
            l = insn_size(instruction);
            /* l == -1 -> invalid instruction */
            if (l != -1) {
                increment_offset(l);
                instruction->loc.offset += l;
            }
        }
        if (list_option('p')) {
            struct out_data dummy;
            nasm_zero(dummy);
            /* Handled specially with .data NULL */
            dummy.type       = OUT_RAWDATA;
            dummy.loc.offset = start;
            dummy.size       = location.offset - start;
            lfmt->output(&dummy);
        }
    } else {
        instruction->loc = location;
        l = assemble(instruction);
        /* We can't get an invalid instruction here */
        nasm_assert(l >= 0);
        instruction->loc.offset += l;
        increment_offset(l);

        if (instruction->times > 1) {
            lfmt->uplevel(LIST_TIMES, instruction->times);
            for (n = 2; n <= instruction->times; n++) {
                l = assemble(instruction);
                instruction->loc.offset += l;
                increment_offset(l);
            }
            lfmt->downlevel(LIST_TIMES);
        }
    }
}
