/*
 * VORTECH Compiler - SSA Construction
 *
 * Based on Braun et al.'s simple SSA construction algorithm.
 * Per-block variable definitions with PHI insertion at merge points.
 * Builds SSA on-the-fly during a single pass over HIR.
 */
#include "ssa.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

/* ---- Debug helpers ---- */
const char *binop_str(BinOpKind k) {
    switch (k) {
    case BINOP_ADD:    return "+";
    case BINOP_SUB:    return "-";
    case BINOP_MUL:    return "*";
    case BINOP_DIV:    return "/";
    case BINOP_MOD:    return "%";
    case BINOP_AND:    return "&";
    case BINOP_OR:     return "|";
    case BINOP_XOR:    return "^";
    case BINOP_LSHIFT: return "<<";
    case BINOP_RSHIFT: return ">>";
    }
    return "?";
}

const char *unop_str(UnOpKind k) {
    switch (k) {
    case UNOP_NEG:  return "-";
    case UNOP_NOT:  return "!";
    case UNOP_BNOT: return "~";
    }
    return "?";
}

const char *cmp_str(CmpKind k) {
    switch (k) {
    case CMP_EQ:  return "==";
    case CMP_NEQ: return "!=";
    case CMP_LT:  return "<";
    case CMP_GT:  return ">";
    case CMP_LEQ: return "<=";
    case CMP_GEQ: return ">=";
    }
    return "?";
}

const char *ssa_op_str(SsaOpcode op) {
    switch (op) {
    case OP_CONST:       return "CONST";
    case OP_BINOP:       return "BINOP";
    case OP_UNOP:        return "UNOP";
    case OP_LOAD:        return "LOAD";
    case OP_STORE:       return "STORE";
    case OP_GEP:         return "GEP";
    case OP_CMP:         return "CMP";
    case OP_BRANCH:      return "BRANCH";
    case OP_JUMP:        return "JUMP";
    case OP_CALL:        return "CALL";
    case OP_RET:         return "RET";
    case OP_PHI:         return "PHI";
    case OP_ALLOC:       return "ALLOC";
    case OP_ARENA_ALLOC: return "ARENA_ALLOC";
    case OP_ARENA_RESET: return "ARENA_RESET";
    case OP_PRINT:       return "PRINT";
    }
    return "?";
}

/* ---- SSA Block helpers ---- */
static SsaBlock *ssa_get_block(SsaFunc *func, uint32_t idx) {
    if (idx >= func->nblocks) return NULL;
    return &func->blocks[idx];
}

static void block_add_pred(SsaBlock *b, uint32_t pred) {
    uint32_t cap = b->preds_cap ? b->preds_cap * 2 : 4;
    if (b->npreds >= b->preds_cap) {
        b->preds = realloc(b->preds, sizeof(uint32_t) * cap);
        if (!b->preds) { fprintf(stderr, "out of memory\n"); exit(1); }
        b->preds_cap = cap;
    }
    b->preds[b->npreds++] = pred;
}

static void block_add_succ(SsaBlock *b, uint32_t succ) {
    uint32_t cap = b->succs_cap ? b->succs_cap * 2 : 4;
    if (b->nsuccs >= b->succs_cap) {
        b->succs = realloc(b->succs, sizeof(uint32_t) * cap);
        if (!b->succs) { fprintf(stderr, "out of memory\n"); exit(1); }
        b->succs_cap = cap;
    }
    b->succs[b->nsuccs++] = succ;
}

static void block_add_inst(SsaBlock *b, SsaInst *inst) {
    uint32_t cap = b->insts_cap ? b->insts_cap * 2 : 8;
    if (b->ninsts >= b->insts_cap) {
        b->insts = realloc(b->insts, sizeof(SsaInst) * cap);
        if (!b->insts) { fprintf(stderr, "out of memory\n"); exit(1); }
        b->insts_cap = cap;
    }
    b->insts[b->ninsts++] = *inst;
}

/* ---- Per-block variable definition table ---- */
typedef struct VarDef {
    char           *name;
    uint32_t        vreg;
    struct VarDef  *next;
} VarDef;

typedef struct BlockDefTable {
    VarDef                *defs;        /* linked list of name -> vreg for this block */
    struct BlockDefTable  *parent;      /* for scoped lookup */
} BlockDefTable;

/* ---- Incomplete PHI tracking ---- */
typedef struct IncompletePhi {
    char     *var_name;
    uint32_t  phi_vreg;
    uint32_t  block;
    struct IncompletePhi *next;
} IncompletePhi;

/* ---- SSA Builder (Braun algorithm) ---- */
struct SsaBuilder {
    Arena              *arena;
    SsaFunc            *func;
    uint32_t            cur_block;
    uint32_t            next_vreg;

    /* Per-block variable definition tables */
    BlockDefTable     **block_defs;    /* indexed by block number */

    /* Current scope (used for nested blocks within a function) */
    BlockDefTable      *current_scope;

    /* Incomplete PHI nodes that need to be filled when blocks are sealed */
    IncompletePhi      *incomplete_phis;
};

/* ---- Builder API ---- */
static SsaBuilder *ssa_builder_create(Arena *arena, SsaFunc *func) {
    SsaBuilder *b = arena_calloc(arena, 1, sizeof(SsaBuilder));
    b->arena = arena;
    b->func = func;
    b->cur_block = func->entry;
    b->next_vreg = func->nparams;
    b->incomplete_phis = NULL;

    b->block_defs = arena_calloc(arena, func->blocks_cap, sizeof(BlockDefTable *));

    /* Initialize definition table for entry block */
    b->block_defs[0] = arena_calloc(arena, 1, sizeof(BlockDefTable));
    b->current_scope = b->block_defs[0];

    /* Define parameters in entry block */
    for (uint32_t i = 0; i < func->nparams; i++) {
        VarDef *vd = arena_calloc(arena, 1, sizeof(VarDef));
        vd->name = func->param_names[i];
        vd->vreg = func->param_vregs[i];
        vd->next = b->block_defs[0]->defs;
        b->block_defs[0]->defs = vd;
    }

    return b;
}

static uint32_t ssa_builder_new_vreg(SsaBuilder *b) {
    return b->next_vreg++;
}

static uint32_t ssa_builder_add_block(SsaBuilder *b) {
    SsaFunc *func = b->func;
    uint32_t idx = func->nblocks;
    if (idx >= func->blocks_cap) {
        uint32_t cap = func->blocks_cap ? func->blocks_cap * 2 : 16;
        func->blocks = realloc(func->blocks, sizeof(SsaBlock) * cap);
        if (!func->blocks) { fprintf(stderr, "out of memory\n"); exit(1); }
        /* Also grow block_defs */
        BlockDefTable **new_defs = realloc(b->block_defs, sizeof(BlockDefTable *) * cap);
        if (!new_defs) { fprintf(stderr, "out of memory\n"); exit(1); }
        memset(new_defs + func->blocks_cap, 0, sizeof(BlockDefTable *) * (cap - func->blocks_cap));
        b->block_defs = new_defs;
        func->blocks_cap = cap;
    }
    memset(&func->blocks[idx], 0, sizeof(SsaBlock));
    func->blocks[idx].label = idx;
    func->blocks[idx].sealed = false;
    func->nblocks++;

    /* Create definition table for new block */
    b->block_defs[idx] = arena_calloc(b->arena, 1, sizeof(BlockDefTable));

    return idx;
}

static void ssa_builder_set_cur_block(SsaBuilder *b, uint32_t block) {
    b->cur_block = block;
    b->current_scope = b->block_defs[block];
}

