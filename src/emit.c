/*
 * VORTECH Compiler - Native x86-64 Code Emitter
 *
 * Generates x86-64 machine code directly into a byte buffer.
 * Wraps it in an ELF64 relocatable object file.
 * No external assembler. No GCC for code generation.
 * Uses system linker (ld/gcc) only for linking with C runtime.
 *
 * Architecture:
 *   Part 1: CodeBuf (dynamic byte buffer)
 *   Part 2: Emitter state (labels, patches, symbols, relocations)
 *   Part 3: x86-64 encoding helpers
 *   Part 4: Instruction encoding functions
 *   Part 5: Prologue / epilogue encoding
 *   Part 6: Per-instruction emission
 *   Part 7: Function emission loop
 *   Part 8: Print helper runtime
 *   Part 9: Branch patch resolution
 *   Part 10: ELF64 object file writer
 *   Part 11: Public API
 */
#include "emit.h"
#include "elf.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Part 1: CodeBuf - Dynamic Byte Buffer
 * ================================================================ */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} CodeBuf;

static void cb_init(CodeBuf *cb) {
    cb->cap = 4096;
    cb->data = malloc(cb->cap);
    if (!cb->data) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }
    cb->len = 0;
}

static void cb_ensure(CodeBuf *cb, size_t extra) {
    while (cb->len + extra > cb->cap) {
        cb->cap *= 2;
        cb->data = realloc(cb->data, cb->cap);
        if (!cb->data) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }
    }
}

static void cb_push1(CodeBuf *cb, uint8_t b) {
    cb_ensure(cb, 1);
    cb->data[cb->len++] = b;
}

static void cb_push2(CodeBuf *cb, uint8_t a, uint8_t b) {
    cb_ensure(cb, 2);
    cb->data[cb->len++] = a;
    cb->data[cb->len++] = b;
}

static void cb_push3(CodeBuf *cb, uint8_t a, uint8_t b, uint8_t c) {
    cb_ensure(cb, 3);
    cb->data[cb->len++] = a;
    cb->data[cb->len++] = b;
    cb->data[cb->len++] = c;
}

static void cb_push4(CodeBuf *cb, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    cb_ensure(cb, 4);
    cb->data[cb->len++] = a;
    cb->data[cb->len++] = b;
    cb->data[cb->len++] = c;
    cb->data[cb->len++] = d;
}

static void cb_push32le(CodeBuf *cb, uint32_t val) {
    cb_ensure(cb, 4);
    cb->data[cb->len++] = (uint8_t)(val);
    cb->data[cb->len++] = (uint8_t)(val >> 8);
    cb->data[cb->len++] = (uint8_t)(val >> 16);
    cb->data[cb->len++] = (uint8_t)(val >> 24);
}

static void cb_push64le(CodeBuf *cb, uint64_t val) {
    cb_ensure(cb, 8);
    for (int i = 0; i < 8; i++) {
        cb->data[cb->len++] = (uint8_t)(val >> (i * 8));
    }
}

/* Patch a 32-bit little-endian value at a given offset */
static void cb_patch32(CodeBuf *cb, size_t offset, uint32_t val) {
    cb->data[offset]     = (uint8_t)(val);
    cb->data[offset + 1] = (uint8_t)(val >> 8);
    cb->data[offset + 2] = (uint8_t)(val >> 16);
    cb->data[offset + 3] = (uint8_t)(val >> 24);
}

static void cb_free(CodeBuf *cb) {
    free(cb->data);
    cb->data = NULL;
    cb->len = 0;
    cb->cap = 0;
}

/* ================================================================
 * Part 2: Emitter State
 * ================================================================ */

/* A branch patch: we emitted a placeholder rel32, need to fill it in */
typedef struct {
    size_t   rel32_offset;   /* offset in CodeBuf where the rel32 field is */
    uint32_t target_label;   /* which label (block index) it targets */
} BranchPatch;

/* A symbol for the ELF symbol table */
typedef struct {
    const char *name;
    uint32_t    name_off;    /* offset in strtab */
    uint8_t     bind;        /* STB_LOCAL, STB_GLOBAL */
    uint8_t     type;        /* STT_FUNC, STT_NOTYPE */
    uint16_t    shndx;       /* section index or SHN_UNDEF */
    uint64_t    value;       /* offset within section */
    uint64_t    size;        /* symbol size */
} ElfSym;

/* An ELF relocation entry */
typedef struct {
    uint32_t offset;         /* offset in .text */
    uint32_t type;           /* R_X86_64_PLT32, R_X86_64_PC32, etc. */
    uint32_t sym_idx;        /* symbol table index */
    int64_t  addend;
} ElfReloc;

/* String table builder */
typedef struct {
    char     *data;
    uint32_t  len;
    uint32_t  cap;
} StrTab;

/* Full emitter state */
typedef struct {
    CodeBuf       code;          /* .text section bytes */
    CodeBuf       rodata;        /* .rodata section bytes */

    /* Label resolution (per-function, reset between functions) */
    uint32_t     *label_offsets; /* block_index -> code offset */
    uint32_t      nlabels;

    /* Branch patches (per-function, applied after function is emitted) */
    BranchPatch  *patches;
    uint32_t      npatches;
    uint32_t      patches_cap;

    /* ELF symbols */
    ElfSym       *syms;
    uint32_t      nsyms;
    uint32_t      syms_cap;

    /* ELF relocations */
    ElfReloc     *relocs;
    uint32_t      nrelocs;
    uint32_t      relocs_cap;

    /* String tables */
    StrTab        strtab;        /* symbol name strings */
    StrTab        shstrtab;      /* section name strings */
} Emitter;

/* ---- StrTab helpers ---- */
static void strtab_init(StrTab *st) {
    st->cap = 256;
    st->data = malloc(st->cap);
    st->len = 0;
    /* First byte is always null */
    st->data[st->len++] = '\0';
}

static uint32_t strtab_add(StrTab *st, const char *s) {
    uint32_t off = st->len;
    size_t slen = strlen(s) + 1;
    while (st->len + slen > st->cap) {
        st->cap *= 2;
        st->data = realloc(st->data, st->cap);
        if (!st->data) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }
    }
    memcpy(st->data + st->len, s, slen);
    st->len += (uint32_t)slen;
    return off;
}

static void strtab_free(StrTab *st) {
    free(st->data);
    st->data = NULL;
    st->len = 0;
    st->cap = 0;
}

/* ---- Emitter lifecycle ---- */
static Emitter *emitter_create(void) {
    Emitter *em = calloc(1, sizeof(Emitter));
    if (!em) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }

    cb_init(&em->code);
    cb_init(&em->rodata);

    em->patches_cap = 64;
    em->patches = malloc(em->patches_cap * sizeof(BranchPatch));

    em->syms_cap = 32;
    em->syms = malloc(em->syms_cap * sizeof(ElfSym));

    em->relocs_cap = 32;
    em->relocs = malloc(em->relocs_cap * sizeof(ElfReloc));

    strtab_init(&em->strtab);
    strtab_init(&em->shstrtab);

    /* Add section names to shstrtab */
    strtab_add(&em->shstrtab, "");       /* offset 0: null */
    /* We'll add the actual names later when we know the layout */

    /* Null symbol (index 0) */
    ElfSym null_sym = {0};
    null_sym.name_off = 0;
    null_sym.bind = STB_LOCAL;
    null_sym.type = STT_NOTYPE;
    null_sym.shndx = SHN_UNDEF;
    if (em->nsyms >= em->syms_cap) {
        em->syms_cap *= 2;
        em->syms = realloc(em->syms, em->syms_cap * sizeof(ElfSym));
    }
    em->syms[em->nsyms++] = null_sym;

    return em;
}

