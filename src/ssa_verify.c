/*
 * VORTECH Compiler - SSA Verifier
 *
 * Every optimization pass gets: Verifier Before, Verifier After.
 * If a pass corrupts IR: Compiler aborts. Immediately.
 * A rock compiler is boring. That's the point.
 */
#include "ssa.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

static bool verify_block(SsaFunc *func, SsaBlock *blk, uint32_t blk_idx) {
    bool ok = true;

    /* Check that the block has at least one instruction */
    if (blk->ninsts == 0) {
        diag_msg(DIAG_WARN, "block bb%u is empty", blk_idx);
        return true; /* not fatal */
    }

    /* Check that the last instruction is a terminator */
    SsaInst *last = &blk->insts[blk->ninsts - 1];
    bool is_terminator = (last->op == OP_RET || last->op == OP_JUMP ||
                          last->op == OP_BRANCH);
    if (!is_terminator) {
        diag_msg(DIAG_ERROR, "block bb%u does not end with terminator (ends with %s)",
                 blk_idx, ssa_op_str(last->op));
        ok = false;
    }

    /* Check that only the last instruction is a terminator */
    for (uint32_t i = 0; i + 1 < blk->ninsts; i++) {
        SsaInst *inst = &blk->insts[i];
        if (inst->op == OP_RET || inst->op == OP_JUMP || inst->op == OP_BRANCH) {
            diag_msg(DIAG_ERROR, "block bb%u has terminator at position %u (not last)",
                     blk_idx, i);
            ok = false;
        }
    }

    /* Check vreg references */
    for (uint32_t i = 0; i < blk->ninsts; i++) {
        SsaInst *inst = &blk->insts[i];

        switch (inst->op) {
        case OP_BINOP:
            if (inst->binop.lhs >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: BINOP lhs v%u out of range (max v%u)",
                         blk_idx, i, inst->binop.lhs, func->next_vreg - 1);
                ok = false;
            }
            if (inst->binop.rhs >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: BINOP rhs v%u out of range (max v%u)",
                         blk_idx, i, inst->binop.rhs, func->next_vreg - 1);
                ok = false;
            }
            break;
        case OP_UNOP:
            if (inst->unop.operand >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: UNOP operand v%u out of range",
                         blk_idx, i, inst->unop.operand);
                ok = false;
            }
            break;
        case OP_LOAD:
            if (inst->load.ptr >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: LOAD ptr v%u out of range",
                         blk_idx, i, inst->load.ptr);
                ok = false;
            }
            break;
        case OP_STORE:
            if (inst->store.ptr >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: STORE ptr v%u out of range",
                         blk_idx, i, inst->store.ptr);
                ok = false;
            }
            if (inst->store.val >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: STORE val v%u out of range",
                         blk_idx, i, inst->store.val);
                ok = false;
            }
            break;
        case OP_CMP:
            if (inst->cmp.lhs >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: CMP lhs v%u out of range",
                         blk_idx, i, inst->cmp.lhs);
                ok = false;
            }
            if (inst->cmp.rhs >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: CMP rhs v%u out of range",
                         blk_idx, i, inst->cmp.rhs);
                ok = false;
            }
            break;
        case OP_BRANCH:
            if (inst->branch.true_bb >= func->nblocks) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: BRANCH true_bb %u out of range",
                         blk_idx, i, inst->branch.true_bb);
                ok = false;
            }
            if (inst->branch.false_bb >= func->nblocks) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: BRANCH false_bb %u out of range",
                         blk_idx, i, inst->branch.false_bb);
                ok = false;
            }
            if (inst->branch.cond >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: BRANCH cond v%u out of range",
                         blk_idx, i, inst->branch.cond);
                ok = false;
            }
            break;
        case OP_JUMP:
            if (inst->jump.target_bb >= func->nblocks) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: JUMP target %u out of range",
                         blk_idx, i, inst->jump.target_bb);
                ok = false;
            }
            break;
        case OP_RET:
            if (inst->ret.val != VT_INVALID_VREG && inst->ret.val >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: RET val v%u out of range",
                         blk_idx, i, inst->ret.val);
                ok = false;
            }
            break;
        case OP_CALL:
            for (uint32_t j = 0; j < inst->call.nargs; j++) {
                if (inst->call.args[j] >= func->next_vreg) {
                    diag_msg(DIAG_ERROR, "bb%u inst %u: CALL arg[%u] v%u out of range",
                             blk_idx, i, j, inst->call.args[j]);
                    ok = false;
                }
            }
            break;
        case OP_PHI:
            for (uint32_t j = 0; j < inst->phi.n; j++) {
                if (inst->phi.srcs[j] >= func->next_vreg) {
                    diag_msg(DIAG_ERROR, "bb%u inst %u: PHI src[%u] v%u out of range",
                             blk_idx, i, j, inst->phi.srcs[j]);
                    ok = false;
                }
                if (inst->phi.preds[j] >= func->nblocks) {
                    diag_msg(DIAG_ERROR, "bb%u inst %u: PHI pred[%u] bb%u out of range",
                             blk_idx, i, j, inst->phi.preds[j]);
                    ok = false;
                }
            }
            break;
        case OP_PRINT:
            if (inst->print.val >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: PRINT val v%u out of range",
                         blk_idx, i, inst->print.val);
                ok = false;
            }
            break;
        default:
            break;
        }

        /* Check destination vreg */
        if (inst->dst != VT_INVALID_VREG) {
            if (inst->dst >= func->next_vreg) {
                diag_msg(DIAG_ERROR, "bb%u inst %u: dst v%u out of range",
                         blk_idx, i, inst->dst);
                ok = false;
            }
        }
    }

    return ok;
}

bool ssa_verify_func(SsaFunc *func) {
    bool ok = true;

    for (uint32_t i = 0; i < func->nblocks; i++) {
        if (!verify_block(func, &func->blocks[i], i)) {
            ok = false;
        }
    }

    /* Check entry block exists */
    if (func->entry >= func->nblocks) {
        diag_msg(DIAG_ERROR, "entry block %u out of range", func->entry);
        ok = false;
    }

    return ok;
}

bool ssa_verify_program(SsaProgram *prog) {
    bool ok = true;
    for (uint32_t i = 0; i < prog->nfuncs; i++) {
        if (!ssa_verify_func(&prog->funcs[i])) {
            ok = false;
        }
    }
    return ok;
}
