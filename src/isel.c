/*
 * VORTECH Compiler - Instruction Selection Implementation
 *
 * Greedy, pattern-based, table-driven.
 */
#include "isel.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MachInst mach_inst(MachOpcode op, uint32_t dst, uint32_t src, uint32_t src2,
                           int64_t imm, VtType *type) {
    MachInst mi;
    memset(&mi, 0, sizeof(mi));
    mi.op = op;
    mi.dst = dst;
    mi.src = src;
    mi.src2 = src2;
    mi.imm = imm;
    mi.type = type;
    mi.is_vreg = true;
    return mi;
}

static MachBlock *mach_block_create(uint32_t label) {
    MachBlock *mb = calloc(1, sizeof(MachBlock));
    if (!mb) { fprintf(stderr, "out of memory\n"); exit(1); }
    mb->label = label;
    mb->insts_cap = 16;
    mb->insts = calloc(mb->insts_cap, sizeof(MachInst));
    return mb;
}

static void mach_block_emit(MachBlock *mb, MachInst inst) {
    if (mb->ninsts >= mb->insts_cap) {
        mb->insts_cap *= 2;
        mb->insts = realloc(mb->insts, sizeof(MachInst) * mb->insts_cap);
        if (!mb->insts) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    mb->insts[mb->ninsts++] = inst;
}

/* Check if a value is a power of 2 */
static bool is_power_of_2(int64_t v) {
    return v > 0 && (v & (v - 1)) == 0;
}

/* Get log2 of a power of 2 */
static int log2_of(int64_t v) {
    int s = 0;
    while (v > 1) { v >>= 1; s++; }
    return s;
}

MachFunc *isel_select(Arena *arena, SsaFunc *ssa_func) {
    (void)arena;

    MachFunc *mf = calloc(1, sizeof(MachFunc));
    if (!mf) { fprintf(stderr, "out of memory\n"); exit(1); }

    mf->name = ssa_func->name;
    mf->nblocks = ssa_func->nblocks;
    mf->next_vreg = ssa_func->next_vreg;
    mf->nparams = ssa_func->nparams;
    mf->param_vregs = ssa_func->param_vregs;
    mf->param_types = ssa_func->param_types;
    mf->ret_type = ssa_func->ret_type;

    mf->blocks = calloc(mf->nblocks, sizeof(MachBlock));
    if (!mf->blocks) { fprintf(stderr, "out of memory\n"); exit(1); }

    for (uint32_t bi = 0; bi < ssa_func->nblocks; bi++) {
        SsaBlock *sblk = &ssa_func->blocks[bi];
        mf->blocks[bi].label = sblk->label;
        mf->blocks[bi].insts_cap = sblk->ninsts * 2 + 8;
        mf->blocks[bi].insts = calloc(mf->blocks[bi].insts_cap, sizeof(MachInst));
        mf->blocks[bi].ninsts = 0;

        /* Emit block label */
        mach_block_emit(&mf->blocks[bi], mach_inst(MINST_LABEL, 0, 0, 0, 0, NULL));
        mf->blocks[bi].insts[0].label = NULL; /* label is the block index */

        for (uint32_t ii = 0; ii < sblk->ninsts; ii++) {
            SsaInst *inst = &sblk->insts[ii];

            switch (inst->op) {
            case OP_CONST: {
                if (inst->dst == VT_INVALID_VREG) break;

                /* Pattern: small constants can use MOV_IMM */
                if (inst->const_.value >= -2147483648LL && inst->const_.value <= 2147483647LL) {
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_MOV_IMM, inst->dst, 0, 0,
                                  inst->const_.value, inst->dst_type));
                } else {
                    /* Large constant: movabs */
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_MOV_IMM, inst->dst, 0, 0,
                                  inst->const_.value, inst->dst_type));
                }
                break;
            }
            case OP_BINOP: {
                switch (inst->binop.kind) {
                case BINOP_ADD:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_ADD, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_SUB:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_SUB, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_MUL: {
                    /* Pattern: MUL by power of 2 -> SHL */
                    /* Check if rhs is a known constant (from const folding) */
                    /* For now, just emit MUL */
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_MUL, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                }
                case BINOP_DIV:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_DIV, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_MOD:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_DIV, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_AND:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_AND, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_OR:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_OR, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_XOR:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_XOR, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_LSHIFT:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_SHL, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                case BINOP_RSHIFT:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_SHR, inst->dst, inst->binop.lhs, inst->binop.rhs,
                                  0, inst->dst_type));
                    break;
                }
                break;
            }
            case OP_UNOP: {
                switch (inst->unop.kind) {
                case UNOP_NEG:
                    /* neg dst, operand */
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_NEG, inst->dst, inst->unop.operand, 0, 0,
                                  inst->dst_type));
                    break;
                case UNOP_NOT:
                    /* test operand, operand; sete dst */
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_TEST, 0, inst->unop.operand, inst->unop.operand,
                                  0, inst->dst_type));
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_SET_EQ, inst->dst, 0, 0, 0, inst->dst_type));
                    break;
                case UNOP_BNOT:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_NOT, inst->dst, inst->unop.operand, 0, 0,
                                  inst->dst_type));
                    break;
                }
                break;
            }
            case OP_CMP: {
                mach_block_emit(&mf->blocks[bi],
                    mach_inst(MINST_CMP, 0, inst->cmp.lhs, inst->cmp.rhs, 0,
                              inst->dst_type));
                /* Set result based on comparison kind */
                switch (inst->cmp.kind) {
                case CMP_EQ:  mach_block_emit(&mf->blocks[bi], mach_inst(MINST_SET_EQ,  inst->dst, 0, 0, 0, inst->dst_type)); break;
                case CMP_NEQ: mach_block_emit(&mf->blocks[bi], mach_inst(MINST_SET_NEQ, inst->dst, 0, 0, 0, inst->dst_type)); break;
                case CMP_LT:  mach_block_emit(&mf->blocks[bi], mach_inst(MINST_SET_LT,  inst->dst, 0, 0, 0, inst->dst_type)); break;
                case CMP_GT:  mach_block_emit(&mf->blocks[bi], mach_inst(MINST_SET_GT,  inst->dst, 0, 0, 0, inst->dst_type)); break;
                case CMP_LEQ: mach_block_emit(&mf->blocks[bi], mach_inst(MINST_SET_LEQ, inst->dst, 0, 0, 0, inst->dst_type)); break;
                case CMP_GEQ: mach_block_emit(&mf->blocks[bi], mach_inst(MINST_SET_GEQ, inst->dst, 0, 0, 0, inst->dst_type)); break;
                }
                break;
            }
            case OP_BRANCH: {
                /* Compare condition against 0 and branch */
                switch (inst->branch.cond) {
                /* We need to emit: test cond, cond; jnz true_bb; jmp false_bb */
                default:
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_TEST, 0, inst->branch.cond, inst->branch.cond,
                                  0, NULL));
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_JNE, 0, 0, 0, 0, NULL));
                    mf->blocks[bi].insts[mf->blocks[bi].ninsts - 1].target_bb = inst->branch.true_bb;
                    mach_block_emit(&mf->blocks[bi],
                        mach_inst(MINST_JMP, 0, 0, 0, 0, NULL));
                    mf->blocks[bi].insts[mf->blocks[bi].ninsts - 1].target_bb = inst->branch.false_bb;
                    break;
                }
                break;
            }
            case OP_JUMP: {
                mach_block_emit(&mf->blocks[bi],
                    mach_inst(MINST_JMP, 0, 0, 0, 0, NULL));
                mf->blocks[bi].insts[mf->blocks[bi].ninsts - 1].target_bb = inst->jump.target_bb;
                break;
            }
            case OP_CALL: {
                MachInst mi = mach_inst(MINST_CALL, inst->dst, 0, 0, 0, inst->dst_type);
                mi.func_name = inst->call.func_name;
                mi.arg_vregs = inst->call.args;
                mi.nargs = inst->call.nargs;
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_RET: {
                MachInst mi = mach_inst(MINST_RET, 0, inst->ret.val, 0, 0, NULL);
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_LOAD: {
                /* mov dst, [src] */
                MachInst mi = mach_inst(MINST_MOV, inst->dst, inst->load.ptr, 0, 0,
                                         inst->dst_type);
                mi.offset = 0; /* load from [ptr + 0] */
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_STORE: {
                /* mov [dst], src */
                MachInst mi = mach_inst(MINST_MOV, inst->store.ptr, inst->store.val, 0, 0,
                                         NULL);
                mi.offset = 0;
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_GEP: {
                /* lea dst, [base + offset] */
                MachInst mi = mach_inst(MINST_LEA, inst->dst, inst->gep.ptr, 0, 0,
                                         inst->dst_type);
                mi.offset = inst->gep.offset;
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_ALLOC: {
                /* Stack allocation: sub rsp, bytes */
                MachInst mi = mach_inst(MINST_SUB_IMM, 0, 0, 0, (int64_t)inst->alloc.bytes, NULL);
                /* dst = the stack pointer (will be handled in regalloc) */
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_ARENA_ALLOC: {
                /* Call runtime arena allocator */
                MachInst mi = mach_inst(MINST_CALL, inst->dst, 0, 0, 0, inst->dst_type);
                mi.func_name = "__vortech_arena_alloc";
                mi.nargs = 2;
                mi.arg_vregs = calloc(2, sizeof(uint32_t));
                /* We'd need the arena and size as vregs - simplified for now */
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_ARENA_RESET: {
                MachInst mi = mach_inst(MINST_CALL, 0, 0, 0, 0, NULL);
                mi.func_name = "__vortech_arena_reset";
                mi.nargs = 1;
                mi.arg_vregs = calloc(1, sizeof(uint32_t));
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_PRINT: {
                /* Call printf or write syscall */
                MachInst mi = mach_inst(MINST_CALL, 0, 0, 0, 0, NULL);
                mi.func_name = "__vortech_print_i64";
                mi.nargs = 1;
                mi.arg_vregs = calloc(1, sizeof(uint32_t));
                mi.arg_vregs[0] = inst->print.val;
                mach_block_emit(&mf->blocks[bi], mi);
                break;
            }
            case OP_PHI: {
                /* PHI elimination will be done in a separate pass after all
                 * blocks are built, so we can correctly insert copies at the
                 * end of predecessor blocks (before their terminators). */
                break;
            }
            }
        }
    }

    /* ---- PHI Elimination Pass ---- */
    /* Now that all blocks are built, insert MOV copies for PHI nodes.
     * For each PHI in block B: dst = phi(pred0:src0, pred1:src1, ...),
     * insert "mov dst, srcN" at the end of each predecessor predN,
     * just before the terminator instruction. */
    for (uint32_t bi = 0; bi < ssa_func->nblocks; bi++) {
        SsaBlock *sblk = &ssa_func->blocks[bi];
        for (uint32_t ii = 0; ii < sblk->ninsts; ii++) {
            SsaInst *inst = &sblk->insts[ii];
            if (inst->op != OP_PHI) continue;

            for (uint32_t pi = 0; pi < inst->phi.n; pi++) {
                uint32_t pred_bb = inst->phi.preds[pi];
                if (pred_bb >= mf->nblocks) continue;

                MachInst mi = mach_inst(MINST_MOV, inst->dst, inst->phi.srcs[pi], 0, 0,
                                         inst->dst_type ? inst->dst_type : vt_type_make(arena, VTTYPE_I64));
                MachBlock *pblk = &mf->blocks[pred_bb];

                /* Insert before the terminator (last instruction) */
                if (pblk->ninsts > 0) {
                    MachInst *last = &pblk->insts[pblk->ninsts - 1];
                    if (last->op == MINST_JMP || last->op == MINST_JE ||
                        last->op == MINST_JNE || last->op == MINST_RET) {
                        /* Ensure capacity */
                        if (pblk->ninsts >= pblk->insts_cap) {
                            pblk->insts_cap = pblk->ninsts * 2 + 4;
                            pblk->insts = realloc(pblk->insts, sizeof(MachInst) * pblk->insts_cap);
                            if (!pblk->insts) { fprintf(stderr, "out of memory\n"); exit(1); }
                        }
                        /* Shift terminator right by 1 */
                        pblk->insts[pblk->ninsts] = pblk->insts[pblk->ninsts - 1];
                        /* Insert copy before terminator */
                        pblk->insts[pblk->ninsts - 1] = mi;
                        pblk->ninsts++;
                    } else {
                        mach_block_emit(pblk, mi);
                    }
                } else {
                    mach_block_emit(pblk, mi);
                }
            }
        }
    }

    return mf;
}

/* Debug print */
static const char *mach_op_str(MachOpcode op) {
    switch (op) {
    case MINST_MOV:      return "mov";
    case MINST_MOV_IMM:  return "mov";
    case MINST_ADD:      return "add";
    case MINST_ADD_IMM:  return "add";
    case MINST_SUB:      return "sub";
    case MINST_SUB_IMM:  return "sub";
    case MINST_MUL:      return "imul";
    case MINST_MUL_IMM:  return "imul";
    case MINST_DIV:      return "idiv";
    case MINST_AND:      return "and";
    case MINST_OR:       return "or";
    case MINST_XOR:      return "xor";
    case MINST_SHL:      return "shl";
    case MINST_SHR:      return "shr";
    case MINST_SAR:      return "sar";
    case MINST_NEG:      return "neg";
    case MINST_NOT:      return "not";
    case MINST_CMP:      return "cmp";
    case MINST_CMP_IMM:  return "cmp";
    case MINST_TEST:     return "test";
    case MINST_JMP:      return "jmp";
    case MINST_JE:       return "je";
    case MINST_JNE:      return "jne";
    case MINST_CALL:     return "call";
    case MINST_RET:      return "ret";
    case MINST_PUSH:     return "push";
    case MINST_POP:      return "pop";
    case MINST_LEA:      return "lea";
    case MINST_LABEL:    return "label";
    default:             return "?";
    }
}

void mach_func_print(MachFunc *mf) {
    printf("# Machine function: %s\n", mf->name);
    printf("# vregs: %u, params: %u\n", mf->next_vreg, mf->nparams);

    for (uint32_t bi = 0; bi < mf->nblocks; bi++) {
        MachBlock *mb = &mf->blocks[bi];
        printf(".L%u:\n", mb->label);
        for (uint32_t ii = 0; ii < mb->ninsts; ii++) {
            MachInst *mi = &mb->insts[ii];
            if (mi->op == MINST_LABEL) continue;
            printf("  %s", mach_op_str(mi->op));
            if (mi->op == MINST_CALL) {
                printf(" %s", mi->func_name ? mi->func_name : "?");
            }
            if (mi->dst) printf(" v%u", mi->dst);
            if (mi->src) printf(", v%u", mi->src);
            if (mi->src2) printf(", v%u", mi->src2);
            if (mi->op == MINST_MOV_IMM || mi->op == MINST_ADD_IMM ||
                mi->op == MINST_SUB_IMM || mi->op == MINST_CMP_IMM) {
                printf(", %lld", (long long)mi->imm);
            }
            if (mi->op == MINST_JMP || mi->op == MINST_JE || mi->op == MINST_JNE) {
                printf(" .L%u", mi->target_bb);
            }
            printf("\n");
        }
    }
}