static void emitter_destroy(Emitter *em) {
    cb_free(&em->code);
    cb_free(&em->rodata);
    free(em->label_offsets);
    free(em->patches);
    free(em->syms);
    free(em->relocs);
    strtab_free(&em->strtab);
    strtab_free(&em->shstrtab);
    free(em);
}

/* ---- Symbol management ---- */
static uint32_t emitter_add_sym(Emitter *em, const char *name, uint8_t bind,
                                uint8_t type, uint16_t shndx, uint64_t value,
                                uint64_t size) {
    uint32_t idx = em->nsyms;
    if (em->nsyms >= em->syms_cap) {
        em->syms_cap *= 2;
        em->syms = realloc(em->syms, em->syms_cap * sizeof(ElfSym));
        if (!em->syms) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }
    }
    em->syms[em->nsyms].name = name;
    em->syms[em->nsyms].name_off = strtab_add(&em->strtab, name);
    em->syms[em->nsyms].bind = bind;
    em->syms[em->nsyms].type = type;
    em->syms[em->nsyms].shndx = shndx;
    em->syms[em->nsyms].value = value;
    em->syms[em->nsyms].size = size;
    em->nsyms++;
    return idx;
}

static uint32_t emitter_find_sym(Emitter *em, const char *name) {
    for (uint32_t i = 0; i < em->nsyms; i++) {
        if (em->syms[i].name && strcmp(em->syms[i].name, name) == 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Ensure an external symbol exists, return its index */
static uint32_t emitter_ensure_extern(Emitter *em, const char *name) {
    uint32_t idx = emitter_find_sym(em, name);
    if (idx != UINT32_MAX) return idx;
    return emitter_add_sym(em, name, STB_GLOBAL, STT_NOTYPE, SHN_UNDEF, 0, 0);
}

/* ---- Relocation management ---- */
static void emitter_add_reloc(Emitter *em, uint32_t offset, uint32_t type,
                               uint32_t sym_idx, int64_t addend) {
    if (em->nrelocs >= em->relocs_cap) {
        em->relocs_cap *= 2;
        em->relocs = realloc(em->relocs, em->relocs_cap * sizeof(ElfReloc));
        if (!em->relocs) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }
    }
    em->relocs[em->nrelocs].offset = offset;
    em->relocs[em->nrelocs].type = type;
    em->relocs[em->nrelocs].sym_idx = sym_idx;
    em->relocs[em->nrelocs].addend = addend;
    em->nrelocs++;
}

/* ---- Branch patch management ---- */
static void emitter_add_patch(Emitter *em, size_t rel32_offset, uint32_t target_label) {
    if (em->npatches >= em->patches_cap) {
        em->patches_cap *= 2;
        em->patches = realloc(em->patches, em->patches_cap * sizeof(BranchPatch));
        if (!em->patches) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }
    }
    em->patches[em->npatches].rel32_offset = rel32_offset;
    em->patches[em->npatches].target_label = target_label;
    em->npatches++;
}

/* ================================================================
 * Part 3: x86-64 Encoding Helpers
 * ================================================================ */

/*
 * Register encoding:
 *   Our X86Reg enum: RAX=1, RCX=2, ..., RDI=8, R8=9, ..., R15=16
 *   x86 encoding:    low 3 bits = (reg - 1) & 7
 *   REX.B/R needed:  reg >= REG_R8 (9)
 */

/* Get the 3-bit encoding for a register */
static uint8_t reg_enc(X86Reg r) {
    if (r >= REG_XMM0) return (uint8_t)((r - REG_XMM0) & 7);
    if (r == REG_NONE || r == REG_RAX) return 0;
    return (uint8_t)((r - 1) & 7);
}

/* Does this register need REX.B or REX.R extension? */
static bool reg_ext(X86Reg r) {
    return (r >= REG_R8 && r <= REG_R15);
}

/* Is this a valid GPR? */
static bool reg_valid(X86Reg r) {
    return r >= REG_RAX && r <= REG_R15;
}

/* For 8-bit ops, do we need REX to access spl/bpl/sil/dil? */
static bool reg_needs_rex_8bit(X86Reg r) {
    return (r >= REG_RSP && r <= REG_RDI);
}

/* Build REX prefix byte */
static uint8_t make_rex(bool w, X86Reg reg, X86Reg rm) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;               /* REX.W */
    if (reg_ext(reg)) rex |= 0x04;    /* REX.R */
    if (reg_ext(rm)) rex |= 0x01;     /* REX.B */
    return rex;
}

/* Build ModR/M byte for register-register (mod=3) */
static uint8_t modrm_rr(X86Reg reg, X86Reg rm) {
    return 0xC0 | (reg_enc(reg) << 3) | reg_enc(rm);
}

/* Build ModR/M byte with raw opcode extension /digit (mod=3) */
static uint8_t modrm_ext_rr(uint8_t ext, X86Reg rm) {
    return 0xC0 | ((ext & 7) << 3) | reg_enc(rm);
}

/* Build ModR/M byte for [rm + disp32] with opcode extension (mod=2) */
static uint8_t modrm_ext_disp32(uint8_t ext, X86Reg rm) {
    return 0x80 | ((ext & 7) << 3) | reg_enc(rm);
}

/* Build ModR/M byte for [rm + disp8] with opcode extension (mod=1) */
static uint8_t modrm_ext_disp8(uint8_t ext, X86Reg rm) {
    return 0x40 | ((ext & 7) << 3) | reg_enc(rm);
}

/* Build ModR/M byte for [rm] with opcode extension (mod=0) */
static uint8_t modrm_ext_indirect(uint8_t ext, X86Reg rm) {
    return ((ext & 7) << 3) | reg_enc(rm);
}

/* REX prefix for opcode extension (no REX.R since ext is not a register) */
static uint8_t make_rex_ext(bool w, X86Reg rm) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;             /* REX.W */
    if (reg_ext(rm)) rex |= 0x01;   /* REX.B */
    return rex;
}

/* Build ModR/M byte for [rm + disp32] (mod=2) */
static uint8_t modrm_disp32(X86Reg reg, X86Reg rm) {
    return 0x80 | (reg_enc(reg) << 3) | reg_enc(rm);
}

/* Build ModR/M byte for [rm + disp8] (mod=1) */
static uint8_t modrm_disp8(X86Reg reg, X86Reg rm) {
    return 0x40 | (reg_enc(reg) << 3) | reg_enc(rm);
}

/* Build ModR/M byte for [rm] (mod=0) */
static uint8_t modrm_indirect(X86Reg reg, X86Reg rm) {
    return (reg_enc(reg) << 3) | reg_enc(rm);
}

/* Should we use 64-bit operand size for this type? */
static bool is_64bit_type(VtType *t) {
    if (!t) return true;  /* default to 64-bit */
    uint32_t sz = vt_type_size(t);
    return sz == 8 || sz == 0;  /* i64, u64, ptr, or unknown */
}

/* Should we use 8-bit operand size? */
static bool is_8bit_type(VtType *t) {
    if (!t) return false;
    return vt_type_size(t) == 1;
}

/* ================================================================
 * Part 4: Instruction Encoding Functions
 * ================================================================ */

/*
 * These functions append x86-64 machine code bytes directly to the
 * CodeBuf. No external assembler. No text emission. Pure binary.
 */

