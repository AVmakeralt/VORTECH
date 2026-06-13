/*
 * VORTECH Compiler - High-level Intermediate Representation
 *
 * HIR is a cleaned-up, type-resolved, flattened version of the AST.
 * It serves as the bridge between the parser and SSA construction.
 */
#ifndef VORTECH_HIR_H
#define VORTECH_HIR_H

#include "common.h"
#include "arena.h"
#include "ast.h"

/* ---- HIR Node Kinds ---- */
typedef enum {
    HIR_PROGRAM,
    HIR_FN_DECL,
    HIR_STRUCT_DECL,
    HIR_ARENA_DECL,

    /* Statements */
    HIR_BLOCK,
    HIR_LET_STMT,
    HIR_ASSIGN_STMT,
    HIR_IF_STMT,
    HIR_WHILE_STMT,
    HIR_FOR_STMT,
    HIR_RETURN_STMT,
    HIR_EXPR_STMT,
    HIR_ARENA_RESET_STMT,

    /* Expressions */
    HIR_BINARY_EXPR,
    HIR_UNARY_EXPR,
    HIR_CALL_EXPR,
    HIR_INDEX_EXPR,
    HIR_FIELD_EXPR,
    HIR_IDENT_EXPR,
    HIR_INT_LIT,
    HIR_FLOAT_LIT,
    HIR_BOOL_LIT,
    HIR_STRING_LIT,
    HIR_ALLOC_EXPR,
    HIR_DEREF_EXPR,
    HIR_ADDR_EXPR,
    HIR_CAST_EXPR,
    HIR_SIZEOF_EXPR,
} HirKind;

/* Forward declaration */
typedef struct HirNode HirNode;

/* Function parameter */
typedef struct {
    char    *name;
    VtType  *type;
    uint32_t index;  /* parameter index (0-based) */
} HirParam;

/* Struct field */
typedef struct {
    char    *name;
    VtType  *type;
    uint32_t offset; /* byte offset within struct */
} HirField;

struct HirNode {
    HirKind kind;
    SrcLoc  loc;
    VtType *type;   /* resolved type for all expressions */

    union {
        /* HIR_PROGRAM */
        struct {
            HirNode **decls;
            uint32_t  ndecls;
        } program;

        /* HIR_FN_DECL */
        struct {
            char      *name;
            HirParam  *params;
            uint32_t   nparams;
            VtType    *ret_type;
            HirNode   *body;
            bool       is_extern;
            uint32_t   stack_size; /* calculated later */
        } fn_decl;

        /* HIR_STRUCT_DECL */
        struct {
            char      *name;
            HirField  *fields;
            uint32_t   nfields;
            uint32_t   total_size;
        } struct_decl;

        /* HIR_ARENA_DECL */
        struct {
            char      *name;
        } arena_decl;

        /* HIR_BLOCK */
        struct {
            HirNode  **stmts;
            uint32_t   nstmts;
        } block;

        /* HIR_LET_STMT */
        struct {
            char     *name;
            VtType   *type;
            HirNode  *init;
            uint32_t  local_index; /* local variable index */
        } let_stmt;

        /* HIR_ASSIGN_STMT */
        struct {
            HirNode  *target;
            HirNode  *value;
            TokenKind compound_op;
        } assign_stmt;

        /* HIR_IF_STMT */
        struct {
            HirNode  *condition;
            HirNode  *then_block;
            HirNode  *else_block;
        } if_stmt;

        /* HIR_WHILE_STMT */
        struct {
            HirNode  *condition;
            HirNode  *body;
        } while_stmt;

        /* HIR_FOR_STMT */
        struct {
            HirNode  *init;
            HirNode  *condition;
            HirNode  *update;
            HirNode  *body;
        } for_stmt;

        /* HIR_RETURN_STMT */
        struct {
            HirNode  *value;
        } return_stmt;

        /* HIR_EXPR_STMT */
        struct {
            HirNode  *expr;
        } expr_stmt;

        /* HIR_ARENA_RESET_STMT */
        struct {
            char     *name;
        } arena_reset_stmt;

        /* HIR_BINARY_EXPR */
        struct {
            TokenKind op;
            HirNode  *left;
            HirNode  *right;
        } binary_expr;

        /* HIR_UNARY_EXPR */
        struct {
            TokenKind op;
            HirNode  *operand;
        } unary_expr;

        /* HIR_CALL_EXPR */
        struct {
            char     *name;
            HirNode **args;
            uint32_t  nargs;
        } call_expr;

        /* HIR_INDEX_EXPR */
        struct {
            HirNode  *object;
            HirNode  *index;
        } index_expr;

        /* HIR_FIELD_EXPR */
        struct {
            HirNode  *object;
            char     *field;
            uint32_t  field_offset;
        } field_expr;

        /* HIR_IDENT_EXPR */
        struct {
            char     *name;
            uint32_t  local_index; /* resolved local var index */
            bool      is_param;
            uint32_t  param_index;
        } ident_expr;

        /* HIR_INT_LIT */
        struct {
            int64_t   value;
        } int_lit;

        /* HIR_FLOAT_LIT */
        struct {
            double    value;
        } float_lit;

        /* HIR_BOOL_LIT */
        struct {
            bool      value;
        } bool_lit;

        /* HIR_STRING_LIT */
        struct {
            char     *value;
            uint32_t  length;
        } string_lit;

        /* HIR_ALLOC_EXPR */
        struct {
            char     *arena_name;
            VtType   *type;
            uint32_t  alloc_size;
        } alloc_expr;

        /* HIR_DEREF_EXPR */
        struct {
            HirNode  *operand;
        } deref_expr;

        /* HIR_ADDR_EXPR */
        struct {
            HirNode  *operand;
        } addr_expr;

        /* HIR_CAST_EXPR */
        struct {
            HirNode  *operand;
            VtType   *target_type;
        } cast_expr;

        /* HIR_SIZEOF_EXPR */
        struct {
            VtType   *type;
            uint32_t  size;
        } sizeof_expr;
    };
};

/* Allocate a HIR node */
static inline HirNode *hir_alloc(Arena *a, HirKind kind, SrcLoc loc) {
    HirNode *n = arena_calloc(a, 1, sizeof(HirNode));
    n->kind = kind;
    n->loc = loc;
    return n;
}

/* Build HIR from AST (includes type checking and resolution) */
HirNode *hir_build(Arena *arena, AstNode *ast);

/* Debug print */
void hir_print(HirNode *node, int indent);

#endif /* VORTECH_HIR_H */
