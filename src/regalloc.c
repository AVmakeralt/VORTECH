/*
 * VORTECH Compiler - Linear Scan Register Allocation Implementation
 */
#include "regalloc.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compute live intervals for all vregs in a function */
static LiveInterval *compute_live_intervals(MachFunc *mf, uint32_t *out_count) {
    LiveInterval *intervals = calloc(mf->next_vreg, sizeof(LiveInterval));
    if (!intervals) { diag_msg(DIAG_FATAL, "out of memory in regalloc"); }

    /* Initialize: start = MAX, end = 0 (empty interval) */
    for (uint32_t i = 0; i < mf->next_vreg; i++) {
        intervals[i].vreg = i;
        intervals[i].start = UINT32_MAX;
        intervals[i].end = 0;
        intervals[i].phys_reg = REG_NONE;
        intervals[i].stack_slot = -1;
        intervals[i].is_param = false;
        intervals[i].is_spilled = false;
    }

    /* Mark parameters */
    for (uint32_t i = 0; i < mf->nparams; i++) {
        if (mf->param_vregs[i] < mf->next_vreg) {
            intervals[mf->param_vregs[i]].is_param = true;
            intervals[mf->param_vregs[i]].start = 0;
        }
    }

    /* Walk all instructions and record vreg uses */
    uint32_t global_idx = 0;
    for (uint32_t bi = 0; bi < mf->nblocks; bi++) {
        MachBlock *mb = &mf->blocks[bi];
        for (uint32_t ii = 0; ii < mb->ninsts; ii++) {
            MachInst *mi = &mb->insts[ii];
            uint32_t idx = global_idx++;

            /* Record definitions (dst) */
            if (mi->dst != VT_INVALID_VREG && mi->op != MINST_LABEL && mi->op != MINST_NOP) {
                if (mi->dst < mf->next_vreg) {
                    if (idx < intervals[mi->dst].start) intervals[mi->dst].start = idx;
                    if (idx > intervals[mi->dst].end) intervals[mi->dst].end = idx;
                }
            }

            /* Record uses (src, src2) */
            if (mi->src != VT_INVALID_VREG && mi->src < mf->next_vreg && mi->op != MINST_LABEL && mi->op != MINST_NOP) {
                if (idx < intervals[mi->src].start) intervals[mi->src].start = idx;
                if (idx > intervals[mi->src].end) intervals[mi->src].end = idx;
            }
            if (mi->src2 != VT_INVALID_VREG && mi->src2 < mf->next_vreg && mi->op != MINST_LABEL && mi->op != MINST_NOP) {
                if (idx < intervals[mi->src2].start) intervals[mi->src2].start = idx;
                if (idx > intervals[mi->src2].end) intervals[mi->src2].end = idx;
            }

            /* Record uses in call arguments */
            if (mi->op == MINST_CALL && mi->arg_vregs) {
                for (uint32_t j = 0; j < mi->nargs; j++) {
                    uint32_t arg = mi->arg_vregs[j];
                    if (arg < mf->next_vreg) {
                        if (idx < intervals[arg].start) intervals[arg].start = idx;
                        if (idx > intervals[arg].end) intervals[arg].end = idx;
                    }
                }
            }
        }
    }

    /* Extend parameter intervals to cover their full usage */
    for (uint32_t i = 0; i < mf->next_vreg; i++) {
        if (intervals[i].is_param && intervals[i].start == UINT32_MAX) {
            intervals[i].start = 0;
        }
    }

    /* Count active intervals (those with start <= end) */
    uint32_t count = 0;
    for (uint32_t i = 0; i < mf->next_vreg; i++) {
        if (intervals[i].start <= intervals[i].end) {
            count++;
        }
    }

    *out_count = count;
    return intervals;
}

/* Compare intervals by start position (for sorting) */
static int cmp_interval_start(const void *a, const void *b) {
    const LiveInterval *ia = (const LiveInterval *)a;
    const LiveInterval *ib = (const LiveInterval *)b;
    if (ia->start != ib->start) return (ia->start < ib->start) ? -1 : 1;
    return (ia->vreg < ib->vreg) ? -1 : 1;
}

