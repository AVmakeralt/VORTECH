/*
 * VORTECH Compiler - Parser Implementation
 *
 * Recursive descent parser for the VORTECH language.
 * Grammar is close to C with: fn, let, arena, alloc, no classes.
 */
#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

/* ---- Forward declarations ---- */
static AstNode *parse_expr(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);
static VtType  *parse_type(Parser *p);

/* ---- Helpers ---- */

static Token cur(Parser *p) {
    return lexer_peek(p->lex);
}

static Token advance(Parser *p) {
    return lexer_next(p->lex);
}

static bool match(Parser *p, TokenKind kind) {
    return lexer_match(p->lex, kind);
}

static Token expect(Parser *p, TokenKind kind) {
    return lexer_expect(p->lex, kind);
}

static bool check(Parser *p, TokenKind kind) {
    return cur(p).kind == kind;
}

static char *dup_ident(Parser *p, const char *s) {
    return arena_strdup(p->arena, s);
}

/* ---- Type parsing ---- */

static VtType *parse_type(Parser *p) {
    Token tok = cur(p);
    VtType *base = NULL;

    switch (tok.kind) {
    case TOK_VOID:  advance(p); base = vt_type_make(p->arena, VTTYPE_VOID);  break;
    case TOK_I8:    advance(p); base = vt_type_make(p->arena, VTTYPE_I8);    break;
    case TOK_I16:   advance(p); base = vt_type_make(p->arena, VTTYPE_I16);   break;
    case TOK_I32:   advance(p); base = vt_type_make(p->arena, VTTYPE_I32);   break;
    case TOK_I64:   advance(p); base = vt_type_make(p->arena, VTTYPE_I64);   break;
    case TOK_U8:    advance(p); base = vt_type_make(p->arena, VTTYPE_U8);    break;
    case TOK_U16:   advance(p); base = vt_type_make(p->arena, VTTYPE_U16);   break;
    case TOK_U32:   advance(p); base = vt_type_make(p->arena, VTTYPE_U32);   break;
    case TOK_U64:   advance(p); base = vt_type_make(p->arena, VTTYPE_U64);   break;
    case TOK_F32:   advance(p); base = vt_type_make(p->arena, VTTYPE_F32);   break;
    case TOK_F64:   advance(p); base = vt_type_make(p->arena, VTTYPE_F64);   break;
    case TOK_BOOL:  advance(p); base = vt_type_make(p->arena, VTTYPE_BOOL);  break;
    case TOK_IDENT: {
        /* Struct type name */
        advance(p);
        base = vt_type_make(p->arena, VTTYPE_STRUCT);
        base->name = dup_ident(p, tok.ident);
        break;
    }
    default:
        diag_report(DIAG_ERROR, tok.loc, "expected type, got %s", token_kind_str(tok.kind));
        advance(p);
        return vt_type_make(p->arena, VTTYPE_I32); /* error recovery */
    }

    /* Pointer suffix: * */
    while (check(p, TOK_STAR)) {
        advance(p);
        base = vt_type_make_ptr(p->arena, base);
    }

    /* Array suffix: [N] */
    while (check(p, TOK_LBRACKET)) {
        advance(p);
        uint64_t count = 0;
        if (check(p, TOK_INT_LIT)) {
            Token n = advance(p);
            count = (uint64_t)n.int_val;
        }
        expect(p, TOK_RBRACKET);
        base = vt_type_make_array(p->arena, base, count);
    }

    return base;
}

/* ---- Expression parsing (precedence climbing) ---- */