/* Write variable: record that `name` has value `vreg` in current block */
static void ssa_builder_write_var(SsaBuilder *b, const char *name, uint32_t vreg) {
    BlockDefTable *dt = b->block_defs[b->cur_block];
    /* Update existing or add new */
    VarDef *vd;
    for (vd = dt->defs; vd; vd = vd->next) {
        if (strcmp(vd->name, name) == 0) {
            vd->vreg = vreg;
            return;
        }
    }
    vd = arena_calloc(b->arena, 1, sizeof(VarDef));
    vd->name = arena_strdup(b->arena, name);
    vd->vreg = vreg;
    vd->next = dt->defs;
    dt->defs = vd;
}

/* Read variable: get the current value of `name` using Braun's algorithm */
static uint32_t ssa_builder_read_var(SsaBuilder *b, uint32_t block, const char *name);

static uint32_t ssa_builder_read_var_rec(SsaBuilder *b, uint32_t block, const char *name) {
    /* Check local definitions in this block */
    BlockDefTable *dt = b->block_defs[block];
    for (VarDef *vd = dt->defs; vd; vd = vd->next) {
        if (strcmp(vd->name, name) == 0) {
            return vd->vreg;
        }
    }

    /* Not defined locally - need to look in predecessors */
    SsaBlock *blk = ssa_get_block(b->func, block);
    if (!blk) return VT_INVALID_VREG;

    if (blk->sealed) {
        /* Block is sealed - all predecessors are known */
        if (blk->npreds == 0) {
            diag_msg(DIAG_ERROR, "undefined variable '%s' in entry block", name);
            return VT_INVALID_VREG;
        }
        if (blk->npreds == 1) {
            /* Single predecessor - just read from it */
            uint32_t val = ssa_builder_read_var(b, blk->preds[0], name);
            /* Remember this value in the current block for future lookups */
            VarDef *vd = arena_calloc(b->arena, 1, sizeof(VarDef));
            vd->name = arena_strdup(b->arena, name);
            vd->vreg = val;
            vd->next = dt->defs;
            dt->defs = vd;
            return val;
        }
        /* Multiple predecessors - need a PHI node */
        /* Create PHI with placeholder values */
        uint32_t phi_vreg = ssa_builder_new_vreg(b);
        /* First, record the PHI so recursive reads find it */
        VarDef *vd = arena_calloc(b->arena, 1, sizeof(VarDef));
        vd->name = arena_strdup(b->arena, name);
        vd->vreg = phi_vreg;
        vd->next = dt->defs;
        dt->defs = vd;

        /* Now read from all predecessors */
        uint32_t *srcs = arena_calloc(b->arena, blk->npreds, sizeof(uint32_t));
        uint32_t *preds = arena_calloc(b->arena, blk->npreds, sizeof(uint32_t));
        for (uint32_t i = 0; i < blk->npreds; i++) {
            srcs[i] = ssa_builder_read_var(b, blk->preds[i], name);
            preds[i] = blk->preds[i];
        }

        /* Check if PHI is trivial (all same value) */
        bool all_same = true;
        for (uint32_t i = 1; i < blk->npreds; i++) {
            if (srcs[i] != srcs[0]) { all_same = false; break; }
        }

        if (all_same) {
            /* Trivial PHI - just use the single value */
            /* Update the definition to point to the actual value */
            for (VarDef *v = dt->defs; v; v = v->next) {
                if (strcmp(v->name, name) == 0) {
                    v->vreg = srcs[0];
                    break;
                }
            }
            /* Don't allocate the phi_vreg - reuse the source vreg directly */
            /* Reclaim the allocated phi_vreg by decrementing next_vreg */
            b->next_vreg--;
            return srcs[0];
        }

        /* Emit the PHI instruction at the beginning of the block */
        SsaInst phi;
        memset(&phi, 0, sizeof(phi));
        phi.op = OP_PHI;
        phi.dst = phi_vreg;
        phi.dst_type = NULL; /* type resolved later */
        phi.phi.srcs = srcs;
        phi.phi.preds = preds;
        phi.phi.n = blk->npreds;

        /* Insert at beginning of block (before other instructions) */
        if (blk->ninsts == 0) {
            block_add_inst(blk, &phi);
        } else {
            /* Shift all instructions up by 1 */
            if (blk->ninsts >= blk->insts_cap) {
                blk->insts_cap = blk->ninsts * 2;
                blk->insts = realloc(blk->insts, sizeof(SsaInst) * blk->insts_cap);
            }
            memmove(&blk->insts[1], &blk->insts[0], sizeof(SsaInst) * blk->ninsts);
            blk->insts[0] = phi;
            blk->ninsts++;
        }

        return phi_vreg;
    } else {
        /* Block not yet sealed - create incomplete PHI */
        uint32_t phi_vreg = ssa_builder_new_vreg(b);

        /* Record the PHI so recursive reads find it */
        VarDef *vd = arena_calloc(b->arena, 1, sizeof(VarDef));
        vd->name = arena_strdup(b->arena, name);
        vd->vreg = phi_vreg;
        vd->next = dt->defs;
        dt->defs = vd;

        /* Add to incomplete phi list */
        IncompletePhi *ip = arena_calloc(b->arena, 1, sizeof(IncompletePhi));
        ip->var_name = arena_strdup(b->arena, name);
        ip->phi_vreg = phi_vreg;
        ip->block = block;
        ip->next = b->incomplete_phis;
        b->incomplete_phis = ip;

        /* Emit placeholder PHI */
        SsaInst phi;
        memset(&phi, 0, sizeof(phi));
        phi.op = OP_PHI;
        phi.dst = phi_vreg;
        phi.dst_type = NULL;
        phi.phi.srcs = NULL;
        phi.phi.preds = NULL;
        phi.phi.n = 0;

        if (blk->ninsts == 0) {
            block_add_inst(blk, &phi);
        } else {
            if (blk->ninsts >= blk->insts_cap) {
                blk->insts_cap = blk->ninsts * 2;
                blk->insts = realloc(blk->insts, sizeof(SsaInst) * blk->insts_cap);
            }
            memmove(&blk->insts[1], &blk->insts[0], sizeof(SsaInst) * blk->ninsts);
            blk->insts[0] = phi;
            blk->ninsts++;
        }

        return phi_vreg;
    }
}

static uint32_t ssa_builder_read_var(SsaBuilder *b, uint32_t block, const char *name) {
    return ssa_builder_read_var_rec(b, block, name);
}

/* Read variable from current block */
static uint32_t ssa_builder_read_var_current(SsaBuilder *b, const char *name) {
    return ssa_builder_read_var(b, b->cur_block, name);
}

