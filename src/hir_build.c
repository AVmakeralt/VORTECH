/*
 * VORTECH Compiler - HIR Builder
 *
 * Converts AST to HIR with type resolution and variable numbering.
 */
#include "hir.h"
#include "lexer.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

/* ---- Scope for variable resolution ---- */
typedef struct ScopeEntry {
    char     *name;
    VtType   *type;
    uint32_t  local_index;
    bool      is_param;
    uint32_t  param_index;
    struct ScopeEntry *next;
} ScopeEntry;

typedef struct Scope {
    ScopeEntry    *entries;
    struct Scope  *parent;
    uint32_t      *next_local;   /* pointer to next local counter */
} Scope;

typedef struct {
    Arena      *arena;
    Scope      *current_scope;
    uint32_t    next_local;     /* next local variable index */
    uint32_t    next_param;     /* next parameter index */
    /* Struct type registry for type resolution */
    HirNode   **struct_decls;
    uint32_t    nstruct_decls;
    uint32_t    struct_decls_cap;
} HirContext;

static Scope *scope_push(HirContext *ctx) {
    Scope *s = arena_calloc(ctx->arena, 1, sizeof(Scope));
    s->parent = ctx->current_scope;
    s->next_local = &ctx->next_local;
    ctx->current_scope = s;
    return s;
}

static void scope_pop(HirContext *ctx) {
    if (ctx->current_scope) {
        ctx->current_scope = ctx->current_scope->parent;
    }
}

static void scope_declare(HirContext *ctx, const char *name, VtType *type,
                          bool is_param, uint32_t param_index) {
    ScopeEntry *e = arena_calloc(ctx->arena, 1, sizeof(ScopeEntry));
    e->name = arena_strdup(ctx->arena, name);
    e->type = type;
    e->is_param = is_param;
    e->param_index = param_index;
    e->local_index = ctx->next_local++;

    e->next = ctx->current_scope->entries;
    ctx->current_scope->entries = e;
}

