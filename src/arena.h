/*
 * VORTECH Compiler - Arena Allocator
 * O(1) allocation, O(1) deallocation, great cache locality.
 */
#ifndef VORTECH_ARENA_H
#define VORTECH_ARENA_H

#include "common.h"

typedef struct ArenaPage {
    char             *base;
    size_t            used;
    size_t            capacity;
    struct ArenaPage *next;
} ArenaPage;

typedef struct {
    ArenaPage *first;
    ArenaPage *current;
    size_t     page_size;
    size_t     total_allocated;
} Arena;

/* Create a new arena with the given page size (0 = default) */
Arena *arena_create(size_t page_size);

/* Allocate `size` bytes with `align` alignment from the arena */
void *arena_alloc_aligned(Arena *a, size_t size, size_t align);

/* Allocate `size` bytes with default alignment */
void *arena_alloc(Arena *a, size_t size);

/* Allocate and zero-fill */
void *arena_calloc(Arena *a, size_t count, size_t elem_size);

/* Duplicate a string into the arena */
char *arena_strdup(Arena *a, const char *s);

/* Reset arena: free all pages after the first, reset first page offset */
void arena_reset(Arena *a);

/* Destroy arena: free all memory */
void arena_destroy(Arena *a);

/* Get total bytes used (for diagnostics/budget) */
size_t arena_used(Arena *a);

#endif /* VORTECH_ARENA_H */
