/*
 * VORTECH Compiler - Lexer Implementation
 */
#include "lexer.h"
#include "diag.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    const char *text;
    TokenKind   kind;
} Keyword;

static Keyword keywords[] = {
    {"fn",      TOK_FN},
    {"let",     TOK_LET},
    {"struct",  TOK_STRUCT},
    {"if",      TOK_IF},
    {"else",    TOK_ELSE},
    {"while",   TOK_WHILE},
    {"for",     TOK_FOR},
    {"return",  TOK_RETURN},
    {"arena",   TOK_ARENA},
    {"alloc",   TOK_ALLOC},
    {"true",    TOK_TRUE},
    {"false",   TOK_FALSE},
    {"as",      TOK_AS},
    {"sizeof",  TOK_SIZEOF},
    /* print is NOT a keyword - it's a built-in function, parsed as a call */
    /* type keywords */
    {"i8",      TOK_I8},
    {"i16",     TOK_I16},
    {"i32",     TOK_I32},
    {"i64",     TOK_I64},
    {"u8",      TOK_U8},
    {"u16",     TOK_U16},
    {"u32",     TOK_U32},
    {"u64",     TOK_U64},
    {"f32",     TOK_F32},
    {"f64",     TOK_F64},
    {"bool",    TOK_BOOL},
    {"void",    TOK_VOID},
    {NULL,      TOK_EOF},
};