/* Seal a block: all predecessors are now known, complete incomplete PHIs */
static void ssa_builder_seal_block(SsaBuilder *b, uint32_t block) {
    SsaBlock *blk = ssa_get_block(b->func, block);
    if (!blk || blk->sealed) return;
    blk->sealed = true;

    /* Process incomplete PHIs for this block */
    IncompletePhi **ipp = &b->incomplete_phis;
    while (*ipp) {
        IncompletePhi *ip = *ipp;
        if (ip->block == block) {
            /* Fill in the PHI with values from predecessors */
            /* Find the PHI instruction in the block */
            SsaInst *phi_inst = NULL;
            for (uint32_t i = 0; i < blk->ninsts; i++) {
                if (blk->insts[i].op == OP_PHI && blk->insts[i].dst == ip->phi_vreg) {
                    phi_inst = &blk->insts[i];
                    break;
                }
            }

            if (phi_inst) {
                phi_inst->phi.n = blk->npreds;
                phi_inst->phi.srcs = arena_calloc(b->arena, blk->npreds, sizeof(uint32_t));
                phi_inst->phi.preds = arena_calloc(b->arena, blk->npreds, sizeof(uint32_t));

                for (uint32_t i = 0; i < blk->npreds; i++) {
                    phi_inst->phi.srcs[i] = ssa_builder_read_var(b, blk->preds[i], ip->var_name);
                    phi_inst->phi.preds[i] = blk->preds[i];
                }

                /* Check for trivial PHI */
                bool all_same = true;
                for (uint32_t i = 1; i < phi_inst->phi.n; i++) {
                    if (phi_inst->phi.srcs[i] != phi_inst->phi.srcs[0]) {
                        all_same = false;
                        break;
                    }
                }
                if (all_same && phi_inst->phi.n > 0) {
                    /* Trivial PHI - update definition to point to the single source */
                    BlockDefTable *dt = b->block_defs[block];
                    for (VarDef *vd = dt->defs; vd; vd = vd->next) {
                        if (strcmp(vd->name, ip->var_name) == 0 && vd->vreg == ip->phi_vreg) {
                            vd->vreg = phi_inst->phi.srcs[0];
                            break;
                        }
                    }
                    /* Keep the PHI instruction - copy propagation will
                     * replace uses of phi_vreg with the source vreg,
                     * and DCE will remove the dead PHI */
                }
            }

            /* Remove from incomplete list */
            *ipp = ip->next;
        } else {
            ipp = &(*ipp)->next;
        }
    }
}

/* ---- Emit helpers ---- */
static SsaInst *ssa_builder_emit(SsaBuilder *b, SsaOpcode op, VtType *type) {
    SsaInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = op;
    inst.dst = (op == OP_BRANCH || op == OP_JUMP || op == OP_STORE ||
                op == OP_RET || op == OP_ARENA_RESET || op == OP_PRINT)
               ? VT_INVALID_VREG : ssa_builder_new_vreg(b);
    inst.dst_type = type;

    SsaBlock *blk = ssa_get_block(b->func, b->cur_block);
    if (blk) {
        block_add_inst(blk, &inst);
        return &blk->insts[blk->ninsts - 1];
    }
    return NULL;
}

static SsaInst *ssa_builder_emit_inst(SsaBuilder *b, SsaInst *inst) {
    SsaBlock *blk = ssa_get_block(b->func, b->cur_block);
    if (blk) {
        block_add_inst(blk, inst);
        return &blk->insts[blk->ninsts - 1];
    }
    return NULL;
}

/* ---- SSA construction from HIR ---- */

typedef struct {
    Arena       *arena;
    SsaBuilder  *builder;
    HirNode     *hir_func;
} SsaBuildCtx;

static uint32_t ssa_build_expr(SsaBuildCtx *ctx, HirNode *hir);
static void     ssa_build_stmt(SsaBuildCtx *ctx, HirNode *hir);

