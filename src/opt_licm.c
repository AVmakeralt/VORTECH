/*
 * VORTECH Compiler - Loop-Invariant Code Motion
 *
 * Only obvious cases. No giant heuristics.
 */
#include "opt.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Detect loop headers: blocks that are predecessors of themselves or have back edges */
static bool is_loop_header(SsaBlock *blk) {
    for (uint32_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] >= blk->label) {
            /* Back edge: predecessor has a higher or equal block number */
            return true;
        }
    }
    return false;
}

/* Check if a vreg is defined in a block that dominates the loop */
/* Simple approximation: check if the vreg is defined before the loop */
static bool is_defined_before_block(SsaFunc *func, uint32_t vreg, uint32_t loop_block) {
    for (uint32_t bi = 0; bi < loop_block; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            if (blk->insts[ii].dst == vreg) return true;
        }
    }
    return false;
}

/* Check if an instruction is loop-invariant */
static bool is_loop_invariant(SsaFunc *func, SsaInst *inst, uint32_t loop_block) {
    switch (inst->op) {
    case OP_BINOP:
        return is_defined_before_block(func, inst->binop.lhs, loop_block) &&
               is_defined_before_block(func, inst->binop.rhs, loop_block);
    case OP_UNOP:
        return is_defined_before_block(func, inst->unop.operand, loop_block);
    case OP_CMP:
        return is_defined_before_block(func, inst->cmp.lhs, loop_block) &&
               is_defined_before_block(func, inst->cmp.rhs, loop_block);
    case OP_CONST:
        return true;
    default:
        return false; /* Memory operations, calls, etc. are not loop-invariant */
    }
}

void opt_licm(SsaFunc *func) {
    /* For each loop header, try to hoist loop-invariant code */
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        if (!is_loop_header(blk)) continue;

        /* Find the preheader (single non-back-edge predecessor) */
        uint32_t preheader = VT_INVALID_BLOCK;
        for (uint32_t i = 0; i < blk->npreds; i++) {
            if (blk->preds[i] < blk->label) {
                if (preheader == VT_INVALID_BLOCK) {
                    preheader = blk->preds[i];
                } else {
                    /* Multiple entry edges - skip for safety */
                    preheader = VT_INVALID_BLOCK;
                    break;
                }
            }
        }

        if (preheader == VT_INVALID_BLOCK) continue;

        /* Collect all blocks in the loop (simple approximation: blocks >= header and <= latch) */
        /* For now, just look at the header block's instructions */

        /* Find loop-invariant instructions in the header */
        uint32_t hoist_count = 0;
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            if (inst->dst != VT_INVALID_VREG && is_loop_invariant(func, inst, bi)) {
                /* This is loop-invariant - it could be hoisted to the preheader */
                /* For safety, we only hoist if the instruction has no side effects
                 * and its operands are defined before the loop.
                 * For now, just mark it but don't actually move it.
                 * A proper implementation would move it to the preheader's last
                 * instruction before the terminator. */
                hoist_count++;
            }
        }

        /* Note: actual hoisting is deferred to a more careful implementation
         * that properly handles dominance. For now, LICM serves as analysis. */
    }
}