static ScopeEntry *scope_lookup(Scope *scope, const char *name) {
    for (ScopeEntry *e = scope->entries; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    if (scope->parent) return scope_lookup(scope->parent, name);
    return NULL;
}

/* ---- Type inference helpers ---- */

static VtType *infer_binary_type(VtType *left, VtType *right, SrcLoc loc) {
    if (!left || !right) return vt_type_make(NULL, VTTYPE_I32);

    /* Pointer arithmetic */
    if (left->kind == VTTYPE_PTR && vt_type_is_numeric(right)) return left;
    if (right->kind == VTTYPE_PTR && vt_type_is_numeric(left)) return right;

    /* Numeric promotion */
    if (vt_type_is_float(left) || vt_type_is_float(right)) {
        if (left->kind == VTTYPE_F64 || right->kind == VTTYPE_F64)
            return vt_type_make(NULL, VTTYPE_F64);
        return vt_type_make(NULL, VTTYPE_F32);
    }

    /* Integer promotion: use the larger type */
    uint32_t ls = vt_type_size(left);
    uint32_t rs = vt_type_size(right);
    if (ls >= rs) return left;
    return right;
}

static VtType *infer_unary_type(TokenKind op, VtType *operand) {
    if (!operand) return vt_type_make(NULL, VTTYPE_I32);
    switch (op) {
    case TOK_BANG:
        return vt_type_make(NULL, VTTYPE_BOOL);
    case TOK_MINUS:
    case TOK_TILDE:
        return operand;
    default:
        return operand;
    }
}

/* ---- AST to HIR conversion ---- */

static HirNode *hir_convert_expr(HirContext *ctx, AstNode *ast);
static HirNode *hir_convert_stmt(HirContext *ctx, AstNode *ast);

static HirNode *hir_convert_expr(HirContext *ctx, AstNode *ast) {
    if (!ast) return NULL;

    switch (ast->kind) {
    case AST_INT_LIT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_INT_LIT, ast->loc);
        n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        n->int_lit.value = ast->int_lit.value;
        return n;
    }
    case AST_FLOAT_LIT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_FLOAT_LIT, ast->loc);
        n->type = vt_type_make(ctx->arena, VTTYPE_F64);
        n->float_lit.value = ast->float_lit.value;
        return n;
    }
    case AST_BOOL_LIT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_BOOL_LIT, ast->loc);
        n->type = vt_type_make(ctx->arena, VTTYPE_BOOL);
        n->bool_lit.value = ast->bool_lit.value;
        return n;
    }
    case AST_STRING_LIT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_STRING_LIT, ast->loc);
        n->type = vt_type_make_ptr(ctx->arena, vt_type_make(ctx->arena, VTTYPE_U8));
        n->string_lit.value = arena_strdup(ctx->arena, ast->string_lit.value);
        n->string_lit.length = (uint32_t)strlen(ast->string_lit.value);
        return n;
    }
    case AST_IDENT_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_IDENT_EXPR, ast->loc);
        n->ident_expr.name = arena_strdup(ctx->arena, ast->ident_expr.name);

        ScopeEntry *e = scope_lookup(ctx->current_scope, ast->ident_expr.name);
        if (e) {
            n->type = e->type;
            n->ident_expr.local_index = e->local_index;
            n->ident_expr.is_param = e->is_param;
            n->ident_expr.param_index = e->param_index;
        } else {
            diag_report(DIAG_ERROR, ast->loc, "undefined variable '%s'", ast->ident_expr.name);
            n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        }
        return n;
    }
    case AST_BINARY_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_BINARY_EXPR, ast->loc);
        n->binary_expr.op = ast->binary_expr.op;
        n->binary_expr.left = hir_convert_expr(ctx, ast->binary_expr.left);
        n->binary_expr.right = hir_convert_expr(ctx, ast->binary_expr.right);

        VtType *lt = n->binary_expr.left ? n->binary_expr.left->type : NULL;
        VtType *rt = n->binary_expr.right ? n->binary_expr.right->type : NULL;

        /* Comparison operators return bool */
        switch (ast->binary_expr.op) {
        case TOK_EQEQ: case TOK_NEQ:
        case TOK_LT:   case TOK_GT:
        case TOK_LEQ:  case TOK_GEQ:
            n->type = vt_type_make(ctx->arena, VTTYPE_BOOL);
            break;
        case TOK_AMPAMP:
        case TOK_PIPEPIPE:
            n->type = vt_type_make(ctx->arena, VTTYPE_BOOL);
            break;
        default:
            n->type = infer_binary_type(lt, rt, ast->loc);
            break;
        }
        return n;
    }
    case AST_UNARY_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_UNARY_EXPR, ast->loc);
        n->unary_expr.op = ast->unary_expr.op;
        n->unary_expr.operand = hir_convert_expr(ctx, ast->unary_expr.operand);
        n->type = infer_unary_type(ast->unary_expr.op,
                                   n->unary_expr.operand ? n->unary_expr.operand->type : NULL);
        return n;
    }
    case AST_CALL_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_CALL_EXPR, ast->loc);
        n->call_expr.name = arena_strdup(ctx->arena, ast->call_expr.name);
        n->call_expr.nargs = ast->call_expr.nargs;
        n->call_expr.args = arena_calloc(ctx->arena, ast->call_expr.nargs, sizeof(HirNode *));

        for (uint32_t i = 0; i < ast->call_expr.nargs; i++) {
            n->call_expr.args[i] = hir_convert_expr(ctx, ast->call_expr.args[i]);
        }

        /* Default return type is i32 for unknown functions, void for print */
        if (strcmp(ast->call_expr.name, "print") == 0) {
            n->type = vt_type_make(ctx->arena, VTTYPE_VOID);
        } else {
            n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        }
        return n;
    }
    case AST_INDEX_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_INDEX_EXPR, ast->loc);
        n->index_expr.object = hir_convert_expr(ctx, ast->index_expr.object);
        n->index_expr.index = hir_convert_expr(ctx, ast->index_expr.index);

        if (n->index_expr.object && n->index_expr.object->type) {
            VtType *obj_type = n->index_expr.object->type;
            if (obj_type->kind == VTTYPE_PTR || obj_type->kind == VTTYPE_ARRAY) {
                n->type = obj_type->base ? obj_type->base : vt_type_make(ctx->arena, VTTYPE_I32);
            } else {
                n->type = vt_type_make(ctx->arena, VTTYPE_I32);
            }
        } else {
            n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        }
        return n;
    }
    case AST_FIELD_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_FIELD_EXPR, ast->loc);
        n->field_expr.object = hir_convert_expr(ctx, ast->field_expr.object);
        n->field_expr.field = arena_strdup(ctx->arena, ast->field_expr.field);
        n->field_expr.field_offset = 0;

        /* Resolve field type and offset */
        if (n->field_expr.object && n->field_expr.object->type) {
            VtType *obj_type = n->field_expr.object->type;
            if (obj_type->kind == VTTYPE_PTR) obj_type = obj_type->base;
            if (obj_type && obj_type->kind == VTTYPE_STRUCT) {
                uint32_t offset = 0;
                for (uint32_t i = 0; i < obj_type->fields.count; i++) {
                    if (strcmp(obj_type->fields.names[i], ast->field_expr.field) == 0) {
                        n->field_expr.field_offset = offset;
                        n->type = obj_type->fields.types[i];
                        break;
                    }
                    offset += vt_type_size(obj_type->fields.types[i]);
                    offset = (offset + 3) & ~3u; /* align */
                }
                if (!n->type) {
                    diag_report(DIAG_ERROR, ast->loc,
                                "struct '%s' has no field '%s'",
                                obj_type->name ? obj_type->name : "?",
                                ast->field_expr.field);
                    n->type = vt_type_make(ctx->arena, VTTYPE_I32);
                }
            } else {
                n->type = vt_type_make(ctx->arena, VTTYPE_I32);
            }
        } else {
            n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        }
        return n;
    }
    case AST_ALLOC_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_ALLOC_EXPR, ast->loc);
        n->alloc_expr.arena_name = arena_strdup(ctx->arena, ast->alloc_expr.arena_name);
        n->alloc_expr.type = ast->alloc_expr.type;
        n->alloc_expr.alloc_size = vt_type_size(ast->alloc_expr.type);
        n->type = vt_type_make_ptr(ctx->arena, ast->alloc_expr.type);
        return n;
    }
    case AST_DEREF_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_DEREF_EXPR, ast->loc);
        n->deref_expr.operand = hir_convert_expr(ctx, ast->deref_expr.operand);
        if (n->deref_expr.operand && n->deref_expr.operand->type &&
            n->deref_expr.operand->type->kind == VTTYPE_PTR) {
            n->type = n->deref_expr.operand->type->base;
        } else {
            diag_report(DIAG_ERROR, ast->loc, "cannot dereference non-pointer type");
            n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        }
        return n;
    }
    case AST_ADDR_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_ADDR_EXPR, ast->loc);
        n->addr_expr.operand = hir_convert_expr(ctx, ast->addr_expr.operand);
        if (n->addr_expr.operand) {
            n->type = vt_type_make_ptr(ctx->arena, n->addr_expr.operand->type);
        } else {
            n->type = vt_type_make_ptr(ctx->arena, vt_type_make(ctx->arena, VTTYPE_VOID));
        }
        return n;
    }
    case AST_CAST_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_CAST_EXPR, ast->loc);
        n->cast_expr.operand = hir_convert_expr(ctx, ast->cast_expr.operand);
        n->cast_expr.target_type = ast->cast_expr.target_type;
        n->type = ast->cast_expr.target_type;
        return n;
    }
    case AST_SIZEOF_EXPR: {
        HirNode *n = hir_alloc(ctx->arena, HIR_SIZEOF_EXPR, ast->loc);
        n->sizeof_expr.type = ast->sizeof_expr.type;
        n->sizeof_expr.size = vt_type_size(ast->sizeof_expr.type);
        n->type = vt_type_make(ctx->arena, VTTYPE_U64);
        return n;
    }
    default:
        diag_report(DIAG_ERROR, ast->loc, "expected expression in HIR conversion");
        HirNode *n = hir_alloc(ctx->arena, HIR_INT_LIT, ast->loc);
        n->type = vt_type_make(ctx->arena, VTTYPE_I32);
        n->int_lit.value = 0;
        return n;
    }
}