/* MOV r64, r64 (opcode 89: MOV r/m64, r64) */
static void enc_mov64(CodeBuf *cb, X86Reg dst, X86Reg src) {
    cb_push3(cb, make_rex(true, src, dst), 0x89, modrm_rr(src, dst));
}

/* MOV r32, r32 */
static void enc_mov32(CodeBuf *cb, X86Reg dst, X86Reg src) {
    if (reg_ext(dst) || reg_ext(src)) {
        cb_push3(cb, make_rex(false, src, dst), 0x89, modrm_rr(src, dst));
    } else {
        cb_push2(cb, 0x89, modrm_rr(src, dst));
    }
}

/* MOV r8, r8 */
static void enc_mov8(CodeBuf *cb, X86Reg dst, X86Reg src) {
    bool need_rex = reg_ext(dst) || reg_ext(src) ||
                    reg_needs_rex_8bit(dst) || reg_needs_rex_8bit(src);
    if (need_rex) {
        cb_push3(cb, make_rex(false, src, dst), 0x88, modrm_rr(src, dst));
    } else {
        cb_push2(cb, 0x88, modrm_rr(src, dst));
    }
}

/* MOV r64, imm32 (sign-extended to 64 bits) */
static void enc_mov64_imm32(CodeBuf *cb, X86Reg dst, int32_t imm) {
    cb_push2(cb, make_rex_ext(true, dst), 0xC7);
    cb_push1(cb, modrm_ext_rr(0, dst));   /* /0 */
    cb_push32le(cb, (uint32_t)imm);
}

/* MOV r64, imm64 (movabs - 10 bytes) */
static void enc_mov64_imm64(CodeBuf *cb, X86Reg dst, int64_t imm) {
    cb_push2(cb, make_rex(true, (X86Reg)0, dst), 0xB8 + reg_enc(dst));
    cb_push64le(cb, (uint64_t)imm);
}

/* MOV r32, imm32 */
static void enc_mov32_imm32(CodeBuf *cb, X86Reg dst, int32_t imm) {
    if (reg_ext(dst)) {
        cb_push2(cb, make_rex(false, (X86Reg)0, dst), 0xB8 + reg_enc(dst));
    } else {
        cb_push1(cb, 0xB8 + reg_enc(dst));
    }
    cb_push32le(cb, (uint32_t)imm);
}

/* MOV r8, imm8 */
static void enc_mov8_imm8(CodeBuf *cb, X86Reg dst, int8_t imm) {
    if (reg_ext(dst) || reg_needs_rex_8bit(dst)) {
        cb_push2(cb, make_rex(false, (X86Reg)0, dst), 0xB0 + reg_enc(dst));
    } else {
        cb_push1(cb, 0xB0 + reg_enc(dst));
    }
    cb_push1(cb, (uint8_t)imm);
}

/* ALU r64, r64 - generic for ADD(01), OR(09), AND(21), XOR(31), SUB(29) */
static void enc_alu64(CodeBuf *cb, uint8_t opcode, X86Reg dst, X86Reg src) {
    cb_push3(cb, make_rex(true, src, dst), opcode, modrm_rr(src, dst));
}

/* ALU r32, r32 */
static void enc_alu32(CodeBuf *cb, uint8_t opcode, X86Reg dst, X86Reg src) {
    if (reg_ext(dst) || reg_ext(src)) {
        cb_push3(cb, make_rex(false, src, dst), opcode, modrm_rr(src, dst));
    } else {
        cb_push2(cb, opcode, modrm_rr(src, dst));
    }
}

/* ADD r64, imm32 */
static void enc_add64_imm32(CodeBuf *cb, X86Reg dst, int32_t imm) {
    cb_push2(cb, make_rex_ext(true, dst), 0x81);
    cb_push1(cb, modrm_ext_rr(0, dst));   /* /0 = ADD */
    cb_push32le(cb, (uint32_t)imm);
}

/* SUB r64, imm32 */
static void enc_sub64_imm32(CodeBuf *cb, X86Reg dst, int32_t imm) {
    cb_push2(cb, make_rex_ext(true, dst), 0x81);
    cb_push1(cb, modrm_ext_rr(5, dst));   /* /5 = SUB */
    cb_push32le(cb, (uint32_t)imm);
}

/* ADD r64, imm8 */
static void enc_add64_imm8(CodeBuf *cb, X86Reg dst, int8_t imm) {
    cb_push2(cb, make_rex_ext(true, dst), 0x83);
    cb_push1(cb, modrm_ext_rr(0, dst));  /* /0 = ADD */
    cb_push1(cb, (uint8_t)imm);
}

/* SUB r64, imm8 */
static void enc_sub64_imm8(CodeBuf *cb, X86Reg dst, int8_t imm) {
    cb_push2(cb, make_rex_ext(true, dst), 0x83);
    cb_push1(cb, modrm_ext_rr(5, dst));  /* /5 = SUB */
    cb_push1(cb, (uint8_t)imm);
}

/* IMUL r64, r64 (0F AF /r) */
static void enc_imul64(CodeBuf *cb, X86Reg dst, X86Reg src) {
    cb_push4(cb, make_rex(true, dst, src), 0x0F, 0xAF, modrm_rr(dst, src));
}

/* IDIV r64 (F7 /7) */
static void enc_idiv64(CodeBuf *cb, X86Reg src) {
    cb_push3(cb, make_rex_ext(true, src), 0xF7, modrm_ext_rr(7, src));
}

/* NEG r64 (F7 /3) */
static void enc_neg64(CodeBuf *cb, X86Reg r) {
    cb_push3(cb, make_rex_ext(true, r), 0xF7, modrm_ext_rr(3, r));
}

/* NOT r64 (F7 /2) */
static void enc_not64(CodeBuf *cb, X86Reg r) {
    cb_push3(cb, make_rex_ext(true, r), 0xF7, modrm_ext_rr(2, r));
}

/* SHL r64, cl (D3 /4) */
static void enc_shl64_cl(CodeBuf *cb, X86Reg r) {
    cb_push3(cb, make_rex_ext(true, r), 0xD3, modrm_ext_rr(4, r));
}

/* SHR r64, cl (D3 /5) */
static void enc_shr64_cl(CodeBuf *cb, X86Reg r) {
    cb_push3(cb, make_rex_ext(true, r), 0xD3, modrm_ext_rr(5, r));
}

/* CMP r64, r64 (39 /r) */
static void enc_cmp64(CodeBuf *cb, X86Reg lhs, X86Reg rhs) {
    cb_push3(cb, make_rex(true, rhs, lhs), 0x39, modrm_rr(rhs, lhs));
}

/* CMP r64, imm32 (81 /7) */
static void enc_cmp64_imm32(CodeBuf *cb, X86Reg lhs, int32_t imm) {
    cb_push2(cb, make_rex_ext(true, lhs), 0x81);
    cb_push1(cb, modrm_ext_rr(7, lhs));  /* /7 = CMP */
    cb_push32le(cb, (uint32_t)imm);
}

/* TEST r64, r64 (85 /r) */
static void enc_test64(CodeBuf *cb, X86Reg lhs, X86Reg rhs) {
    cb_push3(cb, make_rex(true, rhs, lhs), 0x85, modrm_rr(rhs, lhs));
}

/* SETcc r8 (0F 9x /0, mod=3) */
static void enc_setcc(CodeBuf *cb, uint8_t cc, X86Reg dst) {
    bool need_rex = reg_ext(dst) || reg_needs_rex_8bit(dst);
    if (need_rex) {
        cb_push4(cb, make_rex_ext(false, dst), 0x0F, 0x90 + cc, modrm_ext_rr(0, dst));
    } else {
        cb_push3(cb, 0x0F, 0x90 + cc, modrm_ext_rr(0, dst));
    }
}

