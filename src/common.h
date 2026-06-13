/*
 * VORTECH Compiler - Common Definitions
 * Maximum practical performance with minimum compiler complexity.
 */
#ifndef VORTECH_COMMON_H
#define VORTECH_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

/* ---- Configuration ---- */
#define VT_MAX_IDENT_LEN     256
#define VT_MAX_STRING_LEN    4096
#define VT_MAX_PARAMS        32
#define VT_MAX_ARGS          32
#define VT_MAX_FIELDS        128
#define VT_MAX_BLOCKS        4096
#define VT_MAX_INSTS         65536
#define VT_MAX_VREGS         65536
#define VT_INVALID_VREG      UINT32_MAX
#define VT_INVALID_BLOCK     UINT32_MAX
#define VT_ARENA_PAGE_SIZE   (64 * 1024)

/* ---- Dynamic array macro ---- */
#define VT_DA_INIT_CAP 16

#define vt_da_push(arr, len, cap, val) do {                         \
    if ((len) >= (cap)) {                                           \
        (cap) = (cap) ? (cap) * 2 : VT_DA_INIT_CAP;                \
        (arr) = realloc((arr), sizeof(*(arr)) * (cap));             \
        if (!(arr)) { fprintf(stderr, "out of memory\n"); exit(1); } \
    }                                                               \
    (arr)[(len)++] = (val);                                         \
} while (0)

#define vt_da_free(arr, len, cap) do {  \
    free(arr);                          \
    (arr) = NULL;                       \
    (len) = 0;                          \
    (cap) = 0;                          \
} while (0)

/* ---- Source location ---- */
typedef struct {
    const char *filename;
    uint32_t line;
    uint32_t col;
} SrcLoc;

/* ---- Token kinds ---- */
typedef enum {
    TOK_EOF = 0,

    /* Keywords */
    TOK_FN, TOK_LET, TOK_STRUCT, TOK_IF, TOK_ELSE,
    TOK_WHILE, TOK_FOR, TOK_RETURN, TOK_ARENA, TOK_ALLOC,
    TOK_TRUE, TOK_FALSE, TOK_PRINT, TOK_AS, TOK_SIZEOF,

    /* Type keywords */
    TOK_I8, TOK_I16, TOK_I32, TOK_I64,
    TOK_U8, TOK_U16, TOK_U32, TOK_U64,
    TOK_F32, TOK_F64, TOK_BOOL, TOK_VOID,

    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_LSHIFT, TOK_RSHIFT,
    TOK_EQEQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LEQ, TOK_GEQ,
    TOK_AMPAMP, TOK_PIPEPIPE, TOK_BANG, TOK_TILDE,
    TOK_EQ,       /* = */
    TOK_PLUSEQ,   /* += */
    TOK_MINUSEQ,  /* -= */
    TOK_STAREQ,   /* *= */
    TOK_SLASHEQ,  /* /= */

    /* Delimiters */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMI, TOK_COLON,
    TOK_ARROW,   /* -> */
    TOK_DOT,     /* . */

    /* Literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,

    /* Identifier */
    TOK_IDENT,
} TokenKind;

/* ---- Token ---- */
typedef struct {
    TokenKind kind;
    SrcLoc loc;
    /* Value for literals */
    union {
        int64_t   int_val;
        double    float_val;
        char     *str_val;
        char     *ident;
    };
} Token;

/* ---- VORTECH type system ---- */
typedef enum {
    VTTYPE_VOID = 0,
    VTTYPE_I8, VTTYPE_I16, VTTYPE_I32, VTTYPE_I64,
    VTTYPE_U8, VTTYPE_U16, VTTYPE_U32, VTTYPE_U64,
    VTTYPE_F32, VTTYPE_F64,
    VTTYPE_BOOL,
    VTTYPE_PTR,
    VTTYPE_STRUCT,
    VTTYPE_ARRAY,
} VtTypeKind;

typedef struct VtType VtType;
struct VtType {
    VtTypeKind kind;
    /* For PTR: base = pointee type */
    VtType *base;
    /* For STRUCT: name + fields */
    const char *name;
    struct {
        const char **names;
        VtType    **types;
        uint32_t    count;
    } fields;
    /* For ARRAY: element count */
    uint64_t array_count;
};

/* Get size in bytes for a type */
static inline uint32_t vt_type_size(VtType *t) {
    if (!t) return 0;
    switch (t->kind) {
    case VTTYPE_VOID:  return 0;
    case VTTYPE_I8:    return 1;
    case VTTYPE_U8:    return 1;
    case VTTYPE_I16:   return 2;
    case VTTYPE_U16:   return 2;
    case VTTYPE_I32:   return 4;
    case VTTYPE_U32:   return 4;
    case VTTYPE_I64:   return 8;
    case VTTYPE_U64:   return 8;
    case VTTYPE_F32:   return 4;
    case VTTYPE_F64:   return 8;
    case VTTYPE_BOOL:  return 1;
    case VTTYPE_PTR:   return 8;
    case VTTYPE_STRUCT: {
        uint32_t s = 0;
        for (uint32_t i = 0; i < t->fields.count; i++) {
            s += vt_type_size(t->fields.types[i]);
            /* 4-byte alignment for now */
            s = (s + 3) & ~3u;
        }
        return s;
    }
    case VTTYPE_ARRAY:
        return (uint32_t)(t->array_count * vt_type_size(t->base));
    }
    return 0;
}