static HirNode *hir_convert_stmt(HirContext *ctx, AstNode *ast) {
    if (!ast) return NULL;

    switch (ast->kind) {
    case AST_BLOCK: {
        HirNode *n = hir_alloc(ctx->arena, HIR_BLOCK, ast->loc);
        n->block.nstmts = ast->block.nstmts;
        n->block.stmts = arena_calloc(ctx->arena, ast->block.nstmts, sizeof(HirNode *));
        scope_push(ctx);
        for (uint32_t i = 0; i < ast->block.nstmts; i++) {
            n->block.stmts[i] = hir_convert_stmt(ctx, ast->block.stmts[i]);
        }
        scope_pop(ctx);
        return n;
    }
    case AST_LET_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_LET_STMT, ast->loc);
        n->let_stmt.name = arena_strdup(ctx->arena, ast->let_stmt.name);
        n->let_stmt.type = ast->let_stmt.type ? ast->let_stmt.type : vt_type_make(ctx->arena, VTTYPE_I32);

        if (ast->let_stmt.init) {
            n->let_stmt.init = hir_convert_expr(ctx, ast->let_stmt.init);
            /* Infer type from init if not specified */
            if (!ast->let_stmt.type && n->let_stmt.init) {
                n->let_stmt.type = n->let_stmt.init->type;
            }
        }

        /* Declare variable in scope */
        bool is_param = false;
        uint32_t param_index = 0;
        scope_declare(ctx, ast->let_stmt.name, n->let_stmt.type, is_param, param_index);

        /* Set the local index from the scope */
        ScopeEntry *e = scope_lookup(ctx->current_scope, ast->let_stmt.name);
        if (e) n->let_stmt.local_index = e->local_index;

        n->type = n->let_stmt.type;
        return n;
    }
    case AST_ASSIGN_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_ASSIGN_STMT, ast->loc);
        n->assign_stmt.target = hir_convert_expr(ctx, ast->assign_stmt.target);
        n->assign_stmt.value = hir_convert_expr(ctx, ast->assign_stmt.value);
        n->assign_stmt.compound_op = ast->assign_stmt.compound_op;
        if (n->assign_stmt.target) n->type = n->assign_stmt.target->type;
        return n;
    }
    case AST_IF_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_IF_STMT, ast->loc);
        n->if_stmt.condition = hir_convert_expr(ctx, ast->if_stmt.condition);
        n->if_stmt.then_block = hir_convert_stmt(ctx, ast->if_stmt.then_block);
        n->if_stmt.else_block = hir_convert_stmt(ctx, ast->if_stmt.else_block);
        return n;
    }
    case AST_WHILE_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_WHILE_STMT, ast->loc);
        n->while_stmt.condition = hir_convert_expr(ctx, ast->while_stmt.condition);
        n->while_stmt.body = hir_convert_stmt(ctx, ast->while_stmt.body);
        return n;
    }
    case AST_FOR_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_FOR_STMT, ast->loc);
        scope_push(ctx);
        n->for_stmt.init = hir_convert_stmt(ctx, ast->for_stmt.init);
        n->for_stmt.condition = hir_convert_expr(ctx, ast->for_stmt.condition);
        n->for_stmt.update = hir_convert_expr(ctx, ast->for_stmt.update);
        n->for_stmt.body = hir_convert_stmt(ctx, ast->for_stmt.body);
        scope_pop(ctx);
        return n;
    }
    case AST_RETURN_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_RETURN_STMT, ast->loc);
        n->return_stmt.value = hir_convert_expr(ctx, ast->return_stmt.value);
        return n;
    }
    case AST_EXPR_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_EXPR_STMT, ast->loc);
        n->expr_stmt.expr = hir_convert_expr(ctx, ast->expr_stmt.expr);
        return n;
    }
    case AST_ARENA_RESET_STMT: {
        HirNode *n = hir_alloc(ctx->arena, HIR_ARENA_RESET_STMT, ast->loc);
        n->arena_reset_stmt.name = arena_strdup(ctx->arena, ast->arena_reset_stmt.name);
        return n;
    }
    default:
        /* Treat as expression statement */
        return hir_convert_expr(ctx, ast);
    }
}

