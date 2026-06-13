/*
 * VORTECH Compiler - Peephole Optimizer
 *
 * Window-based. Not global graphs.
 * Window size: 4-8 instructions. Tiny memory footprint.
 */
#include "isel.h"
#include "regalloc.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void peephole_run(MachFunc *mf) {
    bool changed = true;
    int max_iterations = 5;

    while (changed && max_iterations-- > 0) {
        changed = false;

        for (uint32_t bi = 0; bi < mf->nblocks; bi++) {
            MachBlock *mb = &mf->blocks[bi];

            for (uint32_t ii = 0; ii + 1 < mb->ninsts; ii++) {
                MachInst *a = &mb->insts[ii];
                MachInst *b = &mb->insts[ii + 1];

                /* Pattern: mov reg, reg; mov reg, reg (redundant copy) */
                if (a->op == MINST_MOV && b->op == MINST_MOV &&
                    a->dst == b->src && a->src == b->dst && a->dst != 0 && b->dst != 0) {
                    /* Second MOV is redundant */
                    b->op = MINST_NOP;
                    changed = true;
                }

                /* Pattern: add reg, 0 -> nop */
                if (a->op == MINST_ADD_IMM && a->imm == 0) {
                    a->op = MINST_NOP;
                    changed = true;
                }

                /* Pattern: sub reg, 0 -> nop */
                if (a->op == MINST_SUB_IMM && a->imm == 0) {
                    a->op = MINST_NOP;
                    changed = true;
                }

                /* Pattern: mov reg, 0 -> xor reg, reg (shorter encoding) */
                if (a->op == MINST_MOV_IMM && a->imm == 0 && a->dst != 0) {
                    a->op = MINST_XOR;
                    a->src = a->dst;
                    a->src2 = a->dst;
                    a->imm = 0;
                    changed = true;
                }

                /* Pattern: imul reg, 1 -> nop (value already in reg) */
                if (a->op == MINST_MUL_IMM && a->imm == 1) {
                    a->op = MINST_NOP;
                    changed = true;
                }

                /* Pattern: imul reg, 2 -> add reg, reg (or shl reg, 1) */
                if (a->op == MINST_MUL_IMM && a->imm == 2 && a->dst != 0) {
                    a->op = MINST_ADD;
                    a->src = a->dst;
                    a->src2 = a->dst;
                    a->imm = 0;
                    changed = true;
                }

                /* Pattern: test reg, reg; sete reg -> alternate pattern */
                /* (handled by instruction selection already) */
            }

            /* Remove NOPs */
            uint32_t write = 0;
            for (uint32_t ii = 0; ii < mb->ninsts; ii++) {
                if (mb->insts[ii].op != MINST_NOP) {
                    if (write != ii) {
                        mb->insts[write] = mb->insts[ii];
                    }
                    write++;
                } else {
                    changed = true;
                }
            }
            mb->ninsts = write;
        }
    }
}
