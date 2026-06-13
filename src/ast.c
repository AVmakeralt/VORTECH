/*
 * VORTECH Compiler - AST Type Helpers Implementation
 */
#include "ast.h"
#include "lexer.h"
#include <stdio.h>

VtType *vt_type_make(Arena *a, VtTypeKind kind) {
    VtType *t = arena_calloc(a, 1, sizeof(VtType));
    t->kind = kind;
    return t;
}

VtType *vt_type_make_ptr(Arena *a, VtType *base) {
    VtType *t = vt_type_make(a, VTTYPE_PTR);
    t->base = base;
    return t;
}

VtType *vt_type_make_array(Arena *a, VtType *elem, uint64_t count) {
    VtType *t = vt_type_make(a, VTTYPE_ARRAY);
    t->base = elem;
    t->array_count = count;
    return t;
}

VtType *vt_type_make_struct(Arena *a, const char *name,
                             const char **field_names, VtType **field_types, uint32_t nfields) {
    VtType *t = vt_type_make(a, VTTYPE_STRUCT);
    t->name = arena_strdup(a, name);
    t->fields.names = arena_calloc(a, nfields, sizeof(const char *));
    t->fields.types = arena_calloc(a, nfields, sizeof(VtType *));
    t->fields.count = nfields;
    for (uint32_t i = 0; i < nfields; i++) {
        t->fields.names[i] = arena_strdup(a, field_names[i]);
        t->fields.types[i] = field_types[i];
    }
    return t;
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

static const char *type_str(VtType *t) {
    if (!t) return "null";
    switch (t->kind) {
    case VTTYPE_VOID:   return "void";
    case VTTYPE_I8:     return "i8";
    case VTTYPE_I16:    return "i16";
    case VTTYPE_I32:    return "i32";
    case VTTYPE_I64:    return "i64";
    case VTTYPE_U8:     return "u8";
    case VTTYPE_U16:    return "u16";
    case VTTYPE_U32:    return "u32";
    case VTTYPE_U64:    return "u64";
    case VTTYPE_F32:    return "f32";
    case VTTYPE_F64:    return "f64";
    case VTTYPE_BOOL:   return "bool";
    case VTTYPE_PTR:    return "ptr";
    case VTTYPE_STRUCT: return t->name ? t->name : "struct";
    case VTTYPE_ARRAY:  return "array";
    }
    return "?";
}

void ast_print(AstNode *node, int indent) {
    if (!node) { print_indent(indent); printf("(null)\n"); return; }
    print_indent(indent);
    switch (node->kind) {
    case AST_PROGRAM:
        printf("PROGRAM (%u decls)\n", node->program.ndecls);
        for (uint32_t i = 0; i < node->program.ndecls; i++)
            ast_print(node->program.decls[i], indent + 1);
        break;
    case AST_FN_DECL:
        printf("FN_DECL %s(%u params) -> %s%s\n",
               node->fn_decl.name, node->fn_decl.nparams,
               type_str(node->fn_decl.ret_type),
               node->fn_decl.is_extern ? " [extern]" : "");
        for (uint32_t i = 0; i < node->fn_decl.nparams; i++)
            ast_print(NULL, indent + 1); /* params printed inline */
        if (node->fn_decl.body)
            ast_print(node->fn_decl.body, indent + 1);
        break;
    case AST_STRUCT_DECL:
        printf("STRUCT_DECL %s (%u fields)\n",
               node->struct_decl.name, node->struct_decl.nfields);
        break;
    case AST_ARENA_DECL:
        printf("ARENA_DECL %s\n", node->arena_decl.name);
        break;
    case AST_BLOCK:
        printf("BLOCK (%u stmts)\n", node->block.nstmts);
        for (uint32_t i = 0; i < node->block.nstmts; i++)
            ast_print(node->block.stmts[i], indent + 1);
        break;
    case AST_LET_STMT:
        printf("LET %s: %s\n", node->let_stmt.name, type_str(node->let_stmt.type));
        if (node->let_stmt.init) ast_print(node->let_stmt.init, indent + 1);
        break;
    case AST_ASSIGN_STMT:
        printf("ASSIGN\n");
        ast_print(node->assign_stmt.target, indent + 1);
        ast_print(node->assign_stmt.value, indent + 1);
        break;
    case AST_IF_STMT:
        printf("IF\n");
        ast_print(node->if_stmt.condition, indent + 1);
        ast_print(node->if_stmt.then_block, indent + 1);
        if (node->if_stmt.else_block) ast_print(node->if_stmt.else_block, indent + 1);
        break;
    case AST_WHILE_STMT:
        printf("WHILE\n");
        ast_print(node->while_stmt.condition, indent + 1);
        ast_print(node->while_stmt.body, indent + 1);
        break;
    case AST_FOR_STMT:
        printf("FOR\n");
        if (node->for_stmt.init) ast_print(node->for_stmt.init, indent + 1);
        if (node->for_stmt.condition) ast_print(node->for_stmt.condition, indent + 1);
        if (node->for_stmt.update) ast_print(node->for_stmt.update, indent + 1);
        ast_print(node->for_stmt.body, indent + 1);
        break;
    case AST_RETURN_STMT:
        printf("RETURN\n");
        if (node->return_stmt.value) ast_print(node->return_stmt.value, indent + 1);
        break;
    case AST_EXPR_STMT:
        printf("EXPR_STMT\n");
        ast_print(node->expr_stmt.expr, indent + 1);
        break;
    case AST_ARENA_RESET_STMT:
        printf("ARENA_RESET %s\n", node->arena_reset_stmt.name);
        break;
    case AST_BINARY_EXPR:
        printf("BINARY %s\n", token_kind_str(node->binary_expr.op));
        ast_print(node->binary_expr.left, indent + 1);
        ast_print(node->binary_expr.right, indent + 1);
        break;
    case AST_UNARY_EXPR:
        printf("UNARY %s\n", token_kind_str(node->unary_expr.op));
        ast_print(node->unary_expr.operand, indent + 1);
        break;
    case AST_CALL_EXPR:
        printf("CALL %s(%u args)\n", node->call_expr.name, node->call_expr.nargs);
        for (uint32_t i = 0; i < node->call_expr.nargs; i++)
            ast_print(node->call_expr.args[i], indent + 1);
        break;
    case AST_INDEX_EXPR:
        printf("INDEX\n");
        ast_print(node->index_expr.object, indent + 1);
        ast_print(node->index_expr.index, indent + 1);
        break;
    case AST_FIELD_EXPR:
        printf("FIELD .%s\n", node->field_expr.field);
        ast_print(node->field_expr.object, indent + 1);
        break;
    case AST_IDENT_EXPR:
        printf("IDENT %s\n", node->ident_expr.name);
        break;
    case AST_INT_LIT:
        printf("INT_LIT %lld\n", (long long)node->int_lit.value);
        break;
    case AST_FLOAT_LIT:
        printf("FLOAT_LIT %g\n", node->float_lit.value);
        break;
    case AST_BOOL_LIT:
        printf("BOOL_LIT %s\n", node->bool_lit.value ? "true" : "false");
        break;
    case AST_STRING_LIT:
        printf("STRING_LIT \"%s\"\n", node->string_lit.value);
        break;
    case AST_ALLOC_EXPR:
        printf("ALLOC %s, %s\n", node->alloc_expr.arena_name, type_str(node->alloc_expr.type));
        break;
    case AST_DEREF_EXPR:
        printf("DEREF\n");
        ast_print(node->deref_expr.operand, indent + 1);
        break;
    case AST_ADDR_EXPR:
        printf("ADDR\n");
        ast_print(node->addr_expr.operand, indent + 1);
        break;
    case AST_CAST_EXPR:
        printf("CAST -> %s\n", type_str(node->cast_expr.target_type));
        ast_print(node->cast_expr.operand, indent + 1);
        break;
    case AST_SIZEOF_EXPR:
        printf("SIZEOF %s\n", type_str(node->sizeof_expr.type));
        break;
    case AST_AGGREGATE_LIT:
        printf("AGGREGATE_LIT %s (%u fields)\n",
               type_str(node->aggregate_lit.type), node->aggregate_lit.nfields);
        break;
    }
}