static uint32_t ssa_build_expr(SsaBuildCtx *ctx, HirNode *hir) {
    if (!hir) return VT_INVALID_VREG;

    SsaBuilder *b = ctx->builder;

    switch (hir->kind) {
    case HIR_INT_LIT: {
        SsaInst *inst = ssa_builder_emit(b, OP_CONST, hir->type);
        inst->const_.value = hir->int_lit.value;
        return inst->dst;
    }
    case HIR_FLOAT_LIT: {
        SsaInst *inst = ssa_builder_emit(b, OP_CONST, hir->type);
        double d = hir->float_lit.value;
        memcpy(&inst->const_.value, &d, sizeof(double));
        return inst->dst;
    }
    case HIR_BOOL_LIT: {
        SsaInst *inst = ssa_builder_emit(b, OP_CONST, hir->type);
        inst->const_.value = hir->bool_lit.value ? 1 : 0;
        return inst->dst;
    }
    case HIR_STRING_LIT: {
        SsaInst *inst = ssa_builder_emit(b, OP_CONST, hir->type);
        inst->const_.value = (int64_t)(uintptr_t)hir->string_lit.value;
        return inst->dst;
    }
    case HIR_IDENT_EXPR: {
        return ssa_builder_read_var_current(b, hir->ident_expr.name);
    }
    case HIR_BINARY_EXPR: {
        /* Short-circuit for && and || */
        if (hir->binary_expr.op == TOK_AMPAMP) {
            uint32_t lhs = ssa_build_expr(ctx, hir->binary_expr.left);
            uint32_t result = ssa_builder_new_vreg(b);

            uint32_t rhs_block = ssa_builder_add_block(b);
            uint32_t merge_block = ssa_builder_add_block(b);
            uint32_t false_block = ssa_builder_add_block(b);

            uint32_t orig_block = b->cur_block;

            SsaInst br;
            memset(&br, 0, sizeof(br));
            br.op = OP_BRANCH;
            br.dst = VT_INVALID_VREG;
            br.branch.cond = lhs;
            br.branch.true_bb = rhs_block;
            br.branch.false_bb = false_block;
            ssa_builder_emit_inst(b, &br);

            SsaBlock *cur = ssa_get_block(b->func, orig_block);
            if (cur) {
                block_add_succ(cur, rhs_block);
                block_add_succ(cur, false_block);
            }

            /* rhs block */
            ssa_builder_set_cur_block(b, rhs_block);
            SsaBlock *rhs_blk = ssa_get_block(b->func, rhs_block);
            if (rhs_blk) block_add_pred(rhs_blk, orig_block);
            uint32_t rhs = ssa_build_expr(ctx, hir->binary_expr.right);

            SsaInst jmp_rhs;
            memset(&jmp_rhs, 0, sizeof(jmp_rhs));
            jmp_rhs.op = OP_JUMP;
            jmp_rhs.dst = VT_INVALID_VREG;
            jmp_rhs.jump.target_bb = merge_block;
            ssa_builder_emit_inst(b, &jmp_rhs);
            if (rhs_blk) block_add_succ(rhs_blk, merge_block);
            ssa_builder_seal_block(b, rhs_block);

            /* false block */
            ssa_builder_set_cur_block(b, false_block);
            SsaBlock *false_blk = ssa_get_block(b->func, false_block);
            if (false_blk) block_add_pred(false_blk, orig_block);

            SsaInst *zero = ssa_builder_emit(b, OP_CONST, vt_type_make(ctx->arena, VTTYPE_BOOL));
            zero->const_.value = 0;

            SsaInst jmp_f;
            memset(&jmp_f, 0, sizeof(jmp_f));
            jmp_f.op = OP_JUMP;
            jmp_f.dst = VT_INVALID_VREG;
            jmp_f.jump.target_bb = merge_block;
            ssa_builder_emit_inst(b, &jmp_f);
            if (false_blk) block_add_succ(false_blk, merge_block);
            ssa_builder_seal_block(b, false_block);

            /* merge block */
            ssa_builder_set_cur_block(b, merge_block);
            SsaBlock *merge_blk = ssa_get_block(b->func, merge_block);
            if (merge_blk) {
                block_add_pred(merge_blk, rhs_block);
                block_add_pred(merge_blk, false_block);
            }

            SsaInst phi;
            memset(&phi, 0, sizeof(phi));
            phi.op = OP_PHI;
            phi.dst = result;
            phi.dst_type = hir->type;
            phi.phi.n = 2;
            phi.phi.srcs = arena_calloc(ctx->arena, 2, sizeof(uint32_t));
            phi.phi.preds = arena_calloc(ctx->arena, 2, sizeof(uint32_t));
            phi.phi.srcs[0] = rhs;
            phi.phi.preds[0] = rhs_block;
            phi.phi.srcs[1] = zero->dst;
            phi.phi.preds[1] = false_block;

            /* Insert PHI at beginning */
            if (merge_blk->ninsts == 0) {
                block_add_inst(merge_blk, &phi);
            } else {
                if (merge_blk->ninsts >= merge_blk->insts_cap) {
                    merge_blk->insts_cap = merge_blk->ninsts * 2;
                    merge_blk->insts = realloc(merge_blk->insts, sizeof(SsaInst) * merge_blk->insts_cap);
                }
                memmove(&merge_blk->insts[1], &merge_blk->insts[0], sizeof(SsaInst) * merge_blk->ninsts);
                merge_blk->insts[0] = phi;
                merge_blk->ninsts++;
            }

            ssa_builder_seal_block(b, merge_block);
            return result;
        }

        if (hir->binary_expr.op == TOK_PIPEPIPE) {
            uint32_t lhs = ssa_build_expr(ctx, hir->binary_expr.left);
            uint32_t result = ssa_builder_new_vreg(b);

            uint32_t rhs_block = ssa_builder_add_block(b);
            uint32_t merge_block = ssa_builder_add_block(b);
            uint32_t true_block = ssa_builder_add_block(b);

            uint32_t orig_block = b->cur_block;

            SsaInst br;
            memset(&br, 0, sizeof(br));
            br.op = OP_BRANCH;
            br.dst = VT_INVALID_VREG;
            br.branch.cond = lhs;
            br.branch.true_bb = true_block;
            br.branch.false_bb = rhs_block;
            ssa_builder_emit_inst(b, &br);

            SsaBlock *cur = ssa_get_block(b->func, orig_block);
            if (cur) {
                block_add_succ(cur, true_block);
                block_add_succ(cur, rhs_block);
            }

            /* true block */
            ssa_builder_set_cur_block(b, true_block);
            SsaBlock *true_blk = ssa_get_block(b->func, true_block);
            if (true_blk) block_add_pred(true_blk, orig_block);

            SsaInst *one = ssa_builder_emit(b, OP_CONST, vt_type_make(ctx->arena, VTTYPE_BOOL));
            one->const_.value = 1;

            SsaInst jmp_t;
            memset(&jmp_t, 0, sizeof(jmp_t));
            jmp_t.op = OP_JUMP;
            jmp_t.dst = VT_INVALID_VREG;
            jmp_t.jump.target_bb = merge_block;
            ssa_builder_emit_inst(b, &jmp_t);
            if (true_blk) block_add_succ(true_blk, merge_block);
            ssa_builder_seal_block(b, true_block);

            /* rhs block */
            ssa_builder_set_cur_block(b, rhs_block);
            SsaBlock *rhs_blk = ssa_get_block(b->func, rhs_block);
            if (rhs_blk) block_add_pred(rhs_blk, orig_block);
            uint32_t rhs = ssa_build_expr(ctx, hir->binary_expr.right);

            SsaInst jmp_r;
            memset(&jmp_r, 0, sizeof(jmp_r));
            jmp_r.op = OP_JUMP;
            jmp_r.dst = VT_INVALID_VREG;
            jmp_r.jump.target_bb = merge_block;
            ssa_builder_emit_inst(b, &jmp_r);
            if (rhs_blk) block_add_succ(rhs_blk, merge_block);
            ssa_builder_seal_block(b, rhs_block);

            /* merge */
            ssa_builder_set_cur_block(b, merge_block);
            SsaBlock *merge_blk = ssa_get_block(b->func, merge_block);
            if (merge_blk) {
                block_add_pred(merge_blk, true_block);
                block_add_pred(merge_blk, rhs_block);
            }

            SsaInst phi;
            memset(&phi, 0, sizeof(phi));
            phi.op = OP_PHI;
            phi.dst = result;
            phi.dst_type = hir->type;
            phi.phi.n = 2;
            phi.phi.srcs = arena_calloc(ctx->arena, 2, sizeof(uint32_t));
            phi.phi.preds = arena_calloc(ctx->arena, 2, sizeof(uint32_t));
            phi.phi.srcs[0] = one->dst;
            phi.phi.preds[0] = true_block;
            phi.phi.srcs[1] = rhs;
            phi.phi.preds[1] = rhs_block;

            if (merge_blk->ninsts == 0) {
                block_add_inst(merge_blk, &phi);
            } else {
                if (merge_blk->ninsts >= merge_blk->insts_cap) {
                    merge_blk->insts_cap = merge_blk->ninsts * 2;
                    merge_blk->insts = realloc(merge_blk->insts, sizeof(SsaInst) * merge_blk->insts_cap);
                }
                memmove(&merge_blk->insts[1], &merge_blk->insts[0], sizeof(SsaInst) * merge_blk->ninsts);
                merge_blk->insts[0] = phi;
                merge_blk->ninsts++;
            }

            ssa_builder_seal_block(b, merge_block);
            return result;
        }

        /* Regular binary expression */
        uint32_t lhs = ssa_build_expr(ctx, hir->binary_expr.left);
        uint32_t rhs = ssa_build_expr(ctx, hir->binary_expr.right);

        switch (hir->binary_expr.op) {
        case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH:
        case TOK_PERCENT: case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        case TOK_LSHIFT: case TOK_RSHIFT: {
            BinOpKind bk;
            switch (hir->binary_expr.op) {
            case TOK_PLUS:   bk = BINOP_ADD; break;
            case TOK_MINUS:  bk = BINOP_SUB; break;
            case TOK_STAR:   bk = BINOP_MUL; break;
            case TOK_SLASH:  bk = BINOP_DIV; break;
            case TOK_PERCENT:bk = BINOP_MOD; break;
            case TOK_AMP:   bk = BINOP_AND; break;
            case TOK_PIPE:  bk = BINOP_OR;  break;
            case TOK_CARET:  bk = BINOP_XOR; break;
            case TOK_LSHIFT: bk = BINOP_LSHIFT; break;
            case TOK_RSHIFT: bk = BINOP_RSHIFT; break;
            default: bk = BINOP_ADD; break;
            }
            SsaInst *inst = ssa_builder_emit(b, OP_BINOP, hir->type);
            inst->binop.kind = bk;
            inst->binop.lhs = lhs;
            inst->binop.rhs = rhs;
            return inst->dst;
        }
        case TOK_EQEQ: case TOK_NEQ: case TOK_LT: case TOK_GT:
        case TOK_LEQ: case TOK_GEQ: {
            CmpKind ck;
            switch (hir->binary_expr.op) {
            case TOK_EQEQ: ck = CMP_EQ;  break;
            case TOK_NEQ:  ck = CMP_NEQ; break;
            case TOK_LT:   ck = CMP_LT;  break;
            case TOK_GT:   ck = CMP_GT;  break;
            case TOK_LEQ:  ck = CMP_LEQ; break;
            case TOK_GEQ:  ck = CMP_GEQ; break;
            default: ck = CMP_EQ; break;
            }
            SsaInst *inst = ssa_builder_emit(b, OP_CMP, hir->type);
            inst->cmp.kind = ck;
            inst->cmp.lhs = lhs;
            inst->cmp.rhs = rhs;
            return inst->dst;
        }
        default:
            diag_msg(DIAG_ERROR, "unsupported binary operator in SSA");
            return VT_INVALID_VREG;
        }
    }
    case HIR_UNARY_EXPR: {
        uint32_t operand = ssa_build_expr(ctx, hir->unary_expr.operand);
        UnOpKind uk;
        switch (hir->unary_expr.op) {
        case TOK_MINUS: uk = UNOP_NEG;  break;
        case TOK_BANG:  uk = UNOP_NOT;  break;
        case TOK_TILDE: uk = UNOP_BNOT; break;
        default: uk = UNOP_NEG; break;
        }
        SsaInst *inst = ssa_builder_emit(b, OP_UNOP, hir->type);
        inst->unop.kind = uk;
        inst->unop.operand = operand;
        return inst->dst;
    }
    case HIR_CALL_EXPR: {
        /* Evaluate all arguments FIRST, before emitting the CALL instruction.
         * This ensures argument definitions (CONST, BINOP, etc.) appear before
         * the CALL in the instruction stream. */
        uint32_t *arg_vregs = arena_calloc(ctx->arena, hir->call_expr.nargs, sizeof(uint32_t));
        for (uint32_t i = 0; i < hir->call_expr.nargs; i++) {
            arg_vregs[i] = ssa_build_expr(ctx, hir->call_expr.args[i]);
        }

        /* Handle print as a special intrinsic */
        if (strcmp(hir->call_expr.name, "print") == 0) {
            if (hir->call_expr.nargs > 0) {
                SsaInst pi;
                memset(&pi, 0, sizeof(pi));
                pi.op = OP_PRINT;
                pi.dst = VT_INVALID_VREG;
                pi.print.val = arg_vregs[0];
                ssa_builder_emit_inst(b, &pi);
            }
            return VT_INVALID_VREG;
        }

        /* Now emit the CALL instruction after all arguments are ready */
        SsaInst *inst = ssa_builder_emit(b, OP_CALL, hir->type);
        inst->call.func_name = hir->call_expr.name;
        inst->call.nargs = hir->call_expr.nargs;
        inst->call.is_extern = true;
        inst->call.args = arg_vregs;

        return inst->dst;
    }
    case HIR_INDEX_EXPR: {
        uint32_t ptr = ssa_build_expr(ctx, hir->index_expr.object);
        uint32_t idx = ssa_build_expr(ctx, hir->index_expr.index);
        VtType *elem_type = hir->type;
        uint32_t elem_size = vt_type_size(elem_type);

        SsaInst *size_inst = ssa_builder_emit(b, OP_CONST, vt_type_make(ctx->arena, VTTYPE_I64));
        size_inst->const_.value = elem_size;

        SsaInst *offset = ssa_builder_emit(b, OP_BINOP, vt_type_make(ctx->arena, VTTYPE_I64));
        offset->binop.kind = BINOP_MUL;
        offset->binop.lhs = idx;
        offset->binop.rhs = size_inst->dst;

        SsaInst *gep = ssa_builder_emit(b, OP_GEP, hir->type);
        gep->gep.ptr = ptr;
        gep->gep.offset = (int64_t)offset->dst;

        SsaInst *load = ssa_builder_emit(b, OP_LOAD, hir->type);
        load->load.ptr = gep->dst;
        return load->dst;
    }
    case HIR_FIELD_EXPR: {
        uint32_t obj = ssa_build_expr(ctx, hir->field_expr.object);
        SsaInst *gep = ssa_builder_emit(b, OP_GEP, hir->type);
        gep->gep.ptr = obj;
        gep->gep.offset = (int64_t)hir->field_expr.field_offset;

        SsaInst *load = ssa_builder_emit(b, OP_LOAD, hir->type);
        load->load.ptr = gep->dst;
        return load->dst;
    }
    case HIR_ALLOC_EXPR: {
        SsaInst *inst = ssa_builder_emit(b, OP_ARENA_ALLOC, hir->type);
        inst->arena_alloc.arena_name = hir->alloc_expr.arena_name;
        inst->arena_alloc.type_size = hir->alloc_expr.alloc_size;
        return inst->dst;
    }
    case HIR_DEREF_EXPR: {
        uint32_t ptr = ssa_build_expr(ctx, hir->deref_expr.operand);
        SsaInst *load = ssa_builder_emit(b, OP_LOAD, hir->type);
        load->load.ptr = ptr;
        return load->dst;
    }
    case HIR_ADDR_EXPR: {
        return ssa_build_expr(ctx, hir->addr_expr.operand);
    }
    case HIR_CAST_EXPR: {
        uint32_t operand = ssa_build_expr(ctx, hir->cast_expr.operand);
        SsaInst *inst = ssa_builder_emit(b, OP_UNOP, hir->type);
        inst->unop.kind = UNOP_NEG;
        inst->unop.operand = operand;
        return inst->dst;
    }
    case HIR_SIZEOF_EXPR: {
        SsaInst *inst = ssa_builder_emit(b, OP_CONST, hir->type);
        inst->const_.value = hir->sizeof_expr.size;
        return inst->dst;
    }
    default:
        diag_msg(DIAG_ERROR, "unsupported HIR expression kind in SSA construction");
        return VT_INVALID_VREG;
    }
}

