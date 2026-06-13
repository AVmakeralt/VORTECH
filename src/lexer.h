/*
 * VORTECH Compiler - Lexer
 */
#ifndef VORTECH_LEXER_H
#define VORTECH_LEXER_H

#include "common.h"
#include "arena.h"

typedef struct {
    const char *source;    /* full source text */
    const char *filename;  /* for diagnostics */
    size_t      pos;       /* current byte offset */
    uint32_t    line;
    uint32_t    col;
    Token       current;   /* current token (peeked) */
    bool        has_peeked;/* whether current is valid */
    Arena      *arena;     /* for string allocations */
} Lexer;

/* Initialize lexer with source text */
void lexer_init(Lexer *lex, const char *source, const char *filename, Arena *arena);

/* Get the next token */
Token lexer_next(Lexer *lex);

/* Peek at the current token without consuming it */
Token lexer_peek(Lexer *lex);

/* Expect a specific token kind, error if not matched */
Token lexer_expect(Lexer *lex, TokenKind kind);

/* Check if the current token is of a given kind */
bool lexer_match(Lexer *lex, TokenKind kind);

/* Get human-readable token name */
const char *token_kind_str(TokenKind kind);

#endif /* VORTECH_LEXER_H */
