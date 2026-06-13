/*
 * VORTECH Compiler - Dead Code Elimination
 *
 * Tiny complexity. Huge value.
 * Remove unused work.
 */
#include "opt.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Mark which vregs are used (referenced) */
static void mark_used(SsaFunc *func, bool *used) {
    /* Clear all marks */
    memset(used, 0, func->next_vreg * sizeof(bool));

    /* Mark all vregs that appear as operands */
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            switch (inst->op) {
            case OP_BINOP:
                used[inst->binop.lhs] = true;
                used[inst->binop.rhs] = true;
                break;
            case OP_UNOP:
                used[inst->unop.operand] = true;
                break;
            case OP_LOAD:
                used[inst->load.ptr] = true;
                break;
            case OP_STORE:
                used[inst->store.ptr] = true;
                used[inst->store.val] = true;
                break;
            case OP_GEP:
                used[inst->gep.ptr] = true;
                break;
            case OP_CMP:
                used[inst->cmp.lhs] = true;
                used[inst->cmp.rhs] = true;
                break;
            case OP_BRANCH:
                used[inst->branch.cond] = true;
                break;
            case OP_CALL:
                for (uint32_t i = 0; i < inst->call.nargs; i++) {
                    used[inst->call.args[i]] = true;
                }
                break;
            case OP_RET:
                if (inst->ret.val != VT_INVALID_VREG)
                    used[inst->ret.val] = true;
                break;
            case OP_PHI:
                for (uint32_t i = 0; i < inst->phi.n; i++) {
                    used[inst->phi.srcs[i]] = true;
                }
                break;
            case OP_PRINT:
                used[inst->print.val] = true;
                break;
            default:
                break;
            }
        }
    }

    /* Mark function parameters as used (they're inputs) */
    for (uint32_t i = 0; i < func->nparams; i++) {
        used[func->param_vregs[i]] = true;
    }
}

/* Check if an instruction has side effects (must be kept) */
static bool has_side_effects(SsaInst *inst) {
    switch (inst->op) {
    case OP_STORE:
    case OP_CALL:
    case OP_RET:
    case OP_BRANCH:
    case OP_JUMP:
    case OP_ARENA_RESET:
    case OP_PRINT:
        return true;
    default:
        return false;
    }
}

void opt_dce(SsaFunc *func) {
    if (func->next_vreg == 0) return;

    bool *used = calloc(func->next_vreg, sizeof(bool));
    if (!used) {
        diag_msg(DIAG_FATAL, "out of memory in DCE");
    }

    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 10;

    while (changed && iterations < MAX_ITERATIONS) {
        changed = false;
        iterations++;

        mark_used(func, used);

        for (uint32_t bi = 0; bi < func->nblocks; bi++) {
            SsaBlock *blk = &func->blocks[bi];
            uint32_t write_idx = 0;

            for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
                SsaInst *inst = &blk->insts[ii];

                /* Keep instruction if: it has side effects, its dst is used, or no dst */
                bool keep = has_side_effects(inst) ||
                           (inst->dst == VT_INVALID_VREG) ||
                           used[inst->dst];

                if (keep) {
                    if (write_idx != ii) {
                        blk->insts[write_idx] = blk->insts[ii];
                    }
                    write_idx++;
                } else {
                    changed = true;
                }
            }
            blk->ninsts = write_idx;
        }
    }

    free(used);
}