static void ssa_build_stmt(SsaBuildCtx *ctx, HirNode *hir) {
    if (!hir) return;

    SsaBuilder *b = ctx->builder;

    switch (hir->kind) {
    case HIR_BLOCK: {
        for (uint32_t i = 0; i < hir->block.nstmts; i++) {
            ssa_build_stmt(ctx, hir->block.stmts[i]);
        }
        break;
    }
    case HIR_LET_STMT: {
        uint32_t init_vreg = VT_INVALID_VREG;
        if (hir->let_stmt.init) {
            init_vreg = ssa_build_expr(ctx, hir->let_stmt.init);
        }
        if (init_vreg != VT_INVALID_VREG) {
            ssa_builder_write_var(b, hir->let_stmt.name, init_vreg);
        } else {
            SsaInst *zero = ssa_builder_emit(b, OP_CONST, hir->let_stmt.type);
            zero->const_.value = 0;
            ssa_builder_write_var(b, hir->let_stmt.name, zero->dst);
        }
        break;
    }
    case HIR_ASSIGN_STMT: {
        uint32_t val = ssa_build_expr(ctx, hir->assign_stmt.value);

        if (hir->assign_stmt.compound_op != TOK_EQ) {
            uint32_t target = ssa_build_expr(ctx, hir->assign_stmt.target);
            BinOpKind bk;
            switch (hir->assign_stmt.compound_op) {
            case TOK_PLUSEQ:  bk = BINOP_ADD; break;
            case TOK_MINUSEQ: bk = BINOP_SUB; break;
            case TOK_STAREQ:  bk = BINOP_MUL; break;
            case TOK_SLASHEQ: bk = BINOP_DIV; break;
            default: bk = BINOP_ADD; break;
            }
            SsaInst *binop = ssa_builder_emit(b, OP_BINOP, hir->assign_stmt.target->type);
            binop->binop.kind = bk;
            binop->binop.lhs = target;
            binop->binop.rhs = val;
            val = binop->dst;
        }

        if (hir->assign_stmt.target->kind == HIR_IDENT_EXPR) {
            ssa_builder_write_var(b, hir->assign_stmt.target->ident_expr.name, val);
        } else if (hir->assign_stmt.target->kind == HIR_DEREF_EXPR) {
            uint32_t ptr = ssa_build_expr(ctx, hir->assign_stmt.target->deref_expr.operand);
            SsaInst *store = ssa_builder_emit(b, OP_STORE, NULL);
            store->dst = VT_INVALID_VREG;
            store->store.ptr = ptr;
            store->store.val = val;
        } else if (hir->assign_stmt.target->kind == HIR_INDEX_EXPR) {
            uint32_t ptr = ssa_build_expr(ctx, hir->assign_stmt.target->index_expr.object);
            uint32_t idx = ssa_build_expr(ctx, hir->assign_stmt.target->index_expr.index);
            VtType *elem_type = hir->assign_stmt.target->type;
            uint32_t elem_size = vt_type_size(elem_type);

            SsaInst *size_inst = ssa_builder_emit(b, OP_CONST, vt_type_make(ctx->arena, VTTYPE_I64));
            size_inst->const_.value = elem_size;
            SsaInst *offset = ssa_builder_emit(b, OP_BINOP, vt_type_make(ctx->arena, VTTYPE_I64));
            offset->binop.kind = BINOP_MUL;
            offset->binop.lhs = idx;
            offset->binop.rhs = size_inst->dst;

            SsaInst *gep = ssa_builder_emit(b, OP_GEP, elem_type);
            gep->gep.ptr = ptr;
            gep->gep.offset = (int64_t)offset->dst;

            SsaInst *store = ssa_builder_emit(b, OP_STORE, NULL);
            store->dst = VT_INVALID_VREG;
            store->store.ptr = gep->dst;
            store->store.val = val;
        } else if (hir->assign_stmt.target->kind == HIR_FIELD_EXPR) {
            uint32_t obj = ssa_build_expr(ctx, hir->assign_stmt.target->field_expr.object);
            SsaInst *gep = ssa_builder_emit(b, OP_GEP, hir->assign_stmt.target->type);
            gep->gep.ptr = obj;
            gep->gep.offset = (int64_t)hir->assign_stmt.target->field_expr.field_offset;

            SsaInst *store = ssa_builder_emit(b, OP_STORE, NULL);
            store->dst = VT_INVALID_VREG;
            store->store.ptr = gep->dst;
            store->store.val = val;
        }
        break;
    }
    case HIR_IF_STMT: {
        uint32_t cond = ssa_build_expr(ctx, hir->if_stmt.condition);
        uint32_t orig_block = b->cur_block;

        if (hir->if_stmt.else_block) {
            uint32_t then_bb = ssa_builder_add_block(b);
            uint32_t else_bb = ssa_builder_add_block(b);
            uint32_t merge_bb = ssa_builder_add_block(b);

            SsaInst br;
            memset(&br, 0, sizeof(br));
            br.op = OP_BRANCH;
            br.dst = VT_INVALID_VREG;
            br.branch.cond = cond;
            br.branch.true_bb = then_bb;
            br.branch.false_bb = else_bb;
            ssa_builder_emit_inst(b, &br);

            SsaBlock *cur = ssa_get_block(b->func, orig_block);
            if (cur) {
                block_add_succ(cur, then_bb);
                block_add_succ(cur, else_bb);
            }

            /* Then block */
            ssa_builder_set_cur_block(b, then_bb);
            SsaBlock *then_blk = ssa_get_block(b->func, then_bb);
            if (then_blk) block_add_pred(then_blk, orig_block);
            ssa_build_stmt(ctx, hir->if_stmt.then_block);

            /* If then block doesn't already have a terminator, add jump to merge */
            SsaBlock *then_after = ssa_get_block(b->func, b->cur_block);
            if (then_after && then_after->ninsts > 0) {
                SsaInst *last = &then_after->insts[then_after->ninsts - 1];
                if (last->op != OP_RET && last->op != OP_JUMP && last->op != OP_BRANCH) {
                    SsaInst jmp;
                    memset(&jmp, 0, sizeof(jmp));
                    jmp.op = OP_JUMP;
                    jmp.dst = VT_INVALID_VREG;
                    jmp.jump.target_bb = merge_bb;
                    ssa_builder_emit_inst(b, &jmp);
                    if (then_after) block_add_succ(then_after, merge_bb);
                }
            }
            ssa_builder_seal_block(b, then_bb);

            /* Else block */
            ssa_builder_set_cur_block(b, else_bb);
            SsaBlock *else_blk = ssa_get_block(b->func, else_bb);
            if (else_blk) block_add_pred(else_blk, orig_block);
            ssa_build_stmt(ctx, hir->if_stmt.else_block);

            SsaBlock *else_after = ssa_get_block(b->func, b->cur_block);
            if (else_after && else_after->ninsts > 0) {
                SsaInst *last = &else_after->insts[else_after->ninsts - 1];
                if (last->op != OP_RET && last->op != OP_JUMP && last->op != OP_BRANCH) {
                    SsaInst jmp;
                    memset(&jmp, 0, sizeof(jmp));
                    jmp.op = OP_JUMP;
                    jmp.dst = VT_INVALID_VREG;
                    jmp.jump.target_bb = merge_bb;
                    ssa_builder_emit_inst(b, &jmp);
                    if (else_after) block_add_succ(else_after, merge_bb);
                }
            }
            ssa_builder_seal_block(b, else_bb);

            /* Merge */
            ssa_builder_set_cur_block(b, merge_bb);
            SsaBlock *merge_blk = ssa_get_block(b->func, merge_bb);
            /* Add predecessors - need to figure out which blocks actually jump to merge */
            /* For simple if/else, it's the then_bb and else_bb */
            if (merge_blk) {
                block_add_pred(merge_blk, then_bb);
                block_add_pred(merge_blk, else_bb);
            }
            ssa_builder_seal_block(b, merge_bb);
        } else {
            uint32_t then_bb = ssa_builder_add_block(b);
            uint32_t merge_bb = ssa_builder_add_block(b);

            SsaInst br;
            memset(&br, 0, sizeof(br));
            br.op = OP_BRANCH;
            br.dst = VT_INVALID_VREG;
            br.branch.cond = cond;
            br.branch.true_bb = then_bb;
            br.branch.false_bb = merge_bb;
            ssa_builder_emit_inst(b, &br);

            SsaBlock *cur = ssa_get_block(b->func, orig_block);
            if (cur) {
                block_add_succ(cur, then_bb);
                block_add_succ(cur, merge_bb);
            }

            /* Then block */
            ssa_builder_set_cur_block(b, then_bb);
            SsaBlock *then_blk = ssa_get_block(b->func, then_bb);
            if (then_blk) block_add_pred(then_blk, orig_block);
            ssa_build_stmt(ctx, hir->if_stmt.then_block);

            SsaBlock *then_after = ssa_get_block(b->func, b->cur_block);
            SsaInst jmp;
            memset(&jmp, 0, sizeof(jmp));
            jmp.op = OP_JUMP;
            jmp.dst = VT_INVALID_VREG;
            jmp.jump.target_bb = merge_bb;
            ssa_builder_emit_inst(b, &jmp);
            if (then_after) block_add_succ(then_after, merge_bb);
            ssa_builder_seal_block(b, then_bb);

            /* Merge */
            ssa_builder_set_cur_block(b, merge_bb);
            SsaBlock *merge_blk = ssa_get_block(b->func, merge_bb);
            if (merge_blk) {
                block_add_pred(merge_blk, orig_block);
                block_add_pred(merge_blk, then_bb);
            }
            ssa_builder_seal_block(b, merge_bb);
        }
        break;
    }
    case HIR_WHILE_STMT: {
        uint32_t orig_block = b->cur_block;
        uint32_t header_bb = ssa_builder_add_block(b);
        uint32_t body_bb = ssa_builder_add_block(b);
        uint32_t exit_bb = ssa_builder_add_block(b);

        /* Jump from current block to header */
        SsaInst jmp_header;
        memset(&jmp_header, 0, sizeof(jmp_header));
        jmp_header.op = OP_JUMP;
        jmp_header.dst = VT_INVALID_VREG;
        jmp_header.jump.target_bb = header_bb;
        ssa_builder_emit_inst(b, &jmp_header);

        SsaBlock *cur = ssa_get_block(b->func, orig_block);
        if (cur) block_add_succ(cur, header_bb);

        /* Header block - DO NOT SEAL YET (back edge from body not yet known) */
        ssa_builder_set_cur_block(b, header_bb);
        SsaBlock *header_blk = ssa_get_block(b->func, header_bb);
        if (header_blk) block_add_pred(header_blk, orig_block);
        uint32_t cond = ssa_build_expr(ctx, hir->while_stmt.condition);

        SsaInst br;
        memset(&br, 0, sizeof(br));
        br.op = OP_BRANCH;
        br.dst = VT_INVALID_VREG;
        br.branch.cond = cond;
        br.branch.true_bb = body_bb;
        br.branch.false_bb = exit_bb;
        ssa_builder_emit_inst(b, &br);
        if (header_blk) {
            block_add_succ(header_blk, body_bb);
            block_add_succ(header_blk, exit_bb);
        }

        /* Body block */
        ssa_builder_set_cur_block(b, body_bb);
        SsaBlock *body_blk = ssa_get_block(b->func, body_bb);
        if (body_blk) block_add_pred(body_blk, header_bb);

        ssa_build_stmt(ctx, hir->while_stmt.body);

        SsaInst jmp_body;
        memset(&jmp_body, 0, sizeof(jmp_body));
        jmp_body.op = OP_JUMP;
        jmp_body.dst = VT_INVALID_VREG;
        jmp_body.jump.target_bb = header_bb;
        ssa_builder_emit_inst(b, &jmp_body);
        SsaBlock *body_after = ssa_get_block(b->func, b->cur_block);
        if (body_after) {
            block_add_succ(body_after, header_bb);
        }

        /* NOW seal header - both predecessors (orig and body back edge) are known */
        /* Add the body back edge as a predecessor to the header */
        if (header_blk && body_after) {
            block_add_pred(header_blk, ssa_get_block(b->func, body_bb) ? body_bb : b->cur_block);
        }
        ssa_builder_seal_block(b, header_bb);
        ssa_builder_seal_block(b, body_bb);

        /* Exit block */
        ssa_builder_set_cur_block(b, exit_bb);
        SsaBlock *exit_blk = ssa_get_block(b->func, exit_bb);
        if (exit_blk) block_add_pred(exit_blk, header_bb);
        ssa_builder_seal_block(b, exit_bb);
        break;
    }
    case HIR_FOR_STMT: {
        /* Init */
        if (hir->for_stmt.init) {
            ssa_build_stmt(ctx, hir->for_stmt.init);
        }

        uint32_t orig_block = b->cur_block;
        uint32_t header_bb = ssa_builder_add_block(b);
        uint32_t body_bb = ssa_builder_add_block(b);
        uint32_t update_bb = ssa_builder_add_block(b);
        uint32_t exit_bb = ssa_builder_add_block(b);

        SsaInst jmp_h;
        memset(&jmp_h, 0, sizeof(jmp_h));
        jmp_h.op = OP_JUMP;
        jmp_h.dst = VT_INVALID_VREG;
        jmp_h.jump.target_bb = header_bb;
        ssa_builder_emit_inst(b, &jmp_h);

        SsaBlock *cur = ssa_get_block(b->func, orig_block);
        if (cur) block_add_succ(cur, header_bb);

        /* Header - DO NOT SEAL YET (back edge from update not yet known) */
        ssa_builder_set_cur_block(b, header_bb);
        SsaBlock *header_blk = ssa_get_block(b->func, header_bb);
        if (header_blk) block_add_pred(header_blk, orig_block);

        uint32_t cond = VT_INVALID_VREG;
        if (hir->for_stmt.condition) {
            cond = ssa_build_expr(ctx, hir->for_stmt.condition);
        } else {
            SsaInst *one = ssa_builder_emit(b, OP_CONST, vt_type_make(ctx->arena, VTTYPE_BOOL));
            one->const_.value = 1;
            cond = one->dst;
        }

        SsaInst br;
        memset(&br, 0, sizeof(br));
        br.op = OP_BRANCH;
        br.dst = VT_INVALID_VREG;
        br.branch.cond = cond;
        br.branch.true_bb = body_bb;
        br.branch.false_bb = exit_bb;
        ssa_builder_emit_inst(b, &br);
        if (header_blk) {
            block_add_succ(header_blk, body_bb);
            block_add_succ(header_blk, exit_bb);
        }

        /* Body */
        ssa_builder_set_cur_block(b, body_bb);
        SsaBlock *body_blk = ssa_get_block(b->func, body_bb);
        if (body_blk) block_add_pred(body_blk, header_bb);

        ssa_build_stmt(ctx, hir->for_stmt.body);

        SsaInst jmp_upd;
        memset(&jmp_upd, 0, sizeof(jmp_upd));
        jmp_upd.op = OP_JUMP;
        jmp_upd.dst = VT_INVALID_VREG;
        jmp_upd.jump.target_bb = update_bb;
        ssa_builder_emit_inst(b, &jmp_upd);
        SsaBlock *body_after = ssa_get_block(b->func, b->cur_block);
        if (body_after) block_add_succ(body_after, update_bb);
        ssa_builder_seal_block(b, body_bb);

        /* Update */
        ssa_builder_set_cur_block(b, update_bb);
        SsaBlock *update_blk = ssa_get_block(b->func, update_bb);
        if (update_blk) block_add_pred(update_blk, body_bb);

        if (hir->for_stmt.update) {
            ssa_build_expr(ctx, hir->for_stmt.update);
        }

        SsaInst jmp_back;
        memset(&jmp_back, 0, sizeof(jmp_back));
        jmp_back.op = OP_JUMP;
        jmp_back.dst = VT_INVALID_VREG;
        jmp_back.jump.target_bb = header_bb;
        ssa_builder_emit_inst(b, &jmp_back);
        SsaBlock *update_after = ssa_get_block(b->func, b->cur_block);
        if (update_after) {
            block_add_succ(update_after, header_bb);
        }

        /* NOW seal header - all predecessors known */
        if (header_blk && update_after) {
            block_add_pred(header_blk, update_bb);
        }
        ssa_builder_seal_block(b, header_bb);
        ssa_builder_seal_block(b, update_bb);

        /* Exit */
        ssa_builder_set_cur_block(b, exit_bb);
        SsaBlock *exit_blk = ssa_get_block(b->func, exit_bb);
        if (exit_blk) block_add_pred(exit_blk, header_bb);
        ssa_builder_seal_block(b, exit_bb);
        break;
    }
    case HIR_RETURN_STMT: {
        if (hir->return_stmt.value) {
            uint32_t val = ssa_build_expr(ctx, hir->return_stmt.value);
            SsaInst *ret = ssa_builder_emit(b, OP_RET, NULL);
            ret->dst = VT_INVALID_VREG;
            ret->ret.val = val;
        } else {
            SsaInst *ret = ssa_builder_emit(b, OP_RET, NULL);
            ret->dst = VT_INVALID_VREG;
            ret->ret.val = VT_INVALID_VREG;
        }
        break;
    }
    case HIR_EXPR_STMT: {
        ssa_build_expr(ctx, hir->expr_stmt.expr);
        break;
    }
    case HIR_ARENA_RESET_STMT: {
        SsaInst *inst = ssa_builder_emit(b, OP_ARENA_RESET, NULL);
        inst->dst = VT_INVALID_VREG;
        inst->arena_reset.arena_name = hir->arena_reset_stmt.name;
        break;
    }
    default:
        ssa_build_expr(ctx, hir);
        break;
    }
}

