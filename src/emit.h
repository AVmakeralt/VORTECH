/*
 * VORTECH Compiler - x86-64 Code Emitter
 *
 * Generates Intel-syntax x86-64 assembly.
 * Output can be assembled with GCC or GAS.
 */
#ifndef VORTECH_EMIT_H
#define VORTECH_EMIT_H

#include "common.h"
#include "arena.h"
#include "isel.h"
#include "regalloc.h"

/* Emit assembly for a machine function to a file */
void emit_func(FILE *out, MachFunc *mf, RegAllocResult *ra);

/* Emit the full program as an assembly file */
void emit_program(FILE *out, MachFunc *funcs, uint32_t nfuncs,
                  RegAllocResult **ras);

#endif /* VORTECH_EMIT_H */
