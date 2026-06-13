/*
 * VORTECH Compiler - Diagnostics Implementation
 */
#include "diag.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static int error_count = 0;

static const char *level_str(DiagLevel level) {
    switch (level) {
    case DIAG_NOTE:  return "note";
    case DIAG_WARN:  return "warning";
    case DIAG_ERROR: return "error";
    case DIAG_FATAL: return "fatal error";
    }
    return "unknown";
}

void diag_report(DiagLevel level, SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    if (loc.filename) {
        fprintf(stderr, "%s:%u:%u: ", loc.filename, loc.line, loc.col);
    }
    fprintf(stderr, "%s: ", level_str(level));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    if (level == DIAG_ERROR || level == DIAG_FATAL) {
        error_count++;
    }
    if (level == DIAG_FATAL) {
        exit(1);
    }
}

void diag_msg(DiagLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    fprintf(stderr, "%s: ", level_str(level));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    if (level == DIAG_ERROR || level == DIAG_FATAL) {
        error_count++;
    }
    if (level == DIAG_FATAL) {
        exit(1);
    }
}

int diag_error_count(void) {
    return error_count;
}

void diag_reset(void) {
    error_count = 0;
}