/* Get alignment for a type */
static inline uint32_t vt_type_align(VtType *t) {
    if (!t) return 1;
    switch (t->kind) {
    case VTTYPE_F64:
    case VTTYPE_I64:
    case VTTYPE_U64:
    case VTTYPE_PTR:
        return 8;
    case VTTYPE_F32:
    case VTTYPE_I32:
    case VTTYPE_U32:
        return 4;
    case VTTYPE_I16:
    case VTTYPE_U16:
        return 2;
    default:
        return 1;
    }
}

/* Is the type an integer type? */
static inline bool vt_type_is_int(VtType *t) {
    if (!t) return false;
    return t->kind >= VTTYPE_I8 && t->kind <= VTTYPE_U64;
}

/* Is the type a float type? */
static inline bool vt_type_is_float(VtType *t) {
    if (!t) return false;
    return t->kind == VTTYPE_F32 || t->kind == VTTYPE_F64;
}

/* Is the type numeric? */
static inline bool vt_type_is_numeric(VtType *t) {
    return vt_type_is_int(t) || vt_type_is_float(t) || t->kind == VTTYPE_BOOL;
}

/* Is the type unsigned? */
static inline bool vt_type_is_unsigned(VtType *t) {
    if (!t) return false;
    return t->kind >= VTTYPE_U8 && t->kind <= VTTYPE_U64;
}

/* ---- SSA IR opcodes ---- */
typedef enum {
    OP_CONST,       /* v = immediate constant                */
    OP_BINOP,       /* v = lhs op rhs                        */
    OP_UNOP,        /* v = op operand                        */
    OP_LOAD,        /* v = load(ptr)                         */
    OP_STORE,       /* store(ptr, val)                       */
    OP_GEP,         /* v = ptr + byte_offset                 */
    OP_CMP,         /* v = cmp(lhs, rhs)                     */
    OP_BRANCH,      /* br cond, true_bb, false_bb            */
    OP_JUMP,        /* jump target_bb                        */
    OP_CALL,        /* v = call(func, args...)               */
    OP_RET,         /* ret val                               */
    OP_PHI,         /* v = phi(pred1:val1, pred2:val2, ...)  */
    OP_ALLOC,       /* v = stack_alloc(bytes)                */
    OP_ARENA_ALLOC, /* v = arena_alloc(arena, type_size)     */
    OP_ARENA_RESET, /* arena_reset(arena)                    */
    OP_PRINT,       /* print(val)                            */
} SsaOpcode;

/* Binary operation kinds */
typedef enum {
    BINOP_ADD = 0,
    BINOP_SUB,
    BINOP_MUL,
    BINOP_DIV,
    BINOP_MOD,
    BINOP_AND,
    BINOP_OR,
    BINOP_XOR,
    BINOP_LSHIFT,
    BINOP_RSHIFT,
} BinOpKind;

/* Unary operation kinds */
typedef enum {
    UNOP_NEG = 0,
    UNOP_NOT,    /* logical not */
    UNOP_BNOT,   /* bitwise not */
} UnOpKind;

/* Comparison kinds */
typedef enum {
    CMP_EQ = 0,
    CMP_NEQ,
    CMP_LT,
    CMP_GT,
    CMP_LEQ,
    CMP_GEQ,
} CmpKind;

