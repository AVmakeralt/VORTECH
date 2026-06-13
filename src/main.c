/*
 * VORTECH Compiler - Main Driver
 *
 * Maximum practical performance with minimum compiler complexity.
 *
 * Pipeline:
 *   Source -> Lexer -> Parser -> AST -> HIR -> SSA -> Optimizations
 *   -> Instruction Selection -> Register Allocation -> Peephole -> Emit
 *
 * The backend generates x86-64 machine code directly into ELF64
 * object files. No external assembler. No GCC for code generation.
 * The system linker (ld) is used only for linking with C runtime.
 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "arena.h"
#include "diag.h"
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "hir.h"
#include "ssa.h"
#include "opt.h"
#include "isel.h"
#include "regalloc.h"
#include "emit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration for peephole */
extern void peephole_run(MachFunc *mf);

/* Compiler configuration */
typedef struct {
    bool        emit_ast;
    bool        emit_hir;
    bool        emit_ssa;
    bool        emit_mach;
    bool        no_optimize;
    bool        no_verify;
    const char *input_file;
    const char *output_file;
    int         opt_level;    /* 0 = none, 1 = basic, 2 = all */
} VortechConfig;

static void print_usage(const char *prog) {
    printf("VORTECH Compiler - Maximum practical performance with minimum compiler complexity\n");
    printf("\n");
    printf("Usage: %s [options] <input.vt>\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -o <file>     Output file (default: a.out)\n");
    printf("  -O0           No optimization\n");
    printf("  -O1           Basic optimizations (const fold, DCE, copy prop)\n");
    printf("  -O2           All optimizations (default)\n");
    printf("  --emit-ast    Print AST and exit\n");
    printf("  --emit-hir    Print HIR and exit\n");
    printf("  --emit-ssa    Print SSA IR and exit\n");
    printf("  --emit-mach   Print machine IR and exit\n");
    printf("  --no-verify   Skip SSA verification\n");
    printf("  -h, --help    Show this help\n");
    printf("\n");
    printf("Native x86-64 backend. Emits ELF64 object files directly.\n");
    printf("Uses system linker (ld) only for linking with C runtime.\n");
}

static VortechConfig parse_args(int argc, char **argv) {
    VortechConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.output_file = "a.out";
    cfg.opt_level = 2;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            cfg.output_file = argv[++i];
        } else if (strcmp(argv[i], "-O0") == 0) {
            cfg.opt_level = 0;
        } else if (strcmp(argv[i], "-O1") == 0) {
            cfg.opt_level = 1;
        } else if (strcmp(argv[i], "-O2") == 0) {
            cfg.opt_level = 2;
        } else if (strcmp(argv[i], "--emit-ast") == 0) {
            cfg.emit_ast = true;
        } else if (strcmp(argv[i], "--emit-hir") == 0) {
            cfg.emit_hir = true;
        } else if (strcmp(argv[i], "--emit-ssa") == 0) {
            cfg.emit_ssa = true;
        } else if (strcmp(argv[i], "--emit-mach") == 0) {
            cfg.emit_mach = true;
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            cfg.no_verify = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "vortech: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        } else {
            cfg.input_file = argv[i];
        }
    }

    if (!cfg.input_file) {
        fprintf(stderr, "vortech: no input file\n");
        print_usage(argv[0]);
        exit(1);
    }

    return cfg;
}

/* Read a file into a string */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "vortech: cannot open '%s'\n", path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fprintf(stderr, "vortech: out of memory reading '%s'\n", path);
        exit(1);
    }

    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* Find the path to a crt file for the system linker */
static char crt1_path[512];
static char crti_path[512];
static char crtn_path[512];

