/*
 * VORTECH Compiler - Copy Propagation
 *
 * Tiny complexity. Huge value.
 * a = b; c = a; -> c = b
 */
#include "opt.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Build a copy map: vreg -> source vreg for simple copies */
static uint32_t *build_copy_map(SsaFunc *func) {
    uint32_t *map = malloc(func->next_vreg * sizeof(uint32_t));
    if (!map) {
        diag_msg(DIAG_FATAL, "out of memory in copy propagation");
    }

    /* Initialize: each vreg maps to itself */
    for (uint32_t i = 0; i < func->next_vreg; i++) {
        map[i] = i;
    }

    /* Find copies: CONST instructions are effectively copies of constants,
     * but we also look for cases where a BINOP with 0 on one side
     * becomes the other side (handled by const fold).
     * The main copy propagation target is PHI nodes with one entry. */

    /* For PHI nodes with a single entry, the phi is just a copy */
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            if (inst->op == OP_PHI && inst->phi.n == 1) {
                /* Single-entry PHI is just a copy */
                if (inst->dst < func->next_vreg) {
                    map[inst->dst] = inst->phi.srcs[0];
                }
            }
        }
    }

    /* Transitively resolve chains */
    for (uint32_t i = 0; i < func->next_vreg; i++) {
        uint32_t current = i;
        int depth = 0;
        while (map[current] != current && depth < 100) {
            current = map[current];
            depth++;
        }
        map[i] = current;
    }

    return map;
}

/* Replace all uses of old_vreg with new_vreg in an instruction */
static void replace_uses(SsaInst *inst, uint32_t *map) {
    switch (inst->op) {
    case OP_BINOP:
        inst->binop.lhs = map[inst->binop.lhs];
        inst->binop.rhs = map[inst->binop.rhs];
        break;
    case OP_UNOP:
        inst->unop.operand = map[inst->unop.operand];
        break;
    case OP_LOAD:
        inst->load.ptr = map[inst->load.ptr];
        break;
    case OP_STORE:
        inst->store.ptr = map[inst->store.ptr];
        inst->store.val = map[inst->store.val];
        break;
    case OP_GEP:
        inst->gep.ptr = map[inst->gep.ptr];
        break;
    case OP_CMP:
        inst->cmp.lhs = map[inst->cmp.lhs];
        inst->cmp.rhs = map[inst->cmp.rhs];
        break;
    case OP_BRANCH:
        inst->branch.cond = map[inst->branch.cond];
        break;
    case OP_CALL:
        for (uint32_t i = 0; i < inst->call.nargs; i++) {
            inst->call.args[i] = map[inst->call.args[i]];
        }
        break;
    case OP_RET:
        if (inst->ret.val != VT_INVALID_VREG)
            inst->ret.val = map[inst->ret.val];
        break;
    case OP_PHI:
        for (uint32_t i = 0; i < inst->phi.n; i++) {
            inst->phi.srcs[i] = map[inst->phi.srcs[i]];
        }
        break;
    case OP_PRINT:
        inst->print.val = map[inst->print.val];
        break;
    default:
        break;
    }
}

void opt_copy_prop(SsaFunc *func) {
    if (func->next_vreg == 0) return;

    uint32_t *map = build_copy_map(func);

    /* Check if any copies exist */
    bool has_copies = false;
    for (uint32_t i = 0; i < func->next_vreg; i++) {
        if (map[i] != i) {
            has_copies = true;
            break;
        }
    }

    if (!has_copies) {
        free(map);
        return;
    }

    /* Replace all uses */
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            replace_uses(&blk->insts[ii], map);
        }
    }

    /* Remove trivially dead PHI nodes (single entry) */
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            if (inst->op == OP_PHI && inst->phi.n == 1) {
                /* This PHI is now dead - DCE will remove it */
            }
        }
    }

    free(map);
}