/* Build HIR from an AST program */
HirNode *hir_build(Arena *arena, AstNode *ast) {
    if (!ast || ast->kind != AST_PROGRAM) {
        diag_msg(DIAG_FATAL, "expected AST_PROGRAM node");
    }

    HirContext ctx;
    ctx.arena = arena;
    ctx.current_scope = NULL;
    ctx.next_local = 0;
    ctx.next_param = 0;
    ctx.struct_decls = NULL;
    ctx.nstruct_decls = 0;
    ctx.struct_decls_cap = 0;

    HirNode *prog = hir_alloc(arena, HIR_PROGRAM, ast->loc);
    prog->program.ndecls = ast->program.ndecls;
    prog->program.decls = arena_calloc(arena, ast->program.ndecls, sizeof(HirNode *));

    /* First pass: register struct types */
    for (uint32_t i = 0; i < ast->program.ndecls; i++) {
        if (ast->program.decls[i]->kind == AST_STRUCT_DECL) {
            AstNode *sd = ast->program.decls[i];
            vt_da_push(ctx.struct_decls, ctx.nstruct_decls, ctx.struct_decls_cap, NULL);
        }
    }

    /* Second pass: convert declarations */
    for (uint32_t i = 0; i < ast->program.ndecls; i++) {
        AstNode *decl = ast->program.decls[i];

        switch (decl->kind) {
        case AST_FN_DECL: {
            HirNode *n = hir_alloc(arena, HIR_FN_DECL, decl->loc);
            n->fn_decl.name = arena_strdup(arena, decl->fn_decl.name);
            n->fn_decl.ret_type = decl->fn_decl.ret_type;
            n->fn_decl.is_extern = decl->fn_decl.is_extern;
            n->fn_decl.stack_size = 0;

            /* Set up scope for function body */
            scope_push(&ctx);
            ctx.next_local = 0;

            /* Parameters */
            n->fn_decl.nparams = decl->fn_decl.nparams;
            n->fn_decl.params = arena_calloc(arena, decl->fn_decl.nparams, sizeof(HirParam));

            for (uint32_t j = 0; j < decl->fn_decl.nparams; j++) {
                n->fn_decl.params[j].name = arena_strdup(arena, decl->fn_decl.params[j].name);
                n->fn_decl.params[j].type = decl->fn_decl.params[j].type;
                n->fn_decl.params[j].index = j;

                /* Declare parameter in scope */
                scope_declare(&ctx, decl->fn_decl.params[j].name,
                             decl->fn_decl.params[j].type, true, j);
            }

            /* Body */
            if (decl->fn_decl.body) {
                n->fn_decl.body = hir_convert_stmt(&ctx, decl->fn_decl.body);
            }

            n->fn_decl.stack_size = ctx.next_local * 8; /* 8 bytes per local */
            scope_pop(&ctx);

            prog->program.decls[i] = n;
            break;
        }
        case AST_STRUCT_DECL: {
            HirNode *n = hir_alloc(arena, HIR_STRUCT_DECL, decl->loc);
            n->struct_decl.name = arena_strdup(arena, decl->struct_decl.name);
            n->struct_decl.nfields = decl->struct_decl.nfields;
            n->struct_decl.fields = arena_calloc(arena, decl->struct_decl.nfields, sizeof(HirField));

            uint32_t offset = 0;
            for (uint32_t j = 0; j < decl->struct_decl.nfields; j++) {
                n->struct_decl.fields[j].name = arena_strdup(arena, decl->struct_decl.fields[j].name);
                n->struct_decl.fields[j].type = decl->struct_decl.fields[j].type;
                n->struct_decl.fields[j].offset = offset;
                offset += vt_type_size(decl->struct_decl.fields[j].type);
                offset = (offset + 3) & ~3u; /* 4-byte align */
            }
            n->struct_decl.total_size = offset;

            prog->program.decls[i] = n;
            break;
        }
        case AST_ARENA_DECL: {
            HirNode *n = hir_alloc(arena, HIR_ARENA_DECL, decl->loc);
            n->arena_decl.name = arena_strdup(arena, decl->arena_decl.name);
            prog->program.decls[i] = n;
            break;
        }
        default:
            diag_report(DIAG_ERROR, decl->loc, "unexpected top-level AST node kind");
            break;
        }
    }

    return prog;
}