static AstNode *parse_primary(Parser *p) {
    Token tok = cur(p);

    switch (tok.kind) {
    case TOK_INT_LIT: {
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_INT_LIT, tok.loc);
        n->int_lit.value = tok.int_val;
        return n;
    }
    case TOK_FLOAT_LIT: {
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_FLOAT_LIT, tok.loc);
        n->float_lit.value = tok.float_val;
        return n;
    }
    case TOK_TRUE:
    case TOK_FALSE: {
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_BOOL_LIT, tok.loc);
        n->bool_lit.value = (tok.kind == TOK_TRUE);
        return n;
    }
    case TOK_STRING_LIT: {
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_STRING_LIT, tok.loc);
        n->string_lit.value = dup_ident(p, tok.str_val);
        return n;
    }
    case TOK_IDENT: {
        advance(p);
        /* Could be: ident, ident(), ident[idx], ident.field, ident->field */
        AstNode *n = ast_alloc(p->arena, AST_IDENT_EXPR, tok.loc);
        n->ident_expr.name = dup_ident(p, tok.ident);

        /* Postfix operators */
        while (1) {
            if (check(p, TOK_LPAREN)) {
                /* Function call */
                advance(p);
                AstNode *call = ast_alloc(p->arena, AST_CALL_EXPR, tok.loc);
                call->call_expr.name = n->ident_expr.name;
                call->call_expr.args = NULL;
                call->call_expr.nargs = 0;

                uint32_t cap = 0;
                while (!check(p, TOK_RPAREN)) {
                    AstNode *arg = parse_expr(p);
                    vt_da_push(call->call_expr.args, call->call_expr.nargs, cap, arg);
                    if (!match(p, TOK_COMMA)) break;
                }
                expect(p, TOK_RPAREN);
                n = call;
            } else if (check(p, TOK_LBRACKET)) {
                /* Index access */
                advance(p);
                AstNode *idx = ast_alloc(p->arena, AST_INDEX_EXPR, tok.loc);
                idx->index_expr.object = n;
                idx->index_expr.index = parse_expr(p);
                expect(p, TOK_RBRACKET);
                n = idx;
            } else if (check(p, TOK_DOT)) {
                /* Field access */
                advance(p);
                Token field = expect(p, TOK_IDENT);
                AstNode *fld = ast_alloc(p->arena, AST_FIELD_EXPR, tok.loc);
                fld->field_expr.object = n;
                fld->field_expr.field = dup_ident(p, field.ident);
                n = fld;
            } else {
                break;
            }
        }
        return n;
    }
    case TOK_LPAREN: {
        advance(p);
        AstNode *n = parse_expr(p);
        expect(p, TOK_RPAREN);
        return n;
    }
    case TOK_ALLOC: {
        advance(p);
        expect(p, TOK_LPAREN);
        Token arena_tok = expect(p, TOK_IDENT);
        expect(p, TOK_COMMA);
        VtType *ty = parse_type(p);
        expect(p, TOK_RPAREN);

        AstNode *n = ast_alloc(p->arena, AST_ALLOC_EXPR, tok.loc);
        n->alloc_expr.arena_name = dup_ident(p, arena_tok.ident);
        n->alloc_expr.type = ty;
        return n;
    }
    case TOK_SIZEOF: {
        advance(p);
        expect(p, TOK_LPAREN);
        VtType *ty = parse_type(p);
        expect(p, TOK_RPAREN);

        AstNode *n = ast_alloc(p->arena, AST_SIZEOF_EXPR, tok.loc);
        n->sizeof_expr.type = ty;
        return n;
    }
    case TOK_BANG: {
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_UNARY_EXPR, tok.loc);
        n->unary_expr.op = TOK_BANG;
        n->unary_expr.operand = parse_primary(p);
        return n;
    }
    case TOK_MINUS: {
        /* Unary negation */
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_UNARY_EXPR, tok.loc);
        n->unary_expr.op = TOK_MINUS;
        n->unary_expr.operand = parse_primary(p);
        return n;
    }
    case TOK_TILDE: {
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_UNARY_EXPR, tok.loc);
        n->unary_expr.op = TOK_TILDE;
        n->unary_expr.operand = parse_primary(p);
        return n;
    }
    case TOK_STAR: {
        /* Dereference */
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_DEREF_EXPR, tok.loc);
        n->deref_expr.operand = parse_primary(p);
        return n;
    }
    case TOK_AMP: {
        /* Address-of */
        advance(p);
        AstNode *n = ast_alloc(p->arena, AST_ADDR_EXPR, tok.loc);
        n->addr_expr.operand = parse_primary(p);
        return n;
    }
    default:
        diag_report(DIAG_ERROR, tok.loc, "expected expression, got %s", token_kind_str(tok.kind));
        advance(p);
        return ast_alloc(p->arena, AST_INT_LIT, tok.loc); /* error recovery */
    }
}