/* ---- Build SSA for a single function ---- */
static SsaFunc *ssa_build_func(Arena *arena, HirNode *hir_fn) {
    SsaFunc *func = arena_calloc(arena, 1, sizeof(SsaFunc));
    func->name = hir_fn->fn_decl.name;
    func->ret_type = hir_fn->fn_decl.ret_type;
    func->nparams = hir_fn->fn_decl.nparams;
    func->next_vreg = hir_fn->fn_decl.nparams;

    func->param_names = arena_calloc(arena, func->nparams, sizeof(char *));
    func->param_vregs = arena_calloc(arena, func->nparams, sizeof(uint32_t));
    func->param_types = arena_calloc(arena, func->nparams, sizeof(VtType *));

    for (uint32_t i = 0; i < func->nparams; i++) {
        func->param_names[i] = hir_fn->fn_decl.params[i].name;
        func->param_vregs[i] = i;
        func->param_types[i] = hir_fn->fn_decl.params[i].type;
    }

    /* Create entry block */
    func->blocks_cap = 16;
    func->blocks = calloc(func->blocks_cap, sizeof(SsaBlock));
    func->entry = 0;
    func->nblocks = 1;
    memset(&func->blocks[0], 0, sizeof(SsaBlock));
    func->blocks[0].label = 0;
    func->blocks[0].sealed = true;

    /* Build */
    SsaBuilder *builder = ssa_builder_create(arena, func);
    SsaBuildCtx ctx;
    ctx.arena = arena;
    ctx.builder = builder;
    ctx.hir_func = hir_fn;

    ssa_builder_set_cur_block(builder, 0);

    if (hir_fn->fn_decl.body) {
        ssa_build_stmt(&ctx, hir_fn->fn_decl.body);
    }

    /* Ensure the last block has a terminator */
    SsaBlock *last = ssa_get_block(func, builder->cur_block);
    if (last && last->ninsts > 0) {
        SsaInst *last_inst = &last->insts[last->ninsts - 1];
        if (last_inst->op != OP_RET && last_inst->op != OP_JUMP && last_inst->op != OP_BRANCH) {
            SsaInst ret;
            memset(&ret, 0, sizeof(ret));
            ret.op = OP_RET;
            ret.dst = VT_INVALID_VREG;
            ret.ret.val = VT_INVALID_VREG;
            ssa_builder_emit_inst(builder, &ret);
        }
    }

    func->next_vreg = builder->next_vreg;
    return func;
}

