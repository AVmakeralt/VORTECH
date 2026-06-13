/*
 * VORTECH Compiler - Abstract Syntax Tree Definitions
 */
#ifndef VORTECH_AST_H
#define VORTECH_AST_H

#include "common.h"
#include "arena.h"

/* ---- AST Node Kinds ---- */
typedef enum {
    AST_PROGRAM,

    /* Top-level declarations */
    AST_FN_DECL,
    AST_STRUCT_DECL,
    AST_ARENA_DECL,

    /* Statements */
    AST_BLOCK,
    AST_LET_STMT,
    AST_ASSIGN_STMT,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_FOR_STMT,
    AST_RETURN_STMT,
    AST_EXPR_STMT,
    AST_ARENA_RESET_STMT,

    /* Expressions */
    AST_BINARY_EXPR,
    AST_UNARY_EXPR,
    AST_CALL_EXPR,
    AST_INDEX_EXPR,
    AST_FIELD_EXPR,
    AST_IDENT_EXPR,
    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_BOOL_LIT,
    AST_STRING_LIT,
    AST_ALLOC_EXPR,
    AST_DEREF_EXPR,
    AST_ADDR_EXPR,
    AST_CAST_EXPR,
    AST_SIZEOF_EXPR,
    AST_AGGREGATE_LIT,  /* struct literal { .field = val, ... } */
} AstKind;

/* Forward declaration */
typedef struct AstNode AstNode;

/* Function parameter */
typedef struct {
    char    *name;
    VtType  *type;
} AstParam;

/* Struct field */
typedef struct {
    char    *name;
    VtType  *type;
} AstField;

/* Aggregate literal field initializer */
typedef struct {
    char     *name;
    AstNode  *value;
} AstFieldInit;

/* The big AST node union */
struct AstNode {
    AstKind  kind;
    SrcLoc   loc;

    union {
        /* AST_PROGRAM */
        struct {
            AstNode **decls;    /* array of fn/struct/arena decls */
            uint32_t  ndecls;
        } program;

        /* AST_FN_DECL */
        struct {
            char      *name;
            AstParam  *params;
            uint32_t   nparams;
            VtType    *ret_type;
            AstNode   *body;    /* block */
            bool       is_extern;
        } fn_decl;

        /* AST_STRUCT_DECL */
        struct {
            char      *name;
            AstField  *fields;
            uint32_t   nfields;
        } struct_decl;

        /* AST_ARENA_DECL */
        struct {
            char      *name;
        } arena_decl;

        /* AST_BLOCK */
        struct {
            AstNode  **stmts;
            uint32_t   nstmts;
        } block;

        /* AST_LET_STMT */
        struct {
            char     *name;
            VtType   *type;
            AstNode  *init;    /* can be NULL */
            bool      is_mutable; /* always mutable for now */
        } let_stmt;

        /* AST_ASSIGN_STMT */
        struct {
            AstNode  *target;   /* ident, index, field, or deref */
            AstNode  *value;
            TokenKind compound_op; /* TOK_EQ, TOK_PLUSEQ, etc. */
        } assign_stmt;

        /* AST_IF_STMT */
        struct {
            AstNode  *condition;
            AstNode  *then_block;
            AstNode  *else_block; /* can be NULL or if_stmt or block */
        } if_stmt;

        /* AST_WHILE_STMT */
        struct {
            AstNode  *condition;
            AstNode  *body;
        } while_stmt;

        /* AST_FOR_STMT */
        struct {
            AstNode  *init;     /* let_stmt or expr_stmt */
            AstNode  *condition;
            AstNode  *update;   /* expr_stmt */
            AstNode  *body;
        } for_stmt;

        /* AST_RETURN_STMT */
        struct {
            AstNode  *value;   /* can be NULL */
        } return_stmt;

        /* AST_EXPR_STMT */
        struct {
            AstNode  *expr;
        } expr_stmt;

        /* AST_ARENA_RESET_STMT */
        struct {
            char     *name;
        } arena_reset_stmt;

        /* AST_BINARY_EXPR */
        struct {
            TokenKind op;
            AstNode  *left;
            AstNode  *right;
        } binary_expr;

        /* AST_UNARY_EXPR */
        struct {
            TokenKind op;
            AstNode  *operand;
        } unary_expr;

        /* AST_CALL_EXPR */
        struct {
            char     *name;
            AstNode **args;
            uint32_t  nargs;
        } call_expr;

        /* AST_INDEX_EXPR */
        struct {
            AstNode  *object;
            AstNode  *index;
        } index_expr;

        /* AST_FIELD_EXPR */
        struct {
            AstNode  *object;
            char     *field;
        } field_expr;

        /* AST_IDENT_EXPR */
        struct {
            char     *name;
        } ident_expr;

        /* AST_INT_LIT */
        struct {
            int64_t   value;
        } int_lit;

        /* AST_FLOAT_LIT */
        struct {
            double    value;
        } float_lit;

        /* AST_BOOL_LIT */
        struct {
            bool      value;
        } bool_lit;

        /* AST_STRING_LIT */
        struct {
            char     *value;
        } string_lit;

        /* AST_ALLOC_EXPR */
        struct {
            char     *arena_name;
            VtType   *type;
        } alloc_expr;

        /* AST_DEREF_EXPR */
        struct {
            AstNode  *operand;
        } deref_expr;

        /* AST_ADDR_EXPR */
        struct {
            AstNode  *operand;
        } addr_expr;

        /* AST_CAST_EXPR */
        struct {
            AstNode  *operand;
            VtType   *target_type;
        } cast_expr;

        /* AST_SIZEOF_EXPR */
        struct {
            VtType   *type;
        } sizeof_expr;

        /* AST_AGGREGATE_LIT */
        struct {
            VtType        *type;
            AstFieldInit  *fields;
            uint32_t       nfields;
        } aggregate_lit;
    };
};

/* Allocate an AST node in the arena */
static inline AstNode *ast_alloc(Arena *a, AstKind kind, SrcLoc loc) {
    AstNode *n = arena_calloc(a, 1, sizeof(AstNode));
    n->kind = kind;
    n->loc = loc;
    return n;
}

/* ---- AST type helpers ---- */

/* Create a type node in the arena */
VtType *vt_type_make(Arena *a, VtTypeKind kind);
VtType *vt_type_make_ptr(Arena *a, VtType *base);
VtType *vt_type_make_array(Arena *a, VtType *elem, uint64_t count);
VtType *vt_type_make_struct(Arena *a, const char *name,
                             const char **field_names, VtType **field_types, uint32_t nfields);

/* Debug: print an AST node */
void ast_print(AstNode *node, int indent);

#endif /* VORTECH_AST_H */