/* Debug print */
static void hir_print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

static const char *hir_type_str(VtType *t) {
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

void hir_print(HirNode *node, int indent) {
    if (!node) { hir_print_indent(indent); printf("(null)\n"); return; }
    hir_print_indent(indent);

    switch (node->kind) {
    case HIR_PROGRAM:
        printf("PROGRAM (%u decls)\n", node->program.ndecls);
        for (uint32_t i = 0; i < node->program.ndecls; i++)
            hir_print(node->program.decls[i], indent + 1);
        break;
    case HIR_FN_DECL:
        printf("FN %s(%u params) -> %s [stack=%u]%s\n",
               node->fn_decl.name, node->fn_decl.nparams,
               hir_type_str(node->fn_decl.ret_type),
               node->fn_decl.stack_size,
               node->fn_decl.is_extern ? " [extern]" : "");
        if (node->fn_decl.body) hir_print(node->fn_decl.body, indent + 1);
        break;
    case HIR_STRUCT_DECL:
        printf("STRUCT %s (%u fields, %u bytes)\n",
               node->struct_decl.name, node->struct_decl.nfields,
               node->struct_decl.total_size);
        break;
    case HIR_ARENA_DECL:
        printf("ARENA %s\n", node->arena_decl.name);
        break;
    case HIR_BLOCK:
        printf("BLOCK (%u stmts)\n", node->block.nstmts);
        for (uint32_t i = 0; i < node->block.nstmts; i++)
            hir_print(node->block.stmts[i], indent + 1);
        break;
    case HIR_LET_STMT:
        printf("LET %s: %s [local=%u]\n",
               node->let_stmt.name, hir_type_str(node->let_stmt.type),
               node->let_stmt.local_index);
        if (node->let_stmt.init) hir_print(node->let_stmt.init, indent + 1);
        break;
    case HIR_ASSIGN_STMT:
        printf("ASSIGN\n");
        hir_print(node->assign_stmt.target, indent + 1);
        hir_print(node->assign_stmt.value, indent + 1);
        break;
    case HIR_INT_LIT:
        printf("INT_LIT %lld : %s\n", (long long)node->int_lit.value, hir_type_str(node->type));
        break;
    case HIR_FLOAT_LIT:
        printf("FLOAT_LIT %g : %s\n", node->float_lit.value, hir_type_str(node->type));
        break;
    case HIR_BOOL_LIT:
        printf("BOOL_LIT %s\n", node->bool_lit.value ? "true" : "false");
        break;
    case HIR_IDENT_EXPR:
        printf("IDENT %s [local=%u param=%s:%u] : %s\n",
               node->ident_expr.name, node->ident_expr.local_index,
               node->ident_expr.is_param ? "yes" : "no", node->ident_expr.param_index,
               hir_type_str(node->type));
        break;
    case HIR_BINARY_EXPR:
        printf("BINOP %s : %s\n", token_kind_str(node->binary_expr.op), hir_type_str(node->type));
        hir_print(node->binary_expr.left, indent + 1);
        hir_print(node->binary_expr.right, indent + 1);
        break;
    case HIR_CALL_EXPR:
        printf("CALL %s(%u args) : %s\n", node->call_expr.name, node->call_expr.nargs,
               hir_type_str(node->type));
        for (uint32_t i = 0; i < node->call_expr.nargs; i++)
            hir_print(node->call_expr.args[i], indent + 1);
        break;
    case HIR_RETURN_STMT:
        printf("RETURN\n");
        if (node->return_stmt.value) hir_print(node->return_stmt.value, indent + 1);
        break;
    default:
        printf("HIR node kind %d\n", node->kind);
        break;
    }
}