/* Operator precedence levels (higher = binds tighter) */
static int binop_precedence(TokenKind op) {
    switch (op) {
    case TOK_PIPEPIPE: return 1;  /* || */
    case TOK_AMPAMP:   return 2;  /* && */
    case TOK_PIPE:     return 3;  /* | */
    case TOK_CARET:    return 4;  /* ^ */
    case TOK_AMP:      return 5;  /* & */
    case TOK_EQEQ:
    case TOK_NEQ:      return 6;  /* == != */
    case TOK_LT:
    case TOK_GT:
    case TOK_LEQ:
    case TOK_GEQ:      return 7;  /* < > <= >= */
    case TOK_LSHIFT:
    case TOK_RSHIFT:   return 8;  /* << >> */
    case TOK_PLUS:
    case TOK_MINUS:    return 9;  /* + - */
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:  return 10; /* * / % */
    default:           return 0;  /* not a binary op */
    }
}

static bool is_binop(TokenKind kind) {
    return binop_precedence(kind) > 0;
}

/* Right-associative operators */
static bool is_right_assoc(TokenKind op) {
    (void)op;
    return false; /* no right-associative binary ops in VORTECH currently */
}

static AstNode *parse_expr_prec(Parser *p, int min_prec) {
    AstNode *left = parse_primary(p);

    for (;;) {
        Token op_tok = cur(p);
        int prec = binop_precedence(op_tok.kind);
        if (prec < min_prec) break;

        advance(p);
        int next_prec = is_right_assoc(op_tok.kind) ? prec : prec + 1;
        AstNode *right = parse_expr_prec(p, next_prec);

        AstNode *bin = ast_alloc(p->arena, AST_BINARY_EXPR, op_tok.loc);
        bin->binary_expr.op = op_tok.kind;
        bin->binary_expr.left = left;
        bin->binary_expr.right = right;
        left = bin;
    }

    return left;
}

static AstNode *parse_expr(Parser *p) {
    return parse_expr_prec(p, 1);
}

/* ---- Statement parsing ---- */

static AstNode *parse_let_stmt(Parser *p) {
    Token tok = advance(p); /* consume 'let' */
    Token name_tok = expect(p, TOK_IDENT);
    VtType *ty = NULL;

    if (match(p, TOK_COLON)) {
        ty = parse_type(p);
    }

    AstNode *init = NULL;
    if (match(p, TOK_EQ)) {
        init = parse_expr(p);
    }

    expect(p, TOK_SEMI);

    AstNode *n = ast_alloc(p->arena, AST_LET_STMT, tok.loc);
    n->let_stmt.name = dup_ident(p, name_tok.ident);
    n->let_stmt.type = ty;
    n->let_stmt.init = init;
    return n;
}

static AstNode *parse_if_stmt(Parser *p) {
    Token tok = advance(p); /* consume 'if' */
    expect(p, TOK_LPAREN);
    AstNode *cond = parse_expr(p);
    expect(p, TOK_RPAREN);

    AstNode *then_block = parse_block(p);
    AstNode *else_block = NULL;

    if (match(p, TOK_ELSE)) {
        if (check(p, TOK_IF)) {
            /* else if */
            else_block = parse_if_stmt(p);
        } else {
            else_block = parse_block(p);
        }
    }

    AstNode *n = ast_alloc(p->arena, AST_IF_STMT, tok.loc);
    n->if_stmt.condition = cond;
    n->if_stmt.then_block = then_block;
    n->if_stmt.else_block = else_block;
    return n;
}

static AstNode *parse_while_stmt(Parser *p) {
    Token tok = advance(p); /* consume 'while' */
    expect(p, TOK_LPAREN);
    AstNode *cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    AstNode *body = parse_block(p);

    AstNode *n = ast_alloc(p->arena, AST_WHILE_STMT, tok.loc);
    n->while_stmt.condition = cond;
    n->while_stmt.body = body;
    return n;
}

