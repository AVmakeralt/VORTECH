/*
 * VORTECH Compiler - Common Subexpression Elimination
 *
 * Moderate complexity. Huge value.
 * a+b; a+b; -> compute once.
 */
#include "opt.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Simple hash for a CSE key */
typedef struct {
    SsaOpcode op;
    BinOpKind binop;
    UnOpKind  unop;
    CmpKind   cmp;
    uint32_t  operand1;
    uint32_t  operand2;
} CseKey;

static uint32_t cse_hash(CseKey *key) {
    uint32_t h = (uint32_t)key->op * 31;
    h ^= key->binop * 37;
    h ^= key->unop * 41;
    h ^= key->cmp * 43;
    h ^= key->operand1 * 53;
    h ^= key->operand2 * 59;
    return h;
}

static bool cse_key_eq(CseKey *a, CseKey *b) {
    return a->op == b->op && a->binop == b->binop && a->unop == b->unop &&
           a->cmp == b->cmp && a->operand1 == b->operand1 && a->operand2 == b->operand2;
}

#define CSE_TABLE_SIZE 1024

typedef struct CseEntry {
    CseKey           key;
    uint32_t         vreg;      /* the vreg that holds this value */
    struct CseEntry *next;
} CseEntry;

void opt_cse(SsaFunc *func) {
    CseEntry **table = calloc(CSE_TABLE_SIZE, sizeof(CseEntry *));
    if (!table) {
        diag_msg(DIAG_FATAL, "out of memory in CSE");
    }

    bool changed = false;

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        SsaBlock *blk = &func->blocks[bi];

        /* Clear table for each block (local CSE) */
        for (int i = 0; i < CSE_TABLE_SIZE; i++) {
            CseEntry *e = table[i];
            while (e) {
                CseEntry *next = e->next;
                free(e);
                e = next;
            }
            table[i] = NULL;
        }

        for (uint32_t ii = 0; ii < blk->ninsts; ii++) {
            SsaInst *inst = &blk->insts[ii];
            CseKey key;
            memset(&key, 0, sizeof(key));
            bool is_cse_candidate = false;

            switch (inst->op) {
            case OP_BINOP:
                key.op = OP_BINOP;
                key.binop = inst->binop.kind;
                key.operand1 = inst->binop.lhs;
                key.operand2 = inst->binop.rhs;
                is_cse_candidate = true;
                break;
            case OP_CMP:
                key.op = OP_CMP;
                key.cmp = inst->cmp.kind;
                key.operand1 = inst->cmp.lhs;
                key.operand2 = inst->cmp.rhs;
                is_cse_candidate = true;
                break;
            case OP_UNOP:
                key.op = OP_UNOP;
                key.unop = inst->unop.kind;
                key.operand1 = inst->unop.operand;
                is_cse_candidate = true;
                break;
            default:
                break;
            }

            if (!is_cse_candidate || inst->dst == VT_INVALID_VREG) continue;

            uint32_t h = cse_hash(&key) % CSE_TABLE_SIZE;

            /* Look up in table */
            CseEntry *entry = table[h];
            while (entry) {
                if (cse_key_eq(&entry->key, &key)) {
                    /* Found a common subexpression - replace with the previous result */
                    uint32_t old_vreg = inst->dst;
                    uint32_t new_vreg = entry->vreg;

                    /* Replace all uses of old_vreg with new_vreg */
                    for (uint32_t bj = 0; bj < func->nblocks; bj++) {
                        SsaBlock *bb = &func->blocks[bj];
                        for (uint32_t ij = 0; ij < bb->ninsts; ij++) {
                            SsaInst *other = &bb->insts[ij];
                            switch (other->op) {
                            case OP_BINOP:
                                if (other->binop.lhs == old_vreg) other->binop.lhs = new_vreg;
                                if (other->binop.rhs == old_vreg) other->binop.rhs = new_vreg;
                                break;
                            case OP_UNOP:
                                if (other->unop.operand == old_vreg) other->unop.operand = new_vreg;
                                break;
                            case OP_CMP:
                                if (other->cmp.lhs == old_vreg) other->cmp.lhs = new_vreg;
                                if (other->cmp.rhs == old_vreg) other->cmp.rhs = new_vreg;
                                break;
                            case OP_LOAD:
                                if (other->load.ptr == old_vreg) other->load.ptr = new_vreg;
                                break;
                            case OP_STORE:
                                if (other->store.ptr == old_vreg) other->store.ptr = new_vreg;
                                if (other->store.val == old_vreg) other->store.val = new_vreg;
                                break;
                            case OP_BRANCH:
                                if (other->branch.cond == old_vreg) other->branch.cond = new_vreg;
                                break;
                            case OP_CALL:
                                for (uint32_t k = 0; k < other->call.nargs; k++) {
                                    if (other->call.args[k] == old_vreg) other->call.args[k] = new_vreg;
                                }
                                break;
                            case OP_RET:
                                if (other->ret.val == old_vreg) other->ret.val = new_vreg;
                                break;
                            case OP_PHI:
                                for (uint32_t k = 0; k < other->phi.n; k++) {
                                    if (other->phi.srcs[k] == old_vreg) other->phi.srcs[k] = new_vreg;
                                }
                                break;
                            case OP_PRINT:
                                if (other->print.val == old_vreg) other->print.val = new_vreg;
                                break;
                            default:
                                break;
                            }
                        }
                    }

                    /* Mark this instruction as dead (DCE will remove it) */
                    inst->op = OP_CONST; /* will be cleaned by DCE */
                    inst->const_.value = 0;
                    changed = true;
                    break;
                }
                entry = entry->next;
            }

            if (!entry) {
                /* New expression - add to table */
                CseEntry *new_entry = malloc(sizeof(CseEntry));
                if (!new_entry) {
                    diag_msg(DIAG_FATAL, "out of memory in CSE");
                }
                new_entry->key = key;
                new_entry->vreg = inst->dst;
                new_entry->next = table[h];
                table[h] = new_entry;
            }
        }
    }

    /* Clean up */
    for (int i = 0; i < CSE_TABLE_SIZE; i++) {
        CseEntry *e = table[i];
        while (e) {
            CseEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    free(table);
}