/* MOVZX r32, r8 (0F B6 /r) */
static void enc_movzx32_8(CodeBuf *cb, X86Reg dst, X86Reg src) {
    bool need_rex = reg_ext(dst) || reg_ext(src) || reg_needs_rex_8bit(src);
    if (need_rex) {
        cb_push4(cb, make_rex(false, dst, src), 0x0F, 0xB6, modrm_rr(dst, src));
    } else {
        cb_push3(cb, 0x0F, 0xB6, modrm_rr(dst, src));
    }
}

/* JMP rel32 (E9 cd) - 5 bytes */
static void enc_jmp32(CodeBuf *cb, int32_t rel32) {
    cb_push1(cb, 0xE9);
    cb_push32le(cb, (uint32_t)rel32);
}

/* JE rel32 (0F 84 cd) - 6 bytes */
static void enc_je32(CodeBuf *cb, int32_t rel32) {
    cb_push2(cb, 0x0F, 0x84);
    cb_push32le(cb, (uint32_t)rel32);
}

/* JNE rel32 (0F 85 cd) - 6 bytes */
static void enc_jne32(CodeBuf *cb, int32_t rel32) {
    cb_push2(cb, 0x0F, 0x85);
    cb_push32le(cb, (uint32_t)rel32);
}

/* Generic Jcc rel32 (0F 8x cd) - 6 bytes */
static void enc_jcc32(CodeBuf *cb, uint8_t cc, int32_t rel32) {
    cb_push2(cb, 0x0F, 0x80 + cc);
    cb_push32le(cb, (uint32_t)rel32);
}

/* CALL rel32 (E8 cd) - 5 bytes */
static void enc_call32(CodeBuf *cb, int32_t rel32) {
    cb_push1(cb, 0xE8);
    cb_push32le(cb, (uint32_t)rel32);
}

/* RET (C3) */
static void enc_ret(CodeBuf *cb) {
    cb_push1(cb, 0xC3);
}

/* PUSH r64 */
static void enc_push(CodeBuf *cb, X86Reg r) {
    if (reg_ext(r)) cb_push1(cb, 0x41);  /* REX.B */
    cb_push1(cb, 0x50 + reg_enc(r));
}

/* POP r64 */
static void enc_pop(CodeBuf *cb, X86Reg r) {
    if (reg_ext(r)) cb_push1(cb, 0x41);  /* REX.B */
    cb_push1(cb, 0x58 + reg_enc(r));
}

/* NOP (90) */
static void enc_nop(CodeBuf *cb) {
    cb_push1(cb, 0x90);
}

/* CQO (sign-extend RAX into RDX:RAX) - REX.W 99 */
static void enc_cqo(CodeBuf *cb) {
    cb_push2(cb, 0x48, 0x99);
}

/* XORPS xmm, xmm (for zeroing XMM) */
static void enc_xorps(CodeBuf *cb, X86Reg dst, X86Reg src) {
    bool need_rex = reg_ext(dst) || reg_ext(src);
    if (need_rex) {
        cb_push3(cb, make_rex(false, src, dst), 0x0F, 0x57);
        cb_push1(cb, modrm_rr(src, dst));
    } else {
        cb_push3(cb, 0x0F, 0x57, modrm_rr(src, dst));
    }
}

/* LEA r64, [rip + disp32] - 7 bytes (RIP-relative) */
static void enc_lea64_rip(CodeBuf *cb, X86Reg dst, int32_t disp32) {
    cb_push2(cb, make_rex(true, dst, (X86Reg)0), 0x8D);
    /* mod=0, reg=dst, rm=5 (RIP-relative) - rm=5 is a raw encoding, NOT a register */
    cb_push1(cb, (uint8_t)((reg_enc(dst) << 3) | 5));
    cb_push32le(cb, (uint32_t)disp32);
}

/* LEA r64, [base + disp32] */
static void enc_lea64_disp32(CodeBuf *cb, X86Reg dst, X86Reg base, int32_t disp) {
    cb_push2(cb, make_rex(true, dst, base), 0x8D);
    /* Special cases for RSP/R13 (need SIB) and RBP/R13 (mod=0 with rm=5 = RIP) */
    if (base == REG_RSP || base == REG_R12) {
        cb_push1(cb, modrm_disp32(dst, (X86Reg)4));  /* rm=4 means SIB follows */
        cb_push1(cb, 0x24);                           /* SIB: scale=0, index=none, base=rsp */
        cb_push32le(cb, (uint32_t)disp);
    } else if (base == REG_RBP || base == REG_R13) {
        /* mod=0 with rm=5 would mean RIP-relative, so use mod=2 (disp32) */
        cb_push1(cb, modrm_disp32(dst, base));
        cb_push32le(cb, (uint32_t)disp);
    } else {
        cb_push1(cb, modrm_disp32(dst, base));
        cb_push32le(cb, (uint32_t)disp);
    }
}

/* LEA r64, [base + disp8] */
static void enc_lea64_disp8(CodeBuf *cb, X86Reg dst, X86Reg base, int8_t disp) {
    cb_push2(cb, make_rex(true, dst, base), 0x8D);
    if (base == REG_RSP || base == REG_R12) {
        cb_push1(cb, modrm_disp8(dst, (X86Reg)4));
        cb_push1(cb, 0x24);
        cb_push1(cb, (uint8_t)disp);
    } else {
        cb_push1(cb, modrm_disp8(dst, base));
        cb_push1(cb, (uint8_t)disp);
    }
}

/* MOV [base + disp32], r64 (store) - REX.W 89 /r */
static void enc_store64(CodeBuf *cb, X86Reg base, X86Reg src, int32_t disp) {
    cb_push2(cb, make_rex(true, src, base), 0x89);
    if (base == REG_RSP || base == REG_R12) {
        cb_push1(cb, modrm_disp32(src, (X86Reg)4));
        cb_push1(cb, 0x24);
        cb_push32le(cb, (uint32_t)disp);
    } else if (base == REG_RBP || base == REG_R13) {
        cb_push1(cb, modrm_disp32(src, base));
        cb_push32le(cb, (uint32_t)disp);
    } else if (disp == 0) {
        cb_push1(cb, modrm_indirect(src, base));
    } else {
        cb_push1(cb, modrm_disp32(src, base));
        cb_push32le(cb, (uint32_t)disp);
    }
}

/* MOV r64, [base + disp32] (load) - REX.W 8B /r */
static void enc_load64(CodeBuf *cb, X86Reg dst, X86Reg base, int32_t disp) {
    cb_push2(cb, make_rex(true, dst, base), 0x8B);
    if (base == REG_RSP || base == REG_R12) {
        cb_push1(cb, modrm_disp32(dst, (X86Reg)4));
        cb_push1(cb, 0x24);
        cb_push32le(cb, (uint32_t)disp);
    } else if (base == REG_RBP || base == REG_R13) {
        cb_push1(cb, modrm_disp32(dst, base));
        cb_push32le(cb, (uint32_t)disp);
    } else if (disp == 0) {
        cb_push1(cb, modrm_indirect(dst, base));
    } else {
        cb_push1(cb, modrm_disp32(dst, base));
        cb_push32le(cb, (uint32_t)disp);
    }
}

/* ================================================================
 * Part 5: Prologue / Epilogue Encoding
 * ================================================================ */