/* ---- Build SSA for entire program ---- */
SsaProgram *ssa_build_program(Arena *arena, HirNode *hir) {
    if (!hir || hir->kind != HIR_PROGRAM) {
        diag_msg(DIAG_FATAL, "expected HIR_PROGRAM for SSA construction");
    }

    uint32_t fn_count = 0;
    for (uint32_t i = 0; i < hir->program.ndecls; i++) {
        if (hir->program.decls[i]->kind == HIR_FN_DECL) fn_count++;
    }

    SsaProgram *prog = arena_calloc(arena, 1, sizeof(SsaProgram));
    prog->funcs = arena_calloc(arena, fn_count, sizeof(SsaFunc));
    prog->nfuncs = fn_count;

    uint32_t fi = 0;
    for (uint32_t i = 0; i < hir->program.ndecls; i++) {
        if (hir->program.decls[i]->kind == HIR_FN_DECL) {
            prog->funcs[fi] = *ssa_build_func(arena, hir->program.decls[i]);
            fi++;
        }
    }

    return prog;
}

/* ---- Print SSA ---- */
void ssa_print_func(SsaFunc *func) {
    printf("fn %s(", func->name);
    for (uint32_t i = 0; i < func->nparams; i++) {
        if (i > 0) printf(", ");
        printf("v%u", func->param_vregs[i]);
    }
    printf(") {\n");

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        printf("  bb%u:", blk->label);

        if (blk->npreds > 0) {
            printf("  ; preds:");
            for (uint32_t i = 0; i < blk->npreds; i++)
                printf(" bb%u", blk->preds[i]);
        }
        printf("\n");

        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            if (inst->dst != VT_INVALID_VREG) {
                printf("    v%u = ", inst->dst);
            } else {
                printf("    ");
            }
            printf("%s", ssa_op_str(inst->op));
            switch (inst->op) {
            case OP_CONST:
                printf(" %lld", (long long)inst->const_.value);
                break;
            case OP_BINOP:
                printf(" %s, v%u, v%u", binop_str(inst->binop.kind),
                       inst->binop.lhs, inst->binop.rhs);
                break;
            case OP_UNOP:
                printf(" %s, v%u", unop_str(inst->unop.kind), inst->unop.operand);
                break;
            case OP_CMP:
                printf(" %s, v%u, v%u", cmp_str(inst->cmp.kind),
                       inst->cmp.lhs, inst->cmp.rhs);
                break;
            case OP_LOAD:
                printf(" v%u", inst->load.ptr);
                break;
            case OP_STORE:
                printf(" v%u, v%u", inst->store.ptr, inst->store.val);
                break;
            case OP_GEP:
                printf(" v%u, %lld", inst->gep.ptr, (long long)inst->gep.offset);
                break;
            case OP_BRANCH:
                printf(" v%u, bb%u, bb%u", inst->branch.cond,
                       inst->branch.true_bb, inst->branch.false_bb);
                break;
            case OP_JUMP:
                printf(" bb%u", inst->jump.target_bb);
                break;
            case OP_CALL:
                printf(" %s(", inst->call.func_name);
                for (uint32_t i = 0; i < inst->call.nargs; i++) {
                    if (i > 0) printf(", ");
                    printf("v%u", inst->call.args[i]);
                }
                printf(")");
                break;
            case OP_RET:
                if (inst->ret.val != VT_INVALID_VREG)
                    printf(" v%u", inst->ret.val);
                break;
            case OP_PHI:
                for (uint32_t i = 0; i < inst->phi.n; i++) {
                    if (i > 0) printf(",");
                    printf(" bb%u:v%u", inst->phi.preds[i], inst->phi.srcs[i]);
                }
                break;
            case OP_ALLOC:
                printf(" %llu", (unsigned long long)inst->alloc.bytes);
                break;
            case OP_ARENA_ALLOC:
                printf(" %s, %llu", inst->arena_alloc.arena_name,
                       (unsigned long long)inst->arena_alloc.type_size);
                break;
            case OP_ARENA_RESET:
                printf(" %s", inst->arena_reset.arena_name);
                break;
            case OP_PRINT:
                printf(" v%u", inst->print.val);
                break;
            }
            printf("\n");
        }
    }
    printf("}\n");
}

void ssa_print_program(SsaProgram *prog) {
    for (uint32_t i = 0; i < prog->nfuncs; i++) {
        ssa_print_func(&prog->funcs[i]);
        printf("\n");
    }
}