static TokenKind lookup_keyword(const char *text, size_t len) {
    for (int i = 0; keywords[i].text; i++) {
        if (strlen(keywords[i].text) == len &&
            strncmp(keywords[i].text, text, len) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

const char *token_kind_str(TokenKind kind) {
    switch (kind) {
    case TOK_EOF:       return "EOF";
    case TOK_FN:        return "fn";
    case TOK_LET:       return "let";
    case TOK_STRUCT:    return "struct";
    case TOK_IF:        return "if";
    case TOK_ELSE:      return "else";
    case TOK_WHILE:     return "while";
    case TOK_FOR:       return "for";
    case TOK_RETURN:    return "return";
    case TOK_ARENA:     return "arena";
    case TOK_ALLOC:     return "alloc";
    case TOK_TRUE:      return "true";
    case TOK_FALSE:     return "false";
    case TOK_PRINT:     return "print";
    case TOK_AS:        return "as";
    case TOK_SIZEOF:    return "sizeof";
    case TOK_I8:        return "i8";
    case TOK_I16:       return "i16";
    case TOK_I32:       return "i32";
    case TOK_I64:       return "i64";
    case TOK_U8:        return "u8";
    case TOK_U16:       return "u16";
    case TOK_U32:       return "u32";
    case TOK_U64:       return "u64";
    case TOK_F32:       return "f32";
    case TOK_F64:       return "f64";
    case TOK_BOOL:      return "bool";
    case TOK_VOID:      return "void";
    case TOK_PLUS:      return "+";
    case TOK_MINUS:     return "-";
    case TOK_STAR:      return "*";
    case TOK_SLASH:     return "/";
    case TOK_PERCENT:   return "%";
    case TOK_AMP:       return "&";
    case TOK_PIPE:      return "|";
    case TOK_CARET:     return "^";
    case TOK_LSHIFT:    return "<<";
    case TOK_RSHIFT:    return ">>";
    case TOK_EQEQ:     return "==";
    case TOK_NEQ:       return "!=";
    case TOK_LT:        return "<";
    case TOK_GT:        return ">";
    case TOK_LEQ:       return "<=";
    case TOK_GEQ:       return ">=";
    case TOK_AMPAMP:    return "&&";
    case TOK_PIPEPIPE:  return "||";
    case TOK_BANG:       return "!";
    case TOK_TILDE:     return "~";
    case TOK_EQ:        return "=";
    case TOK_PLUSEQ:    return "+=";
    case TOK_MINUSEQ:   return "-=";
    case TOK_STAREQ:    return "*=";
    case TOK_SLASHEQ:   return "/=";
    case TOK_LPAREN:    return "(";
    case TOK_RPAREN:    return ")";
    case TOK_LBRACE:    return "{";
    case TOK_RBRACE:    return "}";
    case TOK_LBRACKET:  return "[";
    case TOK_RBRACKET:  return "]";
    case TOK_COMMA:     return ",";
    case TOK_SEMI:      return ";";
    case TOK_COLON:     return ":";
    case TOK_ARROW:     return "->";
    case TOK_DOT:       return ".";
    case TOK_INT_LIT:   return "integer literal";
    case TOK_FLOAT_LIT: return "float literal";
    case TOK_STRING_LIT:return "string literal";
    case TOK_IDENT:     return "identifier";
    }
    return "unknown token";
}

static SrcLoc make_loc(Lexer *lex) {
    SrcLoc loc;
    loc.filename = lex->filename;
    loc.line = lex->line;
    loc.col = lex->col;
    return loc;
}

static char peek_char(Lexer *lex) {
    if (lex->source[lex->pos] == '\0') return '\0';
    return lex->source[lex->pos];
}

static char peek_char_at(Lexer *lex, size_t offset) {
    if (lex->source[lex->pos + offset] == '\0') return '\0';
    return lex->source[lex->pos + offset];
}

static char advance_char(Lexer *lex) {
    char c = lex->source[lex->pos];
    if (c == '\0') return '\0';
    lex->pos++;
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

static void skip_whitespace_and_comments(Lexer *lex) {
    for (;;) {
        char c = peek_char(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_char(lex);
        } else if (c == '/' && peek_char_at(lex, 1) == '/') {
            /* line comment */
            while (peek_char(lex) != '\n' && peek_char(lex) != '\0') {
                advance_char(lex);
            }
        } else if (c == '/' && peek_char_at(lex, 1) == '*') {
            /* block comment */
            advance_char(lex); /* / */
            advance_char(lex); /* * */
            int depth = 1;
            while (depth > 0 && peek_char(lex) != '\0') {
                if (peek_char(lex) == '/' && peek_char_at(lex, 1) == '*') {
                    advance_char(lex);
                    advance_char(lex);
                    depth++;
                } else if (peek_char(lex) == '*' && peek_char_at(lex, 1) == '/') {
                    advance_char(lex);
                    advance_char(lex);
                    depth--;
                } else {
                    advance_char(lex);
                }
            }
        } else {
            break;
        }
    }
}

static Token make_ident_or_keyword(Lexer *lex) {
    SrcLoc loc = make_loc(lex);
    size_t start = lex->pos;
    while (isalnum((unsigned char)peek_char(lex)) || peek_char(lex) == '_') {
        advance_char(lex);
    }
    size_t len = lex->pos - start;
    TokenKind kind = lookup_keyword(lex->source + start, len);

    Token tok;
    tok.kind = kind;
    tok.loc = loc;
    if (kind == TOK_IDENT) {
        tok.ident = arena_alloc(lex->arena, len + 1);
        memcpy(tok.ident, lex->source + start, len);
        tok.ident[len] = '\0';
    } else {
        tok.ident = NULL;
    }
    return tok;
}

static Token make_number(Lexer *lex) {
    SrcLoc loc = make_loc(lex);
    size_t start = lex->pos;

    bool is_float = false;
    int base = 10;

    /* Handle 0x, 0b, 0o prefixes */
    if (peek_char(lex) == '0') {
        char next = peek_char_at(lex, 1);
        if (next == 'x' || next == 'X') {
            base = 16;
            advance_char(lex); /* 0 */
            advance_char(lex); /* x */
            start = lex->pos;
        } else if (next == 'b' || next == 'B') {
            base = 2;
            advance_char(lex);
            advance_char(lex);
            start = lex->pos;
        } else if (next == 'o' || next == 'O') {
            base = 8;
            advance_char(lex);
            advance_char(lex);
            start = lex->pos;
        }
    }

    /* Integer part */
    while (isdigit((unsigned char)peek_char(lex))) {
        advance_char(lex);
    }

    /* Float: . or e/E */
    if (peek_char(lex) == '.' && peek_char_at(lex, 1) != '.') {
        is_float = true;
        advance_char(lex);
        while (isdigit((unsigned char)peek_char(lex))) {
            advance_char(lex);
        }
    }
    if (peek_char(lex) == 'e' || peek_char(lex) == 'E') {
        is_float = true;
        advance_char(lex);
        if (peek_char(lex) == '+' || peek_char(lex) == '-') {
            advance_char(lex);
        }
        while (isdigit((unsigned char)peek_char(lex))) {
            advance_char(lex);
        }
    }

    size_t len = lex->pos - start;
    char *buf = malloc(len + 1);
    if (!buf) {
        diag_report(DIAG_FATAL, loc, "out of memory lexing number");
    }
    memcpy(buf, lex->source + start, len);
    buf[len] = '\0';

    Token tok;
    tok.loc = loc;
    if (is_float) {
        tok.kind = TOK_FLOAT_LIT;
        tok.float_val = strtod(buf, NULL);
    } else {
        tok.kind = TOK_INT_LIT;
        tok.int_val = strtoll(buf, NULL, base);
    }
    free(buf);
    return tok;
}

static Token make_string(Lexer *lex) {
    SrcLoc loc = make_loc(lex);
    advance_char(lex); /* opening quote */

    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        diag_report(DIAG_FATAL, loc, "out of memory lexing string");
    }

    while (peek_char(lex) != '"' && peek_char(lex) != '\0') {
        if (len + 2 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) {
                diag_report(DIAG_FATAL, loc, "out of memory lexing string");
            }
        }
        if (peek_char(lex) == '\\') {
            advance_char(lex);
            char esc = peek_char(lex);
            switch (esc) {
            case 'n':  buf[len++] = '\n'; break;
            case 't':  buf[len++] = '\t'; break;
            case 'r':  buf[len++] = '\r'; break;
            case '\\': buf[len++] = '\\'; break;
            case '"':  buf[len++] = '"';  break;
            case '0':  buf[len++] = '\0'; break;
            default:
                diag_report(DIAG_WARN, loc, "unknown escape sequence '\\%c'", esc);
                buf[len++] = esc;
                break;
            }
            advance_char(lex);
        } else {
            buf[len++] = advance_char(lex);
        }
    }

    if (peek_char(lex) == '"') {
        advance_char(lex); /* closing quote */
    } else {
        diag_report(DIAG_ERROR, loc, "unterminated string literal");
    }

    buf[len] = '\0';
    Token tok;
    tok.kind = TOK_STRING_LIT;
    tok.loc = loc;
    tok.str_val = arena_alloc(lex->arena, len + 1);
    memcpy(tok.str_val, buf, len + 1);
    free(buf);
    return tok;
}

void lexer_init(Lexer *lex, const char *source, const char *filename, Arena *arena) {
    lex->source = source;
    lex->filename = filename;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->has_peeked = false;
    lex->arena = arena;
    memset(&lex->current, 0, sizeof(Token));
}

Token lexer_next(Lexer *lex) {
    if (lex->has_peeked) {
        lex->has_peeked = false;
        return lex->current;
    }
    return lexer_peek(lex);
}

Token lexer_peek(Lexer *lex) {
    if (lex->has_peeked) {
        return lex->current;
    }

    skip_whitespace_and_comments(lex);

    Token tok;
    memset(&tok, 0, sizeof(Token));
    SrcLoc loc = make_loc(lex);
    tok.loc = loc;

    char c = peek_char(lex);
    if (c == '\0') {
        tok.kind = TOK_EOF;
        lex->current = tok;
        lex->has_peeked = true;
        return tok;
    }

    /* Identifiers and keywords */
    if (isalpha((unsigned char)c) || c == '_') {
        tok = make_ident_or_keyword(lex);
        lex->current = tok;
        lex->has_peeked = true;
        return tok;
    }

    /* Numbers */
    if (isdigit((unsigned char)c)) {
        tok = make_number(lex);
        lex->current = tok;
        lex->has_peeked = true;
        return tok;
    }

    /* Strings */
    if (c == '"') {
        tok = make_string(lex);
        lex->current = tok;
        lex->has_peeked = true;
        return tok;
    }

    /* Operators and punctuation */
    advance_char(lex);

    switch (c) {
    case '+':
        if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_PLUSEQ; }
        else tok.kind = TOK_PLUS;
        break;
    case '-':
        if (peek_char(lex) == '>') { advance_char(lex); tok.kind = TOK_ARROW; }
        else if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_MINUSEQ; }
        else tok.kind = TOK_MINUS;
        break;
    case '*':
        if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_STAREQ; }
        else tok.kind = TOK_STAR;
        break;
    case '/':
        if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_SLASHEQ; }
        else tok.kind = TOK_SLASH;
        break;
    case '%': tok.kind = TOK_PERCENT;  break;
    case '&':
        if (peek_char(lex) == '&') { advance_char(lex); tok.kind = TOK_AMPAMP; }
        else tok.kind = TOK_AMP;
        break;
    case '|':
        if (peek_char(lex) == '|') { advance_char(lex); tok.kind = TOK_PIPEPIPE; }
        else tok.kind = TOK_PIPE;
        break;
    case '^': tok.kind = TOK_CARET;    break;
    case '~': tok.kind = TOK_TILDE;    break;
    case '!':
        if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_NEQ; }
        else tok.kind = TOK_BANG;
        break;
    case '=':
        if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_EQEQ; }
        else tok.kind = TOK_EQ;
        break;
    case '<':
        if (peek_char(lex) == '<') { advance_char(lex); tok.kind = TOK_LSHIFT; }
        else if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_LEQ; }
        else tok.kind = TOK_LT;
        break;
    case '>':
        if (peek_char(lex) == '>') { advance_char(lex); tok.kind = TOK_RSHIFT; }
        else if (peek_char(lex) == '=') { advance_char(lex); tok.kind = TOK_GEQ; }
        else tok.kind = TOK_GT;
        break;
    case '(': tok.kind = TOK_LPAREN;    break;
    case ')': tok.kind = TOK_RPAREN;    break;
    case '{': tok.kind = TOK_LBRACE;    break;
    case '}': tok.kind = TOK_RBRACE;    break;
    case '[': tok.kind = TOK_LBRACKET;  break;
    case ']': tok.kind = TOK_RBRACKET;  break;
    case ',': tok.kind = TOK_COMMA;     break;
    case ';': tok.kind = TOK_SEMI;      break;
    case ':': tok.kind = TOK_COLON;     break;
    case '.': tok.kind = TOK_DOT;       break;
    default:
        diag_report(DIAG_ERROR, loc, "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
        tok.kind = TOK_EOF;
        break;
    }

    lex->current = tok;
    lex->has_peeked = true;
    return tok;
}

Token lexer_expect(Lexer *lex, TokenKind kind) {
    Token tok = lexer_peek(lex);
    if (tok.kind != kind) {
        diag_report(DIAG_ERROR, tok.loc,
                    "expected %s, got %s", token_kind_str(kind), token_kind_str(tok.kind));
        /* Skip the unexpected token and try to continue */
        lexer_next(lex);
        return tok;
    }
    lexer_next(lex);
    return tok;
}

bool lexer_match(Lexer *lex, TokenKind kind) {
    if (lexer_peek(lex).kind == kind) {
        lexer_next(lex);
        return true;
    }
    return false;
}
