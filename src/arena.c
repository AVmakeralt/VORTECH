/*
 * VORTECH Compiler - Arena Allocator Implementation
 */
#include "arena.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

Arena *arena_create(size_t page_size) {
    Arena *a = malloc(sizeof(Arena));
    if (!a) {
        fprintf(stderr, "vortech: out of memory creating arena\n");
        exit(1);
    }
    a->page_size = page_size > 0 ? page_size : VT_ARENA_PAGE_SIZE;
    a->first = NULL;
    a->current = NULL;
    a->total_allocated = 0;

    /* Pre-allocate first page */
    ArenaPage *p = malloc(sizeof(ArenaPage));
    if (!p) {
        fprintf(stderr, "vortech: out of memory creating arena page\n");
        exit(1);
    }
    p->base = malloc(a->page_size);
    if (!p->base) {
        fprintf(stderr, "vortech: out of memory allocating arena page\n");
        exit(1);
    }
    p->used = 0;
    p->capacity = a->page_size;
    p->next = NULL;

    a->first = p;
    a->current = p;
    return a;
}

static ArenaPage *arena_new_page(Arena *a, size_t min_size) {
    size_t cap = a->page_size;
    if (min_size > cap) {
        cap = (min_size + a->page_size - 1) / a->page_size * a->page_size;
    }

    ArenaPage *p = malloc(sizeof(ArenaPage));
    if (!p) {
        fprintf(stderr, "vortech: out of memory creating arena page\n");
        exit(1);
    }
    p->base = malloc(cap);
    if (!p->base) {
        fprintf(stderr, "vortech: out of memory allocating arena page (%zu bytes)\n", cap);
        exit(1);
    }
    p->used = 0;
    p->capacity = cap;
    p->next = NULL;
    return p;
}

void *arena_alloc_aligned(Arena *a, size_t size, size_t align) {
    if (!a || size == 0) return NULL;

    ArenaPage *p = a->current;

    /* Align current offset */
    uintptr_t offset = (uintptr_t)p->base + p->used;
    uintptr_t aligned = (offset + align - 1) & ~(align - 1);
    size_t padding = aligned - offset;

    /* If doesn't fit in current page, allocate new page */
    if (p->used + padding + size > p->capacity) {
        ArenaPage *np = arena_new_page(a, size + align);
        np->next = a->first;
        a->first = np;
        a->current = np;
        p = np;

        offset = (uintptr_t)p->base;
        aligned = (offset + align - 1) & ~(align - 1);
        padding = aligned - offset;
    }

    void *ptr = (char *)aligned;
    p->used += padding + size;
    a->total_allocated += padding + size;
    return ptr;
}

void *arena_alloc(Arena *a, size_t size) {
    return arena_alloc_aligned(a, size, sizeof(void *));
}

void *arena_calloc(Arena *a, size_t count, size_t elem_size) {
    size_t total = count * elem_size;
    /* Overflow check */
    if (count != 0 && total / count != elem_size) {
        fprintf(stderr, "vortech: arena_calloc overflow\n");
        exit(1);
    }
    void *ptr = arena_alloc(a, total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

char *arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = arena_alloc(a, len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

void arena_reset(Arena *a) {
    if (!a) return;
    /* Free all pages except the first */
    ArenaPage *p = a->first;
    while (p) {
        ArenaPage *next = p->next;
        if (p != a->first) {
            free(p->base);
            free(p);
        }
        p = next;
    }
    a->first->used = 0;
    a->first->next = NULL;
    a->current = a->first;
    a->total_allocated = 0;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    ArenaPage *p = a->first;
    while (p) {
        ArenaPage *next = p->next;
        free(p->base);
        free(p);
        p = next;
    }
    free(a);
}

size_t arena_used(Arena *a) {
    if (!a) return 0;
    return a->total_allocated;
}