static const char *find_crt_file(const char *name, char *buf, size_t bufsize) {
    /* Common locations for crt files on Linux */
    static const char *search_paths[] = {
        "/usr/lib/x86_64-linux-gnu/",
        "/usr/lib64/",
        "/usr/lib/",
        "/lib/x86_64-linux-gnu/",
        "/lib64/",
        NULL
    };

    for (int i = 0; search_paths[i]; i++) {
        snprintf(buf, bufsize, "%s%s", search_paths[i], name);
        FILE *f = fopen(buf, "r");
        if (f) {
            fclose(f);
            return buf;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    VortechConfig cfg = parse_args(argc, argv);

    /* Read source */
    char *source = read_file(cfg.input_file);

    /* Create arenas for each compilation stage */
    Arena *ast_arena = arena_create(VT_ARENA_PAGE_SIZE);
    Arena *hir_arena = arena_create(VT_ARENA_PAGE_SIZE);
    Arena *ssa_arena = arena_create(VT_ARENA_PAGE_SIZE);
    Arena *mach_arena = arena_create(VT_ARENA_PAGE_SIZE);

    /* ---- Stage 1: Lexing & Parsing ---- */
    Lexer lexer;
    lexer_init(&lexer, source, cfg.input_file, ast_arena);

    Parser parser;
    parser.lex = &lexer;
    parser.arena = ast_arena;

    AstNode *ast = parser_parse(&parser);

    if (diag_error_count() > 0) {
        fprintf(stderr, "vortech: %d error(s) during parsing\n", diag_error_count());
        arena_destroy(ast_arena);
        free(source);
        return 1;
    }

    if (cfg.emit_ast) {
        printf("=== AST ===\n");
        ast_print(ast, 0);
        arena_destroy(ast_arena);
        free(source);
        return 0;
    }

    /* ---- Stage 2: HIR Construction ---- */
    HirNode *hir = hir_build(hir_arena, ast);

    /* AST is no longer needed - reclaim memory */
    arena_destroy(ast_arena);

    if (diag_error_count() > 0) {
        fprintf(stderr, "vortech: %d error(s) during HIR construction\n", diag_error_count());
        free(source);
        return 1;
    }

    if (cfg.emit_hir) {
        printf("=== HIR ===\n");
        hir_print(hir, 0);
        arena_destroy(hir_arena);
        free(source);
        return 0;
    }

    /* ---- Stage 3: SSA Construction ---- */
    SsaProgram *ssa_prog = ssa_build_program(ssa_arena, hir);

    /* HIR is no longer needed */
    arena_destroy(hir_arena);

    if (!cfg.no_verify) {
        if (!ssa_verify_program(ssa_prog)) {
            fprintf(stderr, "vortech: SSA verification failed after construction\n");
            return 1;
        }
    }

    if (cfg.emit_ssa) {
        printf("=== SSA IR ===\n");
        ssa_print_program(ssa_prog);
        arena_destroy(ssa_arena);
        free(source);
        return 0;
    }

    /* ---- Stage 4: Optimization ---- */
    for (uint32_t i = 0; i < ssa_prog->nfuncs; i++) {
        if (cfg.opt_level >= 1) {
            opt_const_fold(&ssa_prog->funcs[i]);
            opt_dce(&ssa_prog->funcs[i]);
            opt_copy_prop(&ssa_prog->funcs[i]);
        }

        if (cfg.opt_level >= 2) {
            opt_sccp(&ssa_prog->funcs[i]);
            opt_cse(&ssa_prog->funcs[i]);
            opt_dce(&ssa_prog->funcs[i]);
            opt_licm(&ssa_prog->funcs[i]);
        }

        if (!cfg.no_verify) {
            if (!ssa_verify_func(&ssa_prog->funcs[i])) {
                fprintf(stderr, "vortech: SSA verification failed after optimization for '%s'\n",
                        ssa_prog->funcs[i].name);
                return 1;
            }
        }
    }

    /* ---- Stage 5: Instruction Selection ---- */
    MachFunc *mach_funcs = calloc(ssa_prog->nfuncs, sizeof(MachFunc));
    for (uint32_t i = 0; i < ssa_prog->nfuncs; i++) {
        MachFunc *mf = isel_select(mach_arena, &ssa_prog->funcs[i]);
        mach_funcs[i] = *mf;
    }

    /* SSA is no longer needed */
    arena_destroy(ssa_arena);

    if (cfg.emit_mach) {
        printf("=== Machine IR ===\n");
        for (uint32_t i = 0; i < ssa_prog->nfuncs; i++) {
            mach_func_print(&mach_funcs[i]);
        }
        free(mach_funcs);
        free(source);
        return 0;
    }

    /* ---- Stage 6: Register Allocation ---- */
    RegAllocResult **ra_results = calloc(ssa_prog->nfuncs, sizeof(RegAllocResult *));
    for (uint32_t i = 0; i < ssa_prog->nfuncs; i++) {
        ra_results[i] = regalloc_linear_scan(&mach_funcs[i], mach_arena);
        regalloc_apply(&mach_funcs[i], ra_results[i]);
    }

    /* ---- Stage 7: Peephole Optimization ---- */
    for (uint32_t i = 0; i < ssa_prog->nfuncs; i++) {
        peephole_run(&mach_funcs[i]);
    }

    /* ---- Stage 8: Native Code Emission ---- */
    /* Emit ELF64 object file directly - no external assembler */
    char *obj_file = NULL;
    {
        size_t len = strlen(cfg.output_file) + 4;
        obj_file = malloc(len);
        snprintf(obj_file, len, "%s.o", cfg.output_file);
    }

    if (!emit_object(obj_file, mach_funcs, ssa_prog->nfuncs, ra_results)) {
        fprintf(stderr, "vortech: native code emission failed\n");
        free(obj_file);
        free(mach_funcs);
        free(source);
        return 1;
    }

    /* ---- Stage 9: Linking ---- */
    /* Use system linker to create the final executable.
     * We link against libc for printf (used by the print builtin).
     * GCC is used only as a linker driver here - it does NOT
     * generate any code. All machine code is produced by our
     * own x86-64 binary emitter. */
    char cmd[2048];
    const char *output = cfg.output_file ? cfg.output_file : "a.out";

    /* Try using ld directly with crt files first */
    const char *crt1 = find_crt_file("crt1.o", crt1_path, sizeof(crt1_path));
    const char *crti = find_crt_file("crti.o", crti_path, sizeof(crti_path));
    const char *crtn = find_crt_file("crtn.o", crtn_path, sizeof(crtn_path));

    if (crt1 && crti && crtn) {
        /* Use ld directly - no GCC involved at all */
        snprintf(cmd, sizeof(cmd),
                 "ld -dynamic-linker /lib64/ld-linux-x86-64.so.2 "
                 "%s %s %s -lc %s -o %s",
                 crt1, crti, obj_file, crtn, output);
    } else {
        /* Fallback: use gcc as linker driver only (no code generation) */
        snprintf(cmd, sizeof(cmd),
                 "gcc -no-pie %s -lc -o %s",
                 obj_file, output);
    }

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "vortech: linking failed (exit code %d)\n", rc);
        fprintf(stderr, "  Object file: %s\n", obj_file);
        fprintf(stderr, "  Try manually: ld %s -lc -o %s\n", obj_file, output);
    } else {
        fprintf(stderr, "vortech: compiled %s -> %s (native x86-64)\n",
                cfg.input_file, output);
        fprintf(stderr, "  Object file: %s\n", obj_file);
    }

    /* Cleanup */
    for (uint32_t i = 0; i < ssa_prog->nfuncs; i++) {
        regalloc_free(ra_results[i]);
    }
    free(ra_results);
    free(mach_funcs);
    arena_destroy(mach_arena);
    free(obj_file);
    free(source);

    return rc;
}
