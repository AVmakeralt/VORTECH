/*
 * VORTECH Compiler - Optimization Pipeline Runner
 *
 * Pipeline:
 *   Constant Folding -> SCCP -> Copy Propagation -> CSE -> DCE -> LICM -> Unrolling
 * Each step justified by measurements. Not "this paper says it gives 3%."
 */
#include "opt.h"
#include "ssa.h"

void opt_run_all(SsaFunc *func, Arena *arena) {
    (void)arena;

    /* Pass 1: Constant Folding (tiny complexity, huge value) */
    opt_const_fold(func);

    /* Pass 2: SCCP (sparse conditional constant propagation) */
    opt_sccp(func);

    /* Pass 3: Copy Propagation (tiny complexity, huge value) */
    opt_copy_prop(func);

    /* Pass 4: CSE (moderate complexity, huge value) */
    opt_cse(func);

    /* Pass 5: DCE (tiny complexity, huge value) */
    opt_dce(func);

    /* Pass 6: LICM (only obvious cases) */
    opt_licm(func);

    /* Pass 7: Simple Loop Unrolling (only when profitable) */
    opt_unroll(func, 4);

    /* Run another round of DCE to clean up after other passes */
    opt_dce(func);

    /* Verify SSA integrity after all passes */
    ssa_verify_func(func);
}