static void enc_prologue(CodeBuf *cb, MachFunc *mf, RegAllocResult *ra) {
    /* push rbp */
    enc_push(cb, REG_RBP);

    /* mov rbp, rsp */
    enc_mov64(cb, REG_RBP, REG_RSP);

    /* sub rsp, stack_size */
    uint32_t stack_size = ra->stack_size;
    if (stack_size > 0) {
        if (stack_size <= 127) {
            enc_sub64_imm8(cb, REG_RSP, (int8_t)stack_size);
        } else {
            enc_sub64_imm32(cb, REG_RSP, (int32_t)stack_size);
        }
    }

    /* Save callee-saved registers that are in use */
    for (int r = REG_RBX; r <= REG_R15; r++) {
        if (reg_is_callee_saved((X86Reg)r) && r != REG_RBP && ra->reg_used[r]) {
            enc_push(cb, (X86Reg)r);
        }
    }

    /* Move parameters from calling convention registers to their assigned regs */
    static const X86Reg param_regs[] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9 };
    for (uint32_t i = 0; i < mf->nparams && i < 6; i++) {
        uint32_t pvreg = mf->param_vregs[i];
        if (pvreg < ra->num_vregs) {
            X86Reg assigned = ra->vreg_to_reg[pvreg];
            if (assigned != REG_NONE && assigned != param_regs[i] && reg_valid(assigned)) {
                enc_mov64(cb, assigned, param_regs[i]);
            }
        }
    }
}

static void enc_epilogue(CodeBuf *cb, RegAllocResult *ra) {
    /* Restore callee-saved registers in reverse order */
    for (int r = REG_R15; r >= (int)REG_RBX; r--) {
        if (reg_is_callee_saved((X86Reg)r) && r != REG_RBP && ra->reg_used[r]) {
            enc_pop(cb, (X86Reg)r);
        }
    }

    /* mov rsp, rbp */
    enc_mov64(cb, REG_RSP, REG_RBP);

    /* pop rbp */
    enc_pop(cb, REG_RBP);

    /* ret */
    enc_ret(cb);
}

/* ================================================================
 * Part 6: Per-Instruction Emission
 * ================================================================ */

/*
 * Emit a single machine instruction as binary x86-64 code.
 * Branch targets use placeholder offsets that are patched later.
 * External function calls generate relocations.
 */
