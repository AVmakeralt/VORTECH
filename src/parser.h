/*
 * VORTECH Compiler - Parser
 */
#ifndef VORTECH_PARSER_H
#define VORTECH_PARSER_H

#include "common.h"
#include "arena.h"
#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lex;
    Arena *arena;
} Parser;

/* Parse a complete program */
AstNode *parser_parse(Parser *p);

#endif /* VORTECH_PARSER_H */