RegAllocResult *regalloc_linear_scan(MachFunc *mf, Arena *arena) {
    (void)arena;

    uint32_t num_intervals;
    LiveInterval *intervals = compute_live_intervals(mf, &num_intervals);

    RegAllocResult *ra = calloc(1, sizeof(RegAllocResult));
    if (!ra) { diag_msg(DIAG_FATAL, "out of memory in regalloc"); }

    ra->num_vregs = mf->next_vreg;
    ra->vreg_to_reg = calloc(mf->next_vreg, sizeof(X86Reg));
    ra->vreg_to_slot = calloc(mf->next_vreg, sizeof(int32_t));
    ra->reg_used = calloc(REG_COUNT, sizeof(bool));

    if (!ra->vreg_to_reg || !ra->vreg_to_slot || !ra->reg_used) {
        diag_msg(DIAG_FATAL, "out of memory in regalloc");
    }

    /* Initialize all vregs to no register / no slot */
    for (uint32_t i = 0; i < mf->next_vreg; i++) {
        ra->vreg_to_reg[i] = REG_NONE;
        ra->vreg_to_slot[i] = -1;
    }

    /* Sort intervals by start position */
    /* Build a sorted array of active intervals */
    LiveInterval *sorted = malloc(num_intervals * sizeof(LiveInterval));
    if (!sorted) { diag_msg(DIAG_FATAL, "out of memory in regalloc"); }

    uint32_t si = 0;
    for (uint32_t i = 0; i < mf->next_vreg; i++) {
        if (intervals[i].start <= intervals[i].end) {
            sorted[si++] = intervals[i];
        }
    }
    qsort(sorted, num_intervals, sizeof(LiveInterval), cmp_interval_start);

    /* Linear scan algorithm */
    /* Active list: intervals currently assigned a register */
    LiveInterval **active = malloc(num_intervals * sizeof(LiveInterval *));
    uint32_t active_count = 0;
    if (!active) { diag_msg(DIAG_FATAL, "out of memory in regalloc"); }

    /* Available registers */
    /* Skip RSP and RBP */
    bool reg_available[REG_COUNT];
    memset(reg_available, 0, sizeof(reg_available));
    for (int i = 0; i < (int)(sizeof(vt_alloc_gprs) / sizeof(vt_alloc_gprs[0])); i++) {
        X86Reg r = vt_alloc_gprs[i];
        if (r != REG_RSP && r != REG_RBP) {
            reg_available[r] = true;
        }
    }

    /* Map function parameters to calling convention registers */
    static const X86Reg param_regs[] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9 };
    for (uint32_t i = 0; i < mf->nparams && i < 6; i++) {
        uint32_t pvreg = mf->param_vregs[i];
        if (pvreg < mf->next_vreg) {
            ra->vreg_to_reg[pvreg] = param_regs[i];
            reg_available[param_regs[i]] = false;
            intervals[pvreg].phys_reg = param_regs[i];
        }
    }

    /* Process each interval */
    for (uint32_t i = 0; i < num_intervals; i++) {
        LiveInterval *current = &sorted[i];

        /* Skip parameters already assigned */
        if (current->is_param && current->phys_reg != REG_NONE) {
            active[active_count++] = current;
            continue;
        }

        /* Expire old intervals */
        for (uint32_t j = 0; j < active_count; ) {
            if (active[j]->end < current->start) {
                /* This interval has expired - free its register */
                if (active[j]->phys_reg != REG_NONE) {
                    reg_available[active[j]->phys_reg] = true;
                }
                /* Remove from active list */
                active[j] = active[active_count - 1];
                active_count--;
            } else {
                j++;
            }
        }

        /* Find a free register */
        X86Reg assigned = REG_NONE;
        for (int r = REG_RAX; r <= REG_R15; r++) {
            if (reg_available[r] && r != REG_RSP && r != REG_RBP) {
                assigned = (X86Reg)r;
                break;
            }
        }

        if (assigned != REG_NONE) {
            /* Register available */
            current->phys_reg = assigned;
            reg_available[assigned] = false;
            ra->vreg_to_reg[current->vreg] = assigned;
            active[active_count++] = current;
        } else {
            /* No register available - spill */
            /* Spill the interval with the furthest end, or current if it ends soonest */
            current->is_spilled = true;
            ra->vreg_to_slot[current->vreg] = (int32_t)ra->stack_slots;
            ra->stack_slots++;

            /* Try to spill the interval with the longest end instead */
            uint32_t spill_idx = UINT32_MAX;
            uint32_t furthest_end = current->end;
            for (uint32_t j = 0; j < active_count; j++) {
                if (active[j]->end > furthest_end && !active[j]->is_param) {
                    furthest_end = active[j]->end;
                    spill_idx = j;
                }
            }

            if (spill_idx != UINT32_MAX) {
                /* Evict the active interval and take its register */
                LiveInterval *evicted = active[spill_idx];
                current->phys_reg = evicted->phys_reg;
                ra->vreg_to_reg[current->vreg] = current->phys_reg;
                reg_available[current->phys_reg] = false;

                evicted->phys_reg = REG_NONE;
                evicted->is_spilled = true;
                ra->vreg_to_reg[evicted->vreg] = REG_NONE;
                ra->vreg_to_slot[evicted->vreg] = (int32_t)ra->stack_slots;
                ra->stack_slots++;

                /* Replace evicted with current in active list */
                active[spill_idx] = current;
            }
        }
    }

    /* Calculate stack size (16-byte aligned) */
    ra->stack_size = (ra->stack_slots * 8 + 15) & ~15u;

    /* Add space for callee-saved register saves */
    uint32_t callee_saved_count = 0;
    for (int r = 0; r < REG_COUNT; r++) {
        if (reg_is_callee_saved((X86Reg)r)) {
            for (uint32_t i = 0; i < mf->next_vreg; i++) {
                if (ra->vreg_to_reg[i] == r) {
                    ra->reg_used[r] = true;
                    callee_saved_count++;
                    break;
                }
            }
        }
    }
    ra->stack_size += callee_saved_count * 8;
    ra->stack_size = (ra->stack_size + 15) & ~15u; /* 16-byte align */

    free(sorted);
    free(active);
    free(intervals);

    return ra;
}