static void emit_inst_binary(Emitter *em, MachInst *mi, RegAllocResult *ra) {
    CodeBuf *cb = &em->code;

    switch (mi->op) {

    case MINST_LABEL:
        /* Label offset is recorded before we process the block */
        break;

    case MINST_NOP:
        enc_nop(cb);
        break;

    case MINST_MOV: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        if (!reg_valid(dst) || !reg_valid(src)) break;
        if (dst == src) break;  /* redundant move */
        if (is_64bit_type(mi->type)) {
            enc_mov64(cb, dst, src);
        } else if (is_8bit_type(mi->type)) {
            enc_mov8(cb, dst, src);
        } else {
            enc_mov32(cb, dst, src);
        }
        break;
    }

    case MINST_MOV_IMM: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        int64_t imm = mi->imm;
        if (is_64bit_type(mi->type)) {
            /* Use movabs for values that don't fit in 32-bit sign extension */
            if (imm < -2147483648LL || imm > 2147483647LL) {
                enc_mov64_imm64(cb, dst, imm);
            } else {
                enc_mov64_imm32(cb, dst, (int32_t)imm);
            }
        } else if (is_8bit_type(mi->type)) {
            enc_mov8_imm8(cb, dst, (int8_t)imm);
        } else {
            enc_mov32_imm32(cb, dst, (int32_t)imm);
        }
        break;
    }

    case MINST_ADD: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        X86Reg src2 = (X86Reg)mi->src2;
        if (!reg_valid(dst) || !reg_valid(src2)) break;
        /* 3-operand: dst = src + src2. Decompose to mov + add */
        if (dst != src && reg_valid(src)) {
            enc_mov64(cb, dst, src);
        }
        enc_alu64(cb, 0x01, dst, src2);   /* ADD r/m64, r64 */
        break;
    }

    case MINST_ADD_IMM: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        int64_t imm = mi->imm;
        if (imm >= -128 && imm <= 127) {
            enc_add64_imm8(cb, dst, (int8_t)imm);
        } else {
            enc_add64_imm32(cb, dst, (int32_t)imm);
        }
        break;
    }

    case MINST_SUB: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        X86Reg src2 = (X86Reg)mi->src2;
        if (!reg_valid(dst) || !reg_valid(src2)) break;
        if (dst != src && reg_valid(src)) {
            enc_mov64(cb, dst, src);
        }
        enc_alu64(cb, 0x29, dst, src2);   /* SUB r/m64, r64 */
        break;
    }

    case MINST_SUB_IMM: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        int64_t imm = mi->imm;
        if (imm >= -128 && imm <= 127) {
            enc_sub64_imm8(cb, dst, (int8_t)imm);
        } else {
            enc_sub64_imm32(cb, dst, (int32_t)imm);
        }
        break;
    }

    case MINST_MUL: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        X86Reg src2 = (X86Reg)mi->src2;
        if (!reg_valid(dst) || !reg_valid(src2)) break;
        /* IMUL dst, src2 - but we need dst = src * src2 */
        if (dst != src && reg_valid(src)) {
            enc_mov64(cb, dst, src);
        }
        enc_imul64(cb, dst, src2);
        break;
    }

    case MINST_MUL_IMM: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        /* IMUL r64, imm32 - 3-operand form: imul dst, dst, imm */
        /* Actually IMUL r64, r/m64, imm32 = REX.W 69 /r id */
        cb_push2(cb, make_rex(true, dst, dst), 0x69);
        cb_push1(cb, modrm_rr(dst, dst));
        cb_push32le(cb, (uint32_t)mi->imm);
        break;
    }

    case MINST_DIV: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg lhs = (X86Reg)mi->src;
        X86Reg rhs = (X86Reg)mi->src2;
        if (!reg_valid(rhs)) break;
        X86Reg lhs_reg = reg_valid(lhs) ? lhs : REG_RAX;
        /* Move lhs to RAX if not already there */
        if (lhs_reg != REG_RAX) {
            enc_mov64(cb, REG_RAX, lhs_reg);
        }
        /* CQO: sign-extend RAX into RDX:RAX */
        enc_cqo(cb);
        /* IDIV rhs */
        enc_idiv64(cb, rhs);
        /* Move result from RAX to dst if needed */
        if (reg_valid(dst) && dst != REG_RAX) {
            enc_mov64(cb, dst, REG_RAX);
        }
        break;
    }

    case MINST_AND: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        X86Reg src2 = (X86Reg)mi->src2;
        if (!reg_valid(dst) || !reg_valid(src2)) break;
        if (dst != src && reg_valid(src)) {
            enc_mov64(cb, dst, src);
        }
        enc_alu64(cb, 0x21, dst, src2);   /* AND r/m64, r64 */
        break;
    }

    case MINST_OR: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        X86Reg src2 = (X86Reg)mi->src2;
        if (!reg_valid(dst) || !reg_valid(src2)) break;
        if (dst != src && reg_valid(src)) {
            enc_mov64(cb, dst, src);
        }
        enc_alu64(cb, 0x09, dst, src2);   /* OR r/m64, r64 */
        break;
    }

    case MINST_XOR: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        X86Reg src2 = (X86Reg)mi->src2;
        if (!reg_valid(dst) || !reg_valid(src2)) break;
        /* Special case: xor reg, reg = zero */
        if (dst == src2 || (reg_valid(src) && src == src2)) {
            enc_alu64(cb, 0x31, dst, src2);  /* XOR r/m64, r64 */
        } else {
            if (dst != src && reg_valid(src)) {
                enc_mov64(cb, dst, src);
            }
            enc_alu64(cb, 0x31, dst, src2);
        }
        break;
    }

    case MINST_SHL: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        if (!reg_valid(dst)) break;
        /* src2 should be in CL (RCX) */
        if (reg_valid(src) && dst != src) {
            enc_mov64(cb, dst, src);
        }
        enc_shl64_cl(cb, dst);
        break;
    }

    case MINST_SHR: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        if (!reg_valid(dst)) break;
        if (reg_valid(src) && dst != src) {
            enc_mov64(cb, dst, src);
        }
        enc_shr64_cl(cb, dst);
        break;
    }

    case MINST_NEG: {
        X86Reg r = (X86Reg)mi->src;
        if (!reg_valid(r)) break;
        enc_neg64(cb, r);
        break;
    }

    case MINST_NOT: {
        X86Reg r = (X86Reg)mi->src;
        if (!reg_valid(r)) break;
        enc_not64(cb, r);
        break;
    }

    case MINST_CMP: {
        X86Reg lhs = (X86Reg)mi->src;
        X86Reg rhs = (X86Reg)mi->src2;
        if (!reg_valid(lhs) || !reg_valid(rhs)) break;
        enc_cmp64(cb, lhs, rhs);
        break;
    }

    case MINST_CMP_IMM: {
        X86Reg lhs = (X86Reg)mi->src;
        if (!reg_valid(lhs)) break;
        enc_cmp64_imm32(cb, lhs, (int32_t)mi->imm);
        break;
    }

    case MINST_TEST: {
        X86Reg lhs = (X86Reg)mi->src;
        X86Reg rhs = (X86Reg)mi->src2;
        if (!reg_valid(lhs) || !reg_valid(rhs)) break;
        enc_test64(cb, lhs, rhs);
        break;
    }

    case MINST_SET_EQ: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        enc_setcc(cb, 0x04, dst);  /* 0x04 = SETE */
        enc_movzx32_8(cb, dst, dst);
        break;
    }

    case MINST_SET_NEQ: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        enc_setcc(cb, 0x05, dst);  /* 0x05 = SETNE */
        enc_movzx32_8(cb, dst, dst);
        break;
    }

    case MINST_SET_LT: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        enc_setcc(cb, 0x0C, dst);  /* 0x0C = SETL */
        enc_movzx32_8(cb, dst, dst);
        break;
    }

    case MINST_SET_GT: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        enc_setcc(cb, 0x0F, dst);  /* 0x0F = SETG */
        enc_movzx32_8(cb, dst, dst);
        break;
    }

    case MINST_SET_LEQ: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        enc_setcc(cb, 0x0E, dst);  /* 0x0E = SETLE */
        enc_movzx32_8(cb, dst, dst);
        break;
    }

    case MINST_SET_GEQ: {
        X86Reg dst = (X86Reg)mi->dst;
        if (!reg_valid(dst)) break;
        enc_setcc(cb, 0x0D, dst);  /* 0x0D = SETGE */
        enc_movzx32_8(cb, dst, dst);
        break;
    }

    case MINST_JMP: {
        /* JMP rel32 - emit placeholder, record patch */
        cb_push1(cb, 0xE9);
        size_t rel32_off = cb->len;
        cb_push32le(cb, 0);  /* placeholder */
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_JE: {
        cb_push2(cb, 0x0F, 0x84);
        size_t rel32_off = cb->len;
        cb_push32le(cb, 0);
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_JNE: {
        cb_push2(cb, 0x0F, 0x85);
        size_t rel32_off = cb->len;
        cb_push32le(cb, 0);
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_JL: {
        enc_jcc32(cb, 0x0C, 0);  /* JL = 0F 8C */
        size_t rel32_off = cb->len - 4;
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_JG: {
        enc_jcc32(cb, 0x0F, 0);  /* JG = 0F 8F */
        size_t rel32_off = cb->len - 4;
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_JLE: {
        enc_jcc32(cb, 0x0E, 0);  /* JLE = 0F 8E */
        size_t rel32_off = cb->len - 4;
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_JGE: {
        enc_jcc32(cb, 0x0D, 0);  /* JGE = 0F 8D */
        size_t rel32_off = cb->len - 4;
        emitter_add_patch(em, rel32_off, mi->target_bb);
        break;
    }

    case MINST_CALL: {
        /* Move arguments to calling convention registers */
        static const X86Reg arg_regs[] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9 };
        if (mi->arg_vregs && ra) {
            for (uint32_t j = 0; j < mi->nargs && j < 6; j++) {
                X86Reg target = arg_regs[j];
                X86Reg src = REG_NONE;
                if (mi->arg_vregs[j] < ra->num_vregs) {
                    src = ra->vreg_to_reg[mi->arg_vregs[j]];
                }
                if (reg_valid(src) && src != target) {
                    enc_mov64(cb, target, src);
                }
            }
        }

        /* CALL func_name - emit as CALL rel32 with relocation */
        cb_push1(cb, 0xE8);
        size_t rel32_off = cb->len;
        cb_push32le(cb, 0xFFFFFFFF);  /* placeholder - will be filled by linker */

        /* Create a PLT32 relocation for the call target */
        const char *func_name = mi->func_name ? mi->func_name : "???";
        uint32_t sym_idx = emitter_ensure_extern(em, func_name);
        emitter_add_reloc(em, (uint32_t)rel32_off, R_X86_64_PLT32, sym_idx, -4);
        break;
    }

    case MINST_RET: {
        /* Move return value to RAX if not already there */
        X86Reg src = (X86Reg)mi->src;
        if (reg_valid(src) && src != REG_RAX) {
            enc_mov64(cb, REG_RAX, src);
        }
        enc_epilogue(cb, ra);
        break;
    }

    case MINST_LEA: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg base = (X86Reg)mi->src;
        if (!reg_valid(dst)) break;
        if (!reg_valid(base)) {
            /* If base is invalid, just load the offset as immediate */
            enc_mov64_imm32(cb, dst, (int32_t)mi->offset);
            break;
        }
        int64_t disp = mi->offset;
        if (disp == 0 && base != REG_RBP && base != REG_R13 && base != REG_RSP && base != REG_R12) {
            /* LEA dst, [base] = MOV dst, base (when offset is 0) */
            enc_mov64(cb, dst, base);
        } else if (disp >= -128 && disp <= 127) {
            enc_lea64_disp8(cb, dst, base, (int8_t)disp);
        } else {
            enc_lea64_disp32(cb, dst, base, (int32_t)disp);
        }
        break;
    }

    case MINST_PUSH: {
        X86Reg src = (X86Reg)mi->src;
        if (reg_valid(src)) {
            enc_push(cb, src);
        }
        break;
    }

    case MINST_POP: {
        X86Reg dst = (X86Reg)mi->dst;
        if (reg_valid(dst)) {
            enc_pop(cb, dst);
        }
        break;
    }

    case MINST_MOVZX: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        if (!reg_valid(dst) || !reg_valid(src)) break;
        enc_movzx32_8(cb, dst, src);
        break;
    }

    case MINST_XORPS: {
        X86Reg dst = (X86Reg)mi->dst;
        X86Reg src = (X86Reg)mi->src;
        if (!reg_valid(dst) || !reg_valid(src)) break;
        enc_xorps(cb, dst, src);
        break;
    }

    case MINST_SYSCALL: {
        cb_push2(cb, 0x0F, 0x05);
        break;
    }

    default:
        /* Unhandled opcode - emit a breakpoint (INT3) for debugging */
        cb_push1(cb, 0xCC);
        break;
    }
}

/* ================================================================
 * Part 7: Function Emission Loop
 * ================================================================ */

static void emit_func_binary(Emitter *em, MachFunc *mf, RegAllocResult *ra) {
    CodeBuf *cb = &em->code;

    /* Reset per-function patch state */
    em->npatches = 0;

    /* Allocate label offset table */
    free(em->label_offsets);
    em->nlabels = mf->nblocks;
    em->label_offsets = calloc(mf->nblocks, sizeof(uint32_t));
    if (!em->label_offsets) { fprintf(stderr, "vortech: out of memory\n"); exit(1); }

    /* Record function symbol */
    uint64_t func_start = cb->len;
    emitter_add_sym(em, mf->name, STB_GLOBAL, STT_FUNC, 1 /* .text section */, func_start, 0);

    /* Emit prologue */
    enc_prologue(cb, mf, ra);

    /* Emit each basic block */
    for (uint32_t bi = 0; bi < mf->nblocks; bi++) {
        MachBlock *mb = &mf->blocks[bi];

        /* Record label offset */
        em->label_offsets[bi] = (uint32_t)cb->len;

        for (uint32_t ii = 0; ii < mb->ninsts; ii++) {
            MachInst *mi = &mb->insts[ii];
            if (mi->op == MINST_LABEL) continue;  /* label is implicit at block start */
            emit_inst_binary(em, mi, ra);
        }
    }

    /* Fallback epilogue (in case no explicit RET) */
    enc_mov64_imm32(cb, REG_RAX, 0);
    enc_epilogue(cb, ra);

    /* Resolve branch patches */
    for (uint32_t i = 0; i < em->npatches; i++) {
        BranchPatch *p = &em->patches[i];
        if (p->target_label < em->nlabels) {
            uint32_t target_off = em->label_offsets[p->target_label];
            /* rel32 = target - (patch_pos + 4) */
            int32_t rel = (int32_t)(target_off - (p->rel32_offset + 4));
            cb_patch32(cb, p->rel32_offset, (uint32_t)rel);
        }
    }

    /* Update function size in symbol table */
    uint64_t func_end = cb->len;
    em->syms[em->nsyms - 1].size = func_end - func_start;
}

/* ================================================================
 * Part 8: Print Helper Runtime
 * ================================================================ */

static void emit_print_helper(Emitter *em) {
    CodeBuf *cb = &em->code;
    uint64_t func_start = cb->len;

    /* Add format string to .rodata */
    uint32_t fmt_off = (uint32_t)em->rodata.len;
    const char *fmt = "%lld\n";
    for (const char *p = fmt; *p; p++) {
        cb_push1(&em->rodata, (uint8_t)*p);
    }
    cb_push1(&em->rodata, 0);  /* null terminator */

    /* Emit __vortech_print_i64 function */
    emitter_add_sym(em, "__vortech_print_i64", STB_GLOBAL, STT_FUNC, 1, func_start, 0);

    /* push rbp */
    enc_push(cb, REG_RBP);
    /* mov rbp, rsp */
    enc_mov64(cb, REG_RBP, REG_RSP);
    /* mov rsi, rdi (arg1 -> printf arg2) */
    enc_mov64(cb, REG_RSI, REG_RDI);
    /* lea rdi, [rip + .print_fmt] - RIP-relative to .rodata */
    enc_lea64_rip(cb, REG_RDI, 0);  /* placeholder - needs reloc */
    /* Add R_X86_64_PC32 relocation for the format string reference */
    /* The rel32 is at the last 4 bytes of the LEA instruction */
    size_t lea_rel32_off = cb->len - 4;
    /* We need a symbol for the .rodata section */
    uint32_t rodata_sym_idx = emitter_find_sym(em, ".rodata");
    if (rodata_sym_idx == UINT32_MAX) {
        rodata_sym_idx = emitter_add_sym(em, ".rodata", STB_GLOBAL, STT_NOTYPE, 2 /* .rodata section */, 0, 0);
    }
    emitter_add_reloc(em, (uint32_t)lea_rel32_off, R_X86_64_PC32, rodata_sym_idx, (int32_t)fmt_off - 4);

    /* xor eax, eax (0 variadic FP args) */
    cb_push2(cb, 0x31, 0xC0);
    /* call printf */
    cb_push1(cb, 0xE8);
    size_t call_rel32_off = cb->len;
    cb_push32le(cb, 0);  /* placeholder */
    uint32_t printf_sym = emitter_ensure_extern(em, "printf");
    emitter_add_reloc(em, (uint32_t)call_rel32_off, R_X86_64_PLT32, printf_sym, -4);

    /* mov rsp, rbp */
    enc_mov64(cb, REG_RSP, REG_RBP);
    /* pop rbp */
    enc_pop(cb, REG_RBP);
    /* ret */
    enc_ret(cb);

    /* Update function size */
    uint64_t func_end = cb->len;
    em->syms[em->nsyms - 1].size = func_end - func_start;
}

/* ================================================================
 * Part 9: ELF64 Object File Writer
 * ================================================================ */

/*
 * Layout of the ELF64 relocatable object file:
 *
 *   [0]         ELF Header          (64 bytes)
 *   [64]        .text section       (code bytes)
 *   [aligned]   .rodata section     (read-only data)
 *   [aligned]   .rela.text section  (relocations for .text)
 *   [aligned]   .symtab section     (symbol table)
 *   [aligned]   .strtab section     (symbol name strings)
 *   [aligned]   .shstrtab section   (section name strings)
 *   [aligned]   Section Headers     (7 entries x 64 bytes)
 *
 * Sections:
 *   0: NULL
 *   1: .text
 *   2: .rodata (skip if empty, but we always have the format string)
 *   3: .rela.text
 *   4: .symtab
 *   5: .strtab
 *   6: .shstrtab
 */

#define SEC_NULL   0
#define SEC_TEXT   1
#define SEC_RODATA 2
#define SEC_RELA   3
#define SEC_SYMTAB 4
#define SEC_STRTAB 5
#define SEC_SHSTRTAB 6
#define NUM_SECTIONS 7

static void write_elf_header(uint8_t *buf, uint64_t shoff, uint16_t shnum, uint16_t shstrndx) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    memset(ehdr, 0, sizeof(Elf64_Ehdr));

    ehdr->e_ident[0] = ELFMAG0;
    ehdr->e_ident[1] = ELFMAG1;
    ehdr->e_ident[2] = ELFMAG2;
    ehdr->e_ident[3] = ELFMAG3;
    ehdr->e_ident[4] = ELFCLASS64;
    ehdr->e_ident[5] = ELFDATA2LSB;
    ehdr->e_ident[6] = EV_CURRENT;
    ehdr->e_ident[7] = ELFOSABI_NONE;

    ehdr->e_type = ET_REL;
    ehdr->e_machine = EM_X86_64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0;
    ehdr->e_phoff = 0;
    ehdr->e_shoff = shoff;
    ehdr->e_flags = 0;
    ehdr->e_ehsize = 64;
    ehdr->e_phentsize = 0;
    ehdr->e_phnum = 0;
    ehdr->e_shentsize = 64;
    ehdr->e_shnum = shnum;
    ehdr->e_shstrndx = shstrndx;
}

