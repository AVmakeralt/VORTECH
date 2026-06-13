/*
 * VORTECH Compiler - Optimization Pass Interface
 *
 * Every optimization must beat:
 *   Performance Gain / Memory Cost
 * by some threshold. Not Performance Gain alone.
 */
#ifndef VORTECH_OPT_H
#define VORTECH_OPT_H

#include "common.h"
#include "arena.h"
#include "ssa.h"

/* Run all optimization passes on a function */
void opt_run_all(SsaFunc *func, Arena *arena);

/* Individual passes */
void opt_const_fold(SsaFunc *func);
void opt_dce(SsaFunc *func);
void opt_copy_prop(SsaFunc *func);
void opt_cse(SsaFunc *func);
void opt_sccp(SsaFunc *func);
void opt_licm(SsaFunc *func);
void opt_unroll(SsaFunc *func, uint32_t max_factor);

#endif /* VORTECH_OPT_H */
