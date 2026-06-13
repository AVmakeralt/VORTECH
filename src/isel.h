/*
 * VORTECH Compiler - Greedy Instruction Selection
 *
 * Greedy selector. Plus pattern matching. Not giant DAG systems.
 * Table-driven.
 * MUL x, 8 -> shl x, 3 through patterns.
 */
#ifndef VORTECH_ISEL_H
#define VORTECH_ISEL_H

#include "common.h"
#include "arena.h"
#include "ssa.h"

/* Machine instruction (post instruction selection, pre register allocation) */
typedef struct {
    MachOpcode  op;
    VtType     *type;       /* for sizing decisions */

    /* Operands - using vreg indices before regalloc, register indices after */
    uint32_t    dst;        /* destination vreg or register */
    uint32_t    src;        /* source vreg or register */
    uint32_t    src2;       /* second source (for 3-operand instructions) */

    /* For immediates */
    int64_t     imm;

    /* For calls */
    const char *func_name;
    uint32_t   *arg_vregs;
    uint32_t    nargs;

    /* For branches/jumps */
    uint32_t    target_bb;
    const char *label;      /* resolved label name */

    /* For GEP/memory */
    uint32_t    base;       /* base register for LEA */
    int64_t     offset;

    /* Flags */
    bool        is_vreg;    /* operands are vregs (before regalloc) */
} MachInst;

/* A machine basic block */
typedef struct {
    uint32_t    label;
    MachInst   *insts;
    uint32_t    ninsts;
    uint32_t    insts_cap;
} MachBlock;

/* A machine function (output of instruction selection) */
typedef struct {
    const char *name;
    MachBlock  *blocks;
    uint32_t    nblocks;
    uint32_t    next_vreg;  /* total vregs used */
    uint32_t    nparams;
    uint32_t   *param_vregs;
    VtType    **param_types;
    VtType     *ret_type;
    uint32_t    stack_size;
} MachFunc;

/* Perform instruction selection on an SSA function */
MachFunc *isel_select(Arena *arena, SsaFunc *ssa_func);

/* Debug print */
void mach_func_print(MachFunc *mf);

#endif /* VORTECH_ISEL_H */