static AstNode *parse_for_stmt(Parser *p) {
    Token tok = advance(p); /* consume 'for' */
    expect(p, TOK_LPAREN);

    AstNode *init = NULL;
    if (!check(p, TOK_SEMI)) {
        if (check(p, TOK_LET)) {
            init = parse_let_stmt(p);
        } else {
            init = parse_expr(p);
            expect(p, TOK_SEMI);
        }
    } else {
        advance(p); /* consume ; */
    }

    AstNode *cond = NULL;
    if (!check(p, TOK_SEMI)) {
        cond = parse_expr(p);
    }
    expect(p, TOK_SEMI);

    AstNode *update = NULL;
    if (!check(p, TOK_RPAREN)) {
        update = parse_expr(p);
    }
    expect(p, TOK_RPAREN);

    AstNode *body = parse_block(p);

    AstNode *n = ast_alloc(p->arena, AST_FOR_STMT, tok.loc);
    n->for_stmt.init = init;
    n->for_stmt.condition = cond;
    n->for_stmt.update = update;
    n->for_stmt.body = body;
    return n;
}

static AstNode *parse_return_stmt(Parser *p) {
    Token tok = advance(p); /* consume 'return' */
    AstNode *val = NULL;
    if (!check(p, TOK_SEMI)) {
        val = parse_expr(p);
    }
    expect(p, TOK_SEMI);

    AstNode *n = ast_alloc(p->arena, AST_RETURN_STMT, tok.loc);
    n->return_stmt.value = val;
    return n;
}

static AstNode *parse_expr_or_assign_stmt(Parser *p) {
    AstNode *expr = parse_expr(p);

    /* Check for assignment */
    if (check(p, TOK_EQ) || check(p, TOK_PLUSEQ) || check(p, TOK_MINUSEQ) ||
        check(p, TOK_STAREQ) || check(p, TOK_SLASHEQ)) {
        Token op = advance(p);
        AstNode *val = parse_expr(p);
        expect(p, TOK_SEMI);

        AstNode *n = ast_alloc(p->arena, AST_ASSIGN_STMT, op.loc);
        n->assign_stmt.target = expr;
        n->assign_stmt.value = val;
        n->assign_stmt.compound_op = op.kind;
        return n;
    }

    expect(p, TOK_SEMI);
    AstNode *n = ast_alloc(p->arena, AST_EXPR_STMT, expr->loc);
    n->expr_stmt.expr = expr;
    return n;
}

static AstNode *parse_stmt(Parser *p) {
    Token tok = cur(p);

    switch (tok.kind) {
    case TOK_LET:
        return parse_let_stmt(p);
    case TOK_IF:
        return parse_if_stmt(p);
    case TOK_WHILE:
        return parse_while_stmt(p);
    case TOK_FOR:
        return parse_for_stmt(p);
    case TOK_RETURN:
        return parse_return_stmt(p);
    case TOK_LBRACE:
        return parse_block(p);
    case TOK_ARENA: {
        /* Check if this is arena_reset or arena declaration */
        if (cur(p).kind == TOK_ARENA) {
            advance(p); /* consume 'arena' */
            if (check(p, TOK_IDENT)) {
                Token name_tok = advance(p);
                if (check(p, TOK_SEMI)) {
                    /* Arena declaration: arena frame; */
                    advance(p);
                    AstNode *n = ast_alloc(p->arena, AST_ARENA_DECL, tok.loc);
                    n->arena_decl.name = dup_ident(p, name_tok.ident);
                    return n;
                }
                /* Could be arena_reset(frame) */
                /* Actually check for arena_reset */
                /* We need to handle: arena_reset(frame); */
                /* But the keyword is just 'arena' so let me re-think */
                /* For now, treat as arena declaration */
                expect(p, TOK_SEMI);
                AstNode *n = ast_alloc(p->arena, AST_ARENA_DECL, tok.loc);
                n->arena_decl.name = dup_ident(p, name_tok.ident);
                return n;
            }
        }
        diag_report(DIAG_ERROR, tok.loc, "expected arena declaration");
        advance(p);
        return ast_alloc(p->arena, AST_EXPR_STMT, tok.loc);
    }
    default:
        return parse_expr_or_assign_stmt(p);
    }
}

