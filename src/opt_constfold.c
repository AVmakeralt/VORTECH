/*
 * VORTECH Compiler - Constant Folding
 *
 * Tiny complexity. Huge value.
 * 5 * 8 -> 40
 */
#include "opt.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

/* Check if a vreg is defined by a CONST instruction with the given value */
static bool is_const_val(SsaFunc *func, uint32_t vreg, int64_t *out_val) {
    if (vreg == VT_INVALID_VREG) return false;

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            if (inst->op == OP_CONST && inst->dst == vreg) {
                if (out_val) *out_val = inst->const_.value;
                return true;
            }
        }
    }
    return false;
}

static int64_t fold_binop(BinOpKind kind, int64_t lhs, int64_t rhs) {
    switch (kind) {
    case BINOP_ADD:    return lhs + rhs;
    case BINOP_SUB:    return lhs - rhs;
    case BINOP_MUL:    return lhs * rhs;
    case BINOP_DIV:    return rhs != 0 ? lhs / rhs : 0;
    case BINOP_MOD:    return rhs != 0 ? lhs % rhs : 0;
    case BINOP_AND:    return lhs & rhs;
    case BINOP_OR:     return lhs | rhs;
    case BINOP_XOR:    return lhs ^ rhs;
    case BINOP_LSHIFT: return lhs << rhs;
    case BINOP_RSHIFT: return lhs >> rhs;
    }
    return 0;
}

static bool fold_cmp(CmpKind kind, int64_t lhs, int64_t rhs) {
    switch (kind) {
    case CMP_EQ:  return lhs == rhs;
    case CMP_NEQ: return lhs != rhs;
    case CMP_LT:  return lhs < rhs;
    case CMP_GT:  return lhs > rhs;
    case CMP_LEQ: return lhs <= rhs;
    case CMP_GEQ: return lhs >= rhs;
    }
    return false;
}

void opt_const_fold(SsaFunc *func) {
    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 10;

    while (changed && iterations < MAX_ITERATIONS) {
        changed = false;
        iterations++;

        for (uint32_t bi = 0; bi < func->nblocks; bi++) {
            SsaBlock *blk = &func->blocks[bi];
            for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
                SsaInst *inst = &blk->insts[ii];

                switch (inst->op) {
                case OP_BINOP: {
                    int64_t lhs_val, rhs_val;
                    if (is_const_val(func, inst->binop.lhs, &lhs_val) &&
                        is_const_val(func, inst->binop.rhs, &rhs_val)) {
                        int64_t result = fold_binop(inst->binop.kind, lhs_val, rhs_val);
                        /* Replace with constant */
                        inst->op = OP_CONST;
                        inst->const_.value = result;
                        changed = true;
                    }
                    /* Identity operations */
                    else if (is_const_val(func, inst->binop.rhs, &rhs_val)) {
                        if (rhs_val == 0 && (inst->binop.kind == BINOP_ADD ||
                                             inst->binop.kind == BINOP_SUB)) {
                            /* x + 0 = x, x - 0 = x */
                            inst->op = OP_CONST; /* will be cleaned up by copy prop */
                            inst->const_.value = 0; /* placeholder - copy prop handles this */
                        } else if (rhs_val == 1 && inst->binop.kind == BINOP_MUL) {
                            /* x * 1 = x */
                            inst->op = OP_CONST;
                            inst->const_.value = 1;
                        }
                    }
                    break;
                }
                case OP_CMP: {
                    int64_t lhs_val, rhs_val;
                    if (is_const_val(func, inst->cmp.lhs, &lhs_val) &&
                        is_const_val(func, inst->cmp.rhs, &rhs_val)) {
                        bool result = fold_cmp(inst->cmp.kind, lhs_val, rhs_val);
                        inst->op = OP_CONST;
                        inst->const_.value = result ? 1 : 0;
                        changed = true;
                    }
                    break;
                }
                case OP_UNOP: {
                    int64_t operand_val;
                    if (is_const_val(func, inst->unop.operand, &operand_val)) {
                        int64_t result;
                        switch (inst->unop.kind) {
                        case UNOP_NEG:  result = -operand_val; break;
                        case UNOP_NOT:  result = operand_val ? 0 : 1; break;
                        case UNOP_BNOT: result = ~operand_val; break;
                        default: result = 0; break;
                        }
                        inst->op = OP_CONST;
                        inst->const_.value = result;
                        changed = true;
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
}
