/*
 * VORTECH Compiler - Linear Scan Register Allocation
 *
 * Fast. Tiny memory use. Predictable.
 * Potentially + Interval Splitting + Rematerialization
 * which gets surprisingly close to graph-coloring quality.
 */
#ifndef VORTECH_REGALLOC_H
#define VORTECH_REGALLOC_H

#include "common.h"
#include "arena.h"
#include "isel.h"

/* Live interval for a virtual register */
typedef struct {
    uint32_t vreg;        /* virtual register number */
    uint32_t start;       /* first instruction index where vreg is live */
    uint32_t end;         /* last instruction index where vreg is live */
    X86Reg   phys_reg;    /* assigned physical register (REG_NONE if unassigned) */
    int32_t  stack_slot;  /* stack slot if spilled (-1 if not spilled) */
    bool     is_param;    /* is this a function parameter? */
    bool     is_spilled;
} LiveInterval;

/* Register allocation result */
typedef struct {
    X86Reg        *vreg_to_reg;     /* vreg -> physical register mapping */
    int32_t       *vreg_to_slot;    /* vreg -> stack slot (for spills) */
    uint32_t       num_vregs;
    uint32_t       stack_slots;     /* total stack slots needed */
    uint32_t       stack_size;      /* total stack frame size in bytes */
    bool          *reg_used;        /* which physical regs are in use */
} RegAllocResult;

/* Perform linear scan register allocation */
RegAllocResult *regalloc_linear_scan(MachFunc *mf, Arena *arena);

/* Apply register allocation to a machine function, rewriting vregs to phys regs */
void regalloc_apply(MachFunc *mf, RegAllocResult *ra);

/* Free register allocation result */
void regalloc_free(RegAllocResult *ra);

#endif /* VORTECH_REGALLOC_H */