/* ---- Machine instruction opcodes (post instruction selection) ---- */
typedef enum {
    MINST_MOV,         /* mov dst, src            */
    MINST_MOV_IMM,     /* mov dst, imm            */
    MINST_ADD,         /* add dst, src            */
    MINST_ADD_IMM,     /* add dst, imm            */
    MINST_SUB,         /* sub dst, src            */
    MINST_SUB_IMM,     /* sub dst, imm            */
    MINST_MUL,         /* imul dst, src           */
    MINST_MUL_IMM,     /* imul dst, imm           */
    MINST_DIV,         /* idiv src (rax /= src)   */
    MINST_AND,         /* and dst, src            */
    MINST_OR,          /* or  dst, src            */
    MINST_XOR,         /* xor dst, src            */
    MINST_SHL,         /* shl dst, count          */
    MINST_SHR,         /* shr dst, count          */
    MINST_SAR,         /* sar dst, count          */
    MINST_NEG,         /* neg dst                 */
    MINST_NOT,         /* not dst                 */
    MINST_CMP,         /* cmp lhs, rhs            */
    MINST_CMP_IMM,     /* cmp lhs, imm            */
    MINST_TEST,        /* test lhs, rhs           */
    MINST_SET_EQ,      /* sete dst                */
    MINST_SET_NEQ,     /* setne dst               */
    MINST_SET_LT,      /* setl dst                */
    MINST_SET_GT,      /* setg dst                */
    MINST_SET_LEQ,     /* setle dst               */
    MINST_SET_GEQ,     /* setge dst               */
    MINST_JMP,         /* jmp label               */
    MINST_JE,          /* je  label               */
    MINST_JNE,         /* jne label               */
    MINST_JL,          /* jl  label               */
    MINST_JG,          /* jg  label               */
    MINST_JLE,         /* jle label               */
    MINST_JGE,         /* jge label               */
    MINST_CALL,        /* call func               */
    MINST_RET,         /* ret                     */
    MINST_PUSH,        /* push src                */
    MINST_POP,         /* pop dst                 */
    MINST_LEA,         /* lea dst, [base + off]   */
    MINST_MOVZX,       /* movzx dst, src          */
    MINST_CVTSI2SD,    /* cvtsi2sd dst, src       */
    MINST_CVTSD2SI,    /* cvtsd2si dst, src       */
    MINST_ADDSD,       /* addsd dst, src          */
    MINST_SUBSD,       /* subsd dst, src          */
    MINST_MULSD,       /* mulsd dst, src          */
    MINST_DIVSD,       /* divsd dst, src          */
    MINST_XORPS,       /* xorps dst, src          */
    MINST_LABEL,       /* label:                  */
    MINST_COMMENT,     /* # comment               */
    MINST_NOP,         /* nop                     */
    MINST_SYSCALL,     /* syscall                 */
} MachOpcode;

/* x86-64 registers */
typedef enum {
    REG_NONE = 0,
    REG_RAX, REG_RCX, REG_RDX, REG_RBX,
    REG_RSP, REG_RBP, REG_RSI, REG_RDI,
    REG_R8,  REG_R9,  REG_R10, REG_R11,
    REG_R12, REG_R13, REG_R14, REG_R15,
    REG_XMM0, REG_XMM1, REG_XMM2, REG_XMM3,
    REG_XMM4, REG_XMM5, REG_XMM6, REG_XMM7,
    REG_COUNT,
} X86Reg;

/* GPR register names for emission */
static inline const char *reg_name(X86Reg r) {
    static const char *names[] = {
        "none",
        "rax", "rcx", "rdx", "rbx",
        "rsp", "rbp", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11",
        "r12", "r13", "r14", "r15",
        "xmm0","xmm1","xmm2","xmm3",
        "xmm4","xmm5","xmm6","xmm7",
    };
    if (r >= REG_COUNT) return "???";
    return names[r];
}

/* 32-bit register names */
static inline const char *reg32_name(X86Reg r) {
    static const char *names[] = {
        "none",
        "eax", "ecx", "edx", "ebx",
        "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d","r11d",
        "r12d","r13d","r14d","r15d",
        "xmm0","xmm1","xmm2","xmm3",
        "xmm4","xmm5","xmm6","xmm7",
    };
    if (r >= REG_COUNT) return "???";
    return names[r];
}

/* 8-bit register names */
static inline const char *reg8_name(X86Reg r) {
    static const char *names[] = {
        "none",
        "al",  "cl",  "dl",  "bl",
        "spl", "bpl", "sil", "dil",
        "r8b", "r9b", "r10b","r11b",
        "r12b","r13b","r14b","r15b",
        "xmm0","xmm1","xmm2","xmm3",
        "xmm4","xmm5","xmm6","xmm7",
    };
    if (r >= REG_COUNT) return "???";
    return names[r];
}

/* Is this a GPR (not XMM)? */
static inline bool reg_is_gpr(X86Reg r) {
    return r >= REG_RAX && r <= REG_R15;
}

/* Is this an XMM register? */
static inline bool reg_is_xmm(X86Reg r) {
    return r >= REG_XMM0 && r <= REG_XMM7;
}

/* Is this register callee-saved? */
static inline bool reg_is_callee_saved(X86Reg r) {
    return r == REG_RBX || r == REG_RBP ||
           (r >= REG_R12 && r <= REG_R15);
}

/* Number of allocatable GPRs (excluding RSP, RBP) */
#define VT_NUM_ALLOC_GPRS  12  /* rax, rcx, rdx, rbx, rsi, rdi, r8-r15 */
#define VT_NUM_ALLOC_XMMS  8

/* Allocatable GPRs in preference order */
static const X86Reg vt_alloc_gprs[] = {
    REG_RAX, REG_RCX, REG_RDX, REG_RSI, REG_RDI,
    REG_R8,  REG_R9,  REG_R10, REG_R11,
    REG_RBX, REG_R12, REG_R13, REG_R14, REG_R15,
};

/* Allocatable XMM registers */
static const X86Reg vt_alloc_xmms[] = {
    REG_XMM0, REG_XMM1, REG_XMM2, REG_XMM3,
    REG_XMM4, REG_XMM5, REG_XMM6, REG_XMM7,
};

#endif /* VORTECH_COMMON_H */