static AstNode *parse_block(Parser *p) {
    Token tok = expect(p, TOK_LBRACE);
    AstNode *block = ast_alloc(p->arena, AST_BLOCK, tok.loc);
    block->block.stmts = NULL;
    block->block.nstmts = 0;

    uint32_t cap = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        /* Check for arena_reset as a statement */
        if (check(p, TOK_IDENT) && cur(p).ident &&
            strcmp(cur(p).ident, "arena_reset") == 0) {
            advance(p); /* consume 'arena_reset' */
            expect(p, TOK_LPAREN);
            Token name_tok = expect(p, TOK_IDENT);
            expect(p, TOK_RPAREN);
            expect(p, TOK_SEMI);
            AstNode *n = ast_alloc(p->arena, AST_ARENA_RESET_STMT, tok.loc);
            n->arena_reset_stmt.name = dup_ident(p, name_tok.ident);
            vt_da_push(block->block.stmts, block->block.nstmts, cap, n);
        } else {
            AstNode *stmt = parse_stmt(p);
            vt_da_push(block->block.stmts, block->block.nstmts, cap, stmt);
        }
    }
    expect(p, TOK_RBRACE);
    return block;
}

/* ---- Top-level declarations ---- */

static AstNode *parse_fn_decl(Parser *p) {
    Token fn_tok = advance(p); /* consume 'fn' */
    Token name_tok = expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);

    AstParam *params = NULL;
    uint32_t nparams = 0;
    uint32_t pcap = 0;

    while (!check(p, TOK_RPAREN)) {
        Token pname = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        VtType *pty = parse_type(p);
        AstParam param;
        param.name = dup_ident(p, pname.ident);
        param.type = pty;
        vt_da_push(params, nparams, pcap, param);
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);

    VtType *ret_type = vt_type_make(p->arena, VTTYPE_VOID);
    if (match(p, TOK_ARROW)) {
        ret_type = parse_type(p);
    }

    AstNode *body = NULL;
    if (check(p, TOK_LBRACE)) {
        body = parse_block(p);
    } else {
        expect(p, TOK_SEMI); /* extern declaration */
    }

    AstNode *n = ast_alloc(p->arena, AST_FN_DECL, fn_tok.loc);
    n->fn_decl.name = dup_ident(p, name_tok.ident);
    n->fn_decl.params = params;
    n->fn_decl.nparams = nparams;
    n->fn_decl.ret_type = ret_type;
    n->fn_decl.body = body;
    n->fn_decl.is_extern = (body == NULL);
    return n;
}

static AstNode *parse_struct_decl(Parser *p) {
    Token tok = advance(p); /* consume 'struct' */
    Token name_tok = expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    AstField *fields = NULL;
    uint32_t nfields = 0;
    uint32_t fcap = 0;

    while (!check(p, TOK_RBRACE)) {
        Token fname = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        VtType *fty = parse_type(p);
        match(p, TOK_COMMA); /* optional trailing comma */

        AstField field;
        field.name = dup_ident(p, fname.ident);
        field.type = fty;
        vt_da_push(fields, nfields, fcap, field);
    }
    expect(p, TOK_RBRACE);

    AstNode *n = ast_alloc(p->arena, AST_STRUCT_DECL, tok.loc);
    n->struct_decl.name = dup_ident(p, name_tok.ident);
    n->struct_decl.fields = fields;
    n->struct_decl.nfields = nfields;
    return n;
}

/* ---- Main parse function ---- */

AstNode *parser_parse(Parser *p) {
    AstNode *prog = ast_alloc(p->arena, AST_PROGRAM, (SrcLoc){p->lex->filename, 1, 1});
    prog->program.decls = NULL;
    prog->program.ndecls = 0;

    uint32_t cap = 0;
    while (!check(p, TOK_EOF)) {
        AstNode *decl = NULL;
        if (check(p, TOK_FN)) {
            decl = parse_fn_decl(p);
        } else if (check(p, TOK_STRUCT)) {
            decl = parse_struct_decl(p);
        } else if (check(p, TOK_ARENA)) {
            advance(p); /* consume 'arena' */
            Token name_tok = expect(p, TOK_IDENT);
            expect(p, TOK_SEMI);
            decl = ast_alloc(p->arena, AST_ARENA_DECL, name_tok.loc);
            decl->arena_decl.name = dup_ident(p, name_tok.ident);
        } else {
            diag_report(DIAG_ERROR, cur(p).loc,
                        "expected top-level declaration, got %s", token_kind_str(cur(p).kind));
            advance(p);
            continue;
        }
        if (decl) {
            vt_da_push(prog->program.decls, prog->program.ndecls, cap, decl);
        }
    }
    return prog;
}