static void write_section_header(uint8_t *buf, uint64_t shdr_off, uint32_t idx,
                                  uint32_t name, uint32_t type,
                                  uint64_t flags, uint64_t addr,
                                  uint64_t offset, uint64_t size,
                                  uint32_t link, uint32_t info,
                                  uint64_t addralign, uint64_t entsize) {
    Elf64_Shdr *shdr = (Elf64_Shdr *)(buf + shdr_off + 64 * idx);
    memset(shdr, 0, 64);
    shdr->sh_name = name;
    shdr->sh_type = type;
    shdr->sh_flags = flags;
    shdr->sh_addr = addr;
    shdr->sh_offset = offset;
    shdr->sh_size = size;
    shdr->sh_link = link;
    shdr->sh_info = info;
    shdr->sh_addralign = addralign;
    shdr->sh_entsize = entsize;
}

static bool emit_write_elf(const char *path, Emitter *em) {
    /* Build section name string table */
    StrTab shstrtab;
    strtab_init(&shstrtab);
    uint32_t shstr_null  = strtab_add(&shstrtab, "");           /* 0 */
    uint32_t shstr_text  = strtab_add(&shstrtab, ".text");      /* 1 */
    uint32_t shstr_rodata = strtab_add(&shstrtab, ".rodata");   /* 8 or wherever */
    uint32_t shstr_rela  = strtab_add(&shstrtab, ".rela.text"); /* 16 or wherever */
    uint32_t shstr_symtab = strtab_add(&shstrtab, ".symtab");   /* etc */
    uint32_t shstr_strtab = strtab_add(&shstrtab, ".strtab");
    uint32_t shstr_shstrtab = strtab_add(&shstrtab, ".shstrtab");

    /* Calculate layout */
    uint64_t off = 64;  /* after ELF header */

    /* .text section */
    uint64_t text_off = off;
    uint64_t text_size = em->code.len;
    off += text_size;
    off = (off + 15) & ~15ULL;  /* 16-byte align */

    /* .rodata section */
    uint64_t rodata_off = off;
    uint64_t rodata_size = em->rodata.len;
    off += rodata_size;
    off = (off + 15) & ~15ULL;

    /* .rela.text section */
    uint64_t rela_off = off;
    uint64_t rela_size = em->nrelocs * 24;  /* each Elf64_Rela is 24 bytes */
    off += rela_size;
    off = (off + 7) & ~7ULL;  /* 8-byte align */

    /* .symtab section */
    uint64_t symtab_off = off;
    uint64_t symtab_size = em->nsyms * 24;  /* each Elf64_Sym is 24 bytes */
    off += symtab_size;
    off = (off + 7) & ~7ULL;

    /* .strtab section */
    uint64_t strtab_off = off;
    uint64_t strtab_size = em->strtab.len;
    off += strtab_size;
    off = (off + 7) & ~7ULL;

    /* .shstrtab section */
    uint64_t shstrtab_off = off;
    uint64_t shstrtab_size = shstrtab.len;
    off += shstrtab_size;
    off = (off + 15) & ~15ULL;

    /* Section headers */
    uint64_t shdr_off = off;

    /* Total file size */
    size_t file_size = shdr_off + NUM_SECTIONS * 64;

    /* Allocate file buffer */
    uint8_t *file_buf = calloc(1, file_size);
    if (!file_buf) {
        fprintf(stderr, "vortech: out of memory writing ELF\n");
        strtab_free(&shstrtab);
        return false;
    }

    /* Write ELF header */
    write_elf_header(file_buf, shdr_off, NUM_SECTIONS, SEC_SHSTRTAB);

    /* Write .text section */
    memcpy(file_buf + text_off, em->code.data, text_size);

    /* Write .rodata section */
    if (rodata_size > 0) {
        memcpy(file_buf + rodata_off, em->rodata.data, rodata_size);
    }

    /* Write .rela.text section */
    for (uint32_t i = 0; i < em->nrelocs; i++) {
        Elf64_Rela rela;
        rela.r_offset = em->relocs[i].offset;
        rela.r_info = ELF64_R_INFO(em->relocs[i].sym_idx, em->relocs[i].type);
        rela.r_addend = em->relocs[i].addend;
        memcpy(file_buf + rela_off + i * 24, &rela, 24);
    }

    /* Write .symtab section */
    for (uint32_t i = 0; i < em->nsyms; i++) {
        Elf64_Sym sym;
        memset(&sym, 0, 24);
        sym.st_name = em->syms[i].name_off;
        sym.st_info = ELF64_ST_INFO(em->syms[i].bind, em->syms[i].type);
        sym.st_other = 0;
        sym.st_shndx = em->syms[i].shndx;
        sym.st_value = em->syms[i].value;
        sym.st_size = em->syms[i].size;
        memcpy(file_buf + symtab_off + i * 24, &sym, 24);
    }

    /* Write .strtab section */
    memcpy(file_buf + strtab_off, em->strtab.data, strtab_size);

    /* Write .shstrtab section */
    memcpy(file_buf + shstrtab_off, shstrtab.data, shstrtab_size);

    /* Write section headers */
    /* Section 0: NULL */
    write_section_header(file_buf, shdr_off, 0, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);

    /* Section 1: .text */
    write_section_header(file_buf, shdr_off, 1, shstr_text, SHT_PROGBITS,
                         SHF_ALLOC | SHF_EXECINSTR, 0, text_off, text_size,
                         0, 0, 16, 0);

    /* Section 2: .rodata */
    write_section_header(file_buf, shdr_off, 2, shstr_rodata, SHT_PROGBITS,
                         SHF_ALLOC, 0, rodata_off, rodata_size,
                         0, 0, 8, 0);

    /* Section 3: .rela.text */
    /* sh_link = symbol table section index, sh_info = .text section index */
    write_section_header(file_buf, shdr_off, 3, shstr_rela, SHT_RELA,
                         0, 0, rela_off, rela_size,
                         SEC_SYMTAB, SEC_TEXT, 8, 24);

    /* Section 4: .symtab */
    /* sh_link = .strtab section index
     * sh_info = one greater than index of last local symbol.
     * We keep the null symbol as local, all others as global. */
    write_section_header(file_buf, shdr_off, 4, shstr_symtab, SHT_SYMTAB,
                         0, 0, symtab_off, symtab_size,
                         SEC_STRTAB, 1, 8, 24);

    /* Section 5: .strtab */
    write_section_header(file_buf, shdr_off, 5, shstr_strtab, SHT_STRTAB,
                         0, 0, strtab_off, strtab_size,
                         0, 0, 1, 0);

    /* Section 6: .shstrtab */
    write_section_header(file_buf, shdr_off, 6, shstr_shstrtab, SHT_STRTAB,
                         0, 0, shstrtab_off, shstrtab_size,
                         0, 0, 1, 0);

    /* Write the file */
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "vortech: cannot create object file '%s'\n", path);
        free(file_buf);
        strtab_free(&shstrtab);
        return false;
    }

    size_t written = fwrite(file_buf, 1, file_size, f);
    fclose(f);

    free(file_buf);
    strtab_free(&shstrtab);

    if (written != file_size) {
        fprintf(stderr, "vortech: failed to write complete object file\n");
        return false;
    }

    return true;
}

/* ================================================================
 * Part 11: Public API
 * ================================================================ */

bool emit_object(const char *path, MachFunc *funcs, uint32_t nfuncs,
                 RegAllocResult **ras) {
    Emitter *em = emitter_create();

    /* Emit each function's machine code */
    for (uint32_t i = 0; i < nfuncs; i++) {
        emit_func_binary(em, &funcs[i], ras[i]);
    }

    /* Emit the print helper runtime function */
    emit_print_helper(em);

    /* Write the ELF64 object file */
    bool ok = emit_write_elf(path, em);

    emitter_destroy(em);
    return ok;
}
