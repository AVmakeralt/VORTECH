/*
 * VORTECH Compiler - Native x86-64 Code Emitter
 *
 * Generates x86-64 machine code directly. No external assembler.
 * Outputs ELF64 relocatable object files (.o).
 * Uses system linker only for linking with C runtime.
 */
#ifndef VORTECH_EMIT_H
#define VORTECH_EMIT_H

#include "common.h"
#include "arena.h"
#include "isel.h"
#include "regalloc.h"

/* Emit all functions as an ELF64 object file */
bool emit_object(const char *path, MachFunc *funcs, uint32_t nfuncs,
                 RegAllocResult **ras);

#endif /* VORTECH_EMIT_H */