void regalloc_apply(MachFunc *mf, RegAllocResult *ra) {
    /* Replace vreg references with physical register references */
    for (uint32_t bi = 0; bi < mf->nblocks; bi++) {
        MachBlock *mb = &mf->blocks[bi];
        for (uint32_t ii = 0; ii < mb->ninsts; ii++) {
            MachInst *mi = &mb->insts[ii];

            if (mi->dst != VT_INVALID_VREG && mi->dst < ra->num_vregs && mi->op != MINST_LABEL && mi->op != MINST_NOP) {
                mi->dst = (uint32_t)ra->vreg_to_reg[mi->dst];
            }
            if (mi->src != VT_INVALID_VREG && mi->src < ra->num_vregs && mi->op != MINST_LABEL && mi->op != MINST_NOP) {
                mi->src = (uint32_t)ra->vreg_to_reg[mi->src];
            }
            if (mi->src2 != VT_INVALID_VREG && mi->src2 < ra->num_vregs) {
                mi->src2 = (uint32_t)ra->vreg_to_reg[mi->src2];
            }

            /* Handle call arguments - DON'T replace arg_vregs.
             * The emitter will look up each vreg's assigned register
             * and emit MOVs to the calling convention registers. */

        }
    }

    mf->stack_size = ra->stack_size;
}

void regalloc_free(RegAllocResult *ra) {
    if (!ra) return;
    free(ra->vreg_to_reg);
    free(ra->vreg_to_slot);
    free(ra->reg_used);
    free(ra);
}
