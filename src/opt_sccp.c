/*
 * VORTECH Compiler - Sparse Conditional Constant Propagation
 *
 * One of the best cost/benefit optimizations ever invented.
 * Memory: Low. Complexity: Moderate. Performance: Good. Probably worth adding.
 */
#include "opt.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lattice: TOP (unknown), CONST (known constant), BOTTOM (variable) */
typedef enum {
    LATTICE_TOP,
    LATTICE_CONST,
    LATTICE_BOTTOM,
} LatticeVal;

typedef struct {
    LatticeVal state;
    int64_t    value;
} ConstLattice;

void opt_sccp(SsaFunc *func) {
    if (func->next_vreg == 0) return;

    ConstLattice *lattice = malloc(func->next_vreg * sizeof(ConstLattice));
    if (!lattice) {
        diag_msg(DIAG_FATAL, "out of memory in SCCP");
    }

    /* Initialize all to TOP */
    for (uint32_t i = 0; i < func->next_vreg; i++) {
        lattice[i].state = LATTICE_TOP;
        lattice[i].value = 0;
    }

    /* Parameters are BOTTOM (unknown at compile time) */
    for (uint32_t i = 0; i < func->nparams; i++) {
        lattice[func->param_vregs[i]].state = LATTICE_BOTTOM;
    }

    /* Worklist: which blocks to visit */
    bool *block_in_worklist = calloc(func->nblocks, sizeof(bool));
    uint32_t *worklist = malloc(func->nblocks * sizeof(uint32_t));
    uint32_t worklist_len = 0;
    if (!block_in_worklist || !worklist) {
        diag_msg(DIAG_FATAL, "out of memory in SCCP");
    }

    /* Start with entry block */
    worklist[worklist_len++] = func->entry;
    block_in_worklist[func->entry] = true;

    /* Iterate until fixpoint */
    bool changed = true;
    int max_iters = 1000;

    while (changed && max_iters-- > 0) {
        changed = false;

        for (uint32_t bi = 0; bi < func->nblocks; bi++) {
            SsaBlock *blk = &func->blocks[bi];
            for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
                SsaInst *inst = &blk->insts[ii];
                ConstLattice old;

                switch (inst->op) {
                case OP_CONST: {
                    if (inst->dst == VT_INVALID_VREG) break;
                    old = lattice[inst->dst];
                    if (lattice[inst->dst].state != LATTICE_CONST ||
                        lattice[inst->dst].value != inst->const_.value) {
                        lattice[inst->dst].state = LATTICE_CONST;
                        lattice[inst->dst].value = inst->const_.value;
                        if (old.state != lattice[inst->dst].state ||
                            old.value != lattice[inst->dst].value) {
                            changed = true;
                        }
                    }
                    break;
                }
                case OP_BINOP: {
                    if (inst->dst == VT_INVALID_VREG) break;
                    old = lattice[inst->dst];
                    ConstLattice *lhs = &lattice[inst->binop.lhs];
                    ConstLattice *rhs = &lattice[inst->binop.rhs];

                    if (lhs->state == LATTICE_BOTTOM || rhs->state == LATTICE_BOTTOM) {
                        if (lattice[inst->dst].state != LATTICE_BOTTOM) {
                            lattice[inst->dst].state = LATTICE_BOTTOM;
                            changed = true;
                        }
                    } else if (lhs->state == LATTICE_CONST && rhs->state == LATTICE_CONST) {
                        /* Both constant - compute result */
                        int64_t result = 0;
                        bool div_zero = false;
                        switch (inst->binop.kind) {
                        case BINOP_ADD:    result = lhs->value + rhs->value; break;
                        case BINOP_SUB:    result = lhs->value - rhs->value; break;
                        case BINOP_MUL:    result = lhs->value * rhs->value; break;
                        case BINOP_DIV:    result = rhs->value != 0 ? lhs->value / rhs->value : 0; div_zero = (rhs->value == 0); break;
                        case BINOP_MOD:    result = rhs->value != 0 ? lhs->value % rhs->value : 0; break;
                        case BINOP_AND:    result = lhs->value & rhs->value; break;
                        case BINOP_OR:     result = lhs->value | rhs->value; break;
                        case BINOP_XOR:    result = lhs->value ^ rhs->value; break;
                        case BINOP_LSHIFT: result = lhs->value << rhs->value; break;
                        case BINOP_RSHIFT: result = lhs->value >> rhs->value; break;
                        }
                        if (!div_zero) {
                            if (lattice[inst->dst].state != LATTICE_CONST ||
                                lattice[inst->dst].value != result) {
                                lattice[inst->dst].state = LATTICE_CONST;
                                lattice[inst->dst].value = result;
                                changed = true;
                            }
                        }
                    }
                    /* If either is TOP, result stays TOP */
                    break;
                }
                case OP_CMP: {
                    if (inst->dst == VT_INVALID_VREG) break;
                    old = lattice[inst->dst];
                    ConstLattice *lhs = &lattice[inst->cmp.lhs];
                    ConstLattice *rhs = &lattice[inst->cmp.rhs];

                    if (lhs->state == LATTICE_BOTTOM || rhs->state == LATTICE_BOTTOM) {
                        if (lattice[inst->dst].state != LATTICE_BOTTOM) {
                            lattice[inst->dst].state = LATTICE_BOTTOM;
                            changed = true;
                        }
                    } else if (lhs->state == LATTICE_CONST && rhs->state == LATTICE_CONST) {
                        bool result = false;
                        switch (inst->cmp.kind) {
                        case CMP_EQ:  result = lhs->value == rhs->value; break;
                        case CMP_NEQ: result = lhs->value != rhs->value; break;
                        case CMP_LT:  result = lhs->value < rhs->value; break;
                        case CMP_GT:  result = lhs->value > rhs->value; break;
                        case CMP_LEQ: result = lhs->value <= rhs->value; break;
                        case CMP_GEQ: result = lhs->value >= rhs->value; break;
                        }
                        if (lattice[inst->dst].state != LATTICE_CONST ||
                            lattice[inst->dst].value != (result ? 1 : 0)) {
                            lattice[inst->dst].state = LATTICE_CONST;
                            lattice[inst->dst].value = result ? 1 : 0;
                            changed = true;
                        }
                    }
                    break;
                }
                case OP_PHI: {
                    if (inst->dst == VT_INVALID_VREG) break;
                    old = lattice[inst->dst];

                    /* Meet all incoming values */
                    ConstLattice result;
                    result.state = LATTICE_TOP;
                    result.value = 0;

                    for (uint32_t i = 0; i < inst->phi.n; i++) {
                        ConstLattice *src = &lattice[inst->phi.srcs[i]];
                        if (src->state == LATTICE_BOTTOM) {
                            result.state = LATTICE_BOTTOM;
                        } else if (src->state == LATTICE_CONST) {
                            if (result.state == LATTICE_TOP) {
                                result.state = LATTICE_CONST;
                                result.value = src->value;
                            } else if (result.state == LATTICE_CONST) {
                                if (result.value != src->value) {
                                    result.state = LATTICE_BOTTOM;
                                }
                            }
                        }
                        /* TOP doesn't affect the meet */
                    }

                    if (old.state != result.state || old.value != result.value) {
                        lattice[inst->dst] = result;
                        changed = true;
                    }
                    break;
                }
                default:
                    /* For other instructions, if they produce a result, mark as BOTTOM */
                    if (inst->dst != VT_INVALID_VREG && lattice[inst->dst].state == LATTICE_TOP) {
                        lattice[inst->dst].state = LATTICE_BOTTOM;
                        changed = true;
                    }
                    break;
                }
            }
        }
    }

    /* Now replace constant vregs with actual constants and remove code that's proven dead */
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];

            /* Replace known-constant values with CONST instructions */
            if (inst->dst != VT_INVALID_VREG && lattice[inst->dst].state == LATTICE_CONST) {
                if (inst->op != OP_CONST) {
                    inst->op = OP_CONST;
                    inst->const_.value = lattice[inst->dst].value;
                }
            }

            /* For branches with constant conditions, convert to jumps */
            if (inst->op == OP_BRANCH && lattice[inst->branch.cond].state == LATTICE_CONST) {
                uint32_t target = lattice[inst->branch.cond].value
                                  ? inst->branch.true_bb
                                  : inst->branch.false_bb;
                inst->op = OP_JUMP;
                inst->jump.target_bb = target;
            }
        }
    }

    free(lattice);
    free(block_in_worklist);
    free(worklist);
}
