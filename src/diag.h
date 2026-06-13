/*
 * VORTECH Compiler - Diagnostics
 */
#ifndef VORTECH_DIAG_H
#define VORTECH_DIAG_H

#include "common.h"

typedef enum {
    DIAG_NOTE,
    DIAG_WARN,
    DIAG_ERROR,
    DIAG_FATAL,
} DiagLevel;

/* Report a diagnostic at a source location */
void diag_report(DiagLevel level, SrcLoc loc, const char *fmt, ...);

/* Report a diagnostic without a source location */
void diag_msg(DiagLevel level, const char *fmt, ...);

/* Get the count of errors reported so far */
int diag_error_count(void);

/* Reset error count */
void diag_reset(void);

#endif /* VORTECH_DIAG_H */
