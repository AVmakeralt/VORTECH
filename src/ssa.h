/*
 * VORTECH Compiler - SSA Intermediate Representation
 *
 * Small. Simple. Predictable. ~30-50 operations.
 * Every extra opcode must justify itself.
 */
#ifndef VORTECH_SSA_H
#define VORTECH_SSA_H

#include "common.h"
#include "arena.h"
#include "hir.h"

/* ---- SSA Instruction ---- */
typedef struct SsaInst {
    SsaOpcode  op;
    SrcLoc     loc;
    uint32_t   dst;           /* destination vreg (VT_INVALID_VREG if none) */
    VtType    *dst_type;      /* type of the result */

    union {
        /* OP_CONST */
        struct { int64_t value; } const_;

        /* OP_BINOP */
        struct { BinOpKind kind; uint32_t lhs; uint32_t rhs; } binop;

        /* OP_UNOP */
        struct { UnOpKind kind; uint32_t operand; } unop;

        /* OP_LOAD */
        struct { uint32_t ptr; } load;

        /* OP_STORE */
        struct { uint32_t ptr; uint32_t val; } store;

        /* OP_GEP */
        struct { uint32_t ptr; int64_t offset; } gep;

        /* OP_CMP */
        struct { CmpKind kind; uint32_t lhs; uint32_t rhs; } cmp;

        /* OP_BRANCH */
        struct { uint32_t cond; uint32_t true_bb; uint32_t false_bb; } branch;

        /* OP_JUMP */
        struct { uint32_t target_bb; } jump;

        /* OP_CALL */
        struct {
            const char *func_name;
            uint32_t   *args;
            uint32_t    nargs;
            bool        is_extern;
        } call;

        /* OP_RET */
        struct { uint32_t val; } ret;

        /* OP_PHI */
        struct {
            uint32_t *srcs;    /* vreg values */
            uint32_t *preds;   /* predecessor block indices */
            uint32_t  n;       /* number of entries */
        } phi;

        /* OP_ALLOC */
        struct { uint64_t bytes; } alloc;

        /* OP_ARENA_ALLOC */
        struct {
            const char *arena_name;
            uint64_t    type_size;
        } arena_alloc;

        /* OP_ARENA_RESET */
        struct { const char *arena_name; } arena_reset;

        /* OP_PRINT */
        struct { uint32_t val; } print;
    };
} SsaInst;

/* ---- Basic Block ---- */
typedef struct SsaBlock {
    uint32_t   label;         /* block index */
    SsaInst   *insts;
    uint32_t   ninsts;
    uint32_t   insts_cap;
    uint32_t  *preds;
    uint32_t   npreds;
    uint32_t   preds_cap;
    uint32_t  *succs;
    uint32_t   nsuccs;
    uint32_t   succs_cap;
    bool       sealed;        /* all predecessors known? */
    bool       visited;       /* for traversal */
} SsaBlock;

/* ---- SSA Function ---- */
typedef struct SsaFunc {
    const char *name;
    SsaBlock   *blocks;
    uint32_t    nblocks;
    uint32_t    blocks_cap;
    uint32_t    entry;        /* entry block index */
    uint32_t    next_vreg;    /* next virtual register number */
    VtType     *ret_type;
    /* Parameter info */
    char      **param_names;
    uint32_t   *param_vregs;  /* vreg for each param */
    VtType    **param_types;
    uint32_t    nparams;
    /* Stack frame info (filled during SSA construction) */
    uint32_t    stack_slots;  /* number of stack slots */
} SsaFunc;

/* ---- SSA Program ---- */
typedef struct {
    SsaFunc   *funcs;
    uint32_t   nfuncs;
} SsaProgram;

/* ---- SSA Builder (opaque - defined in ssa_build.c) ---- */
typedef struct SsaBuilder SsaBuilder;

/* ---- Construction API ---- */

/* Build SSA from HIR */
SsaProgram *ssa_build_program(Arena *arena, HirNode *hir);

/* ---- Verification ---- */
bool ssa_verify_func(SsaFunc *func);
bool ssa_verify_program(SsaProgram *prog);

/* ---- Debug ---- */
void ssa_print_func(SsaFunc *func);
void ssa_print_program(SsaProgram *prog);
const char *binop_str(BinOpKind k);
const char *unop_str(UnOpKind k);
const char *cmp_str(CmpKind k);
const char *ssa_op_str(SsaOpcode op);

#endif /* VORTECH_SSA_H */
