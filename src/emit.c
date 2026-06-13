/*
 * VORTECH Compiler - x86-64 Code Emitter Implementation
 *
 * Generates Intel-syntax GAS assembly.
 */
#include "emit.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Get register name appropriate for the type size */
static const char *reg_for_type(X86Reg r, VtType *t) {
    if (!t) return reg_name(r);
    switch (vt_type_size(t)) {
    case 1:  return reg8_name(r);
    case 2:  return reg_name(r); /* 16-bit would need different names */
    case 4:  return reg32_name(r);
    default: return reg_name(r);
    }
}

/* Emit function prologue */
static void emit_prologue(FILE *out, MachFunc *mf, RegAllocResult *ra) {
    fprintf(out, "    push rbp\n");
    fprintf(out, "    mov rbp, rsp\n");

    if (ra->stack_size > 0) {
        fprintf(out, "    sub rsp, %u\n", ra->stack_size);
    }

    /* Save callee-saved registers */
    for (int r = 0; r < REG_COUNT; r++) {
        if (reg_is_callee_saved((X86Reg)r) && ra->reg_used[r]) {
            /* Calculate stack offset for saved register */
            fprintf(out, "    push %s\n", reg_name((X86Reg)r));
        }
    }

    /* Move parameters from calling convention regs to their assigned regs */
    static const X86Reg param_regs[] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9 };
    for (uint32_t i = 0; i < mf->nparams && i < 6; i++) {
        uint32_t pvreg = mf->param_vregs[i];
        if (pvreg < ra->num_vregs && ra->vreg_to_reg[pvreg] != REG_NONE) {
            X86Reg assigned = ra->vreg_to_reg[pvreg];
            if (assigned != param_regs[i]) {
                fprintf(out, "    mov %s, %s\n", reg_name(assigned), reg_name(param_regs[i]));
            }
        }
    }
}

/* Emit function epilogue */
static void emit_epilogue(FILE *out, RegAllocResult *ra) {
    /* Restore callee-saved registers in reverse order */
    for (int r = REG_COUNT - 1; r >= 0; r--) {
        if (reg_is_callee_saved((X86Reg)r) && ra->reg_used[r]) {
            fprintf(out, "    pop %s\n", reg_name((X86Reg)r));
        }
    }

    fprintf(out, "    mov rsp, rbp\n");
    fprintf(out, "    pop rbp\n");
    fprintf(out, "    ret\n");
}

/* Emit a single machine instruction */
static void emit_inst(FILE *out, MachInst *mi, RegAllocResult *ra) {
    switch (mi->op) {
    case MINST_LABEL:
        if (mi->label) {
            fprintf(out, "%s:\n", mi->label);
        }
        break;

    case MINST_MOV:
        if (mi->type == NULL) {
            /* Store: mov [dst_reg], src_reg */
            fprintf(out, "    mov [%s], %s\n",
                    mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                    mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM");
        } else {
            /* Regular move */
            fprintf(out, "    mov %s, %s\n",
                    mi->dst < REG_COUNT ? reg_for_type((X86Reg)mi->dst, mi->type) : "MEM",
                    mi->src < REG_COUNT ? reg_for_type((X86Reg)mi->src, mi->type) : "MEM");
        }
        break;

    case MINST_MOV_IMM:
        fprintf(out, "    mov %s, %lld\n",
                mi->dst < REG_COUNT ? reg_for_type((X86Reg)mi->dst, mi->type) : "MEM",
                (long long)mi->imm);
        break;

    case MINST_ADD:
        if (mi->dst != mi->src && mi->src < REG_COUNT && mi->dst < REG_COUNT) {
            fprintf(out, "    mov %s, %s\n", reg_name((X86Reg)mi->dst), reg_name((X86Reg)mi->src));
        }
        fprintf(out, "    add %s, %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_ADD_IMM:
        fprintf(out, "    add %s, %lld\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                (long long)mi->imm);
        break;

    case MINST_SUB:
        if (mi->dst != mi->src && mi->src < REG_COUNT && mi->dst < REG_COUNT) {
            fprintf(out, "    mov %s, %s\n", reg_name((X86Reg)mi->dst), reg_name((X86Reg)mi->src));
        }
        fprintf(out, "    sub %s, %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_SUB_IMM:
        fprintf(out, "    sub %s, %lld\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                (long long)mi->imm);
        break;

    case MINST_MUL:
        if (mi->dst != mi->src && mi->src < REG_COUNT && mi->dst < REG_COUNT) {
            fprintf(out, "    mov %s, %s\n", reg_name((X86Reg)mi->dst), reg_name((X86Reg)mi->src));
        }
        fprintf(out, "    imul %s, %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_DIV: {
        /* idiv divides rdx:rax by src, quotient in rax, remainder in rdx */
        X86Reg lhs_reg = mi->src < REG_COUNT ? (X86Reg)mi->src : REG_RAX;
        X86Reg rhs_reg = mi->src2 < REG_COUNT ? (X86Reg)mi->src2 : REG_RCX;
        fprintf(out, "    mov rax, %s\n", reg_name(lhs_reg));
        fprintf(out, "    cqo\n"); /* sign-extend rax into rdx */
        fprintf(out, "    idiv %s\n", reg_name(rhs_reg));
        if (mi->dst < REG_COUNT && (X86Reg)mi->dst != REG_RAX) {
            fprintf(out, "    mov %s, rax\n", reg_name((X86Reg)mi->dst));
        }
        break;
    }

    case MINST_AND:
        if (mi->dst != mi->src && mi->src < REG_COUNT && mi->dst < REG_COUNT) {
            fprintf(out, "    mov %s, %s\n", reg_name((X86Reg)mi->dst), reg_name((X86Reg)mi->src));
        }
        fprintf(out, "    and %s, %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_OR:
        if (mi->dst != mi->src && mi->src < REG_COUNT && mi->dst < REG_COUNT) {
            fprintf(out, "    mov %s, %s\n", reg_name((X86Reg)mi->dst), reg_name((X86Reg)mi->src));
        }
        fprintf(out, "    or %s, %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_XOR:
        if (mi->dst != mi->src && mi->src < REG_COUNT && mi->dst < REG_COUNT) {
            fprintf(out, "    mov %s, %s\n", reg_name((X86Reg)mi->dst), reg_name((X86Reg)mi->src));
        }
        fprintf(out, "    xor %s, %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_SHL:
        fprintf(out, "    shl %s, cl\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_SHR:
        fprintf(out, "    shr %s, cl\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_NEG:
        fprintf(out, "    neg %s\n",
                mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM");
        break;

    case MINST_NOT:
        fprintf(out, "    not %s\n",
                mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM");
        break;

    case MINST_CMP:
        fprintf(out, "    cmp %s, %s\n",
                mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_CMP_IMM:
        fprintf(out, "    cmp %s, %lld\n",
                mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM",
                (long long)mi->imm);
        break;

    case MINST_TEST:
        fprintf(out, "    test %s, %s\n",
                mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM",
                mi->src2 < REG_COUNT ? reg_name((X86Reg)mi->src2) : "MEM");
        break;

    case MINST_SET_EQ:
        fprintf(out, "    sete %s\n",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        /* Zero-extend to full register */
        fprintf(out, "    movzx %s, %s\n",
                mi->dst < REG_COUNT ? reg32_name((X86Reg)mi->dst) : "MEM",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_SET_NEQ:
        fprintf(out, "    setne %s\n",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        fprintf(out, "    movzx %s, %s\n",
                mi->dst < REG_COUNT ? reg32_name((X86Reg)mi->dst) : "MEM",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_SET_LT:
        fprintf(out, "    setl %s\n",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        fprintf(out, "    movzx %s, %s\n",
                mi->dst < REG_COUNT ? reg32_name((X86Reg)mi->dst) : "MEM",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_SET_GT:
        fprintf(out, "    setg %s\n",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        fprintf(out, "    movzx %s, %s\n",
                mi->dst < REG_COUNT ? reg32_name((X86Reg)mi->dst) : "MEM",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_SET_LEQ:
        fprintf(out, "    setle %s\n",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        fprintf(out, "    movzx %s, %s\n",
                mi->dst < REG_COUNT ? reg32_name((X86Reg)mi->dst) : "MEM",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_SET_GEQ:
        fprintf(out, "    setge %s\n",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        fprintf(out, "    movzx %s, %s\n",
                mi->dst < REG_COUNT ? reg32_name((X86Reg)mi->dst) : "MEM",
                mi->dst < REG_COUNT ? reg8_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_JMP:
        fprintf(out, "    jmp .L%u\n", mi->target_bb);
        break;

    case MINST_JE:
        fprintf(out, "    je .L%u\n", mi->target_bb);
        break;

    case MINST_JNE:
        fprintf(out, "    jne .L%u\n", mi->target_bb);
        break;

    case MINST_CALL: {
        /* Move arguments to calling convention registers */
        static const X86Reg arg_regs[] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9 };
        if (mi->arg_vregs) {
            for (uint32_t j = 0; j < mi->nargs && j < 6; j++) {
                X86Reg target = arg_regs[j];
                /* arg_vregs still contains vreg numbers (not replaced by regalloc).
                 * Look up the assigned physical register. */
                X86Reg src = REG_NONE;
                if (ra && mi->arg_vregs[j] < ra->num_vregs) {
                    src = ra->vreg_to_reg[mi->arg_vregs[j]];
                }
                if (src != REG_NONE && src != target) {
                    fprintf(out, "    mov %s, %s\n", reg_name(target), reg_name(src));
                }
            }
        }
        fprintf(out, "    call %s\n", mi->func_name ? mi->func_name : "???");
        break;
    }

    case MINST_RET:
        /* Move return value to rax if not already there */
        if (mi->src < REG_COUNT && (X86Reg)mi->src != REG_RAX) {
            fprintf(out, "    mov rax, %s\n", reg_name((X86Reg)mi->src));
        }
        emit_epilogue(out, ra);
        break;

    case MINST_LEA:
        if (mi->offset != 0) {
            fprintf(out, "    lea %s, [%s + %lld]\n",
                    mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                    mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM",
                    (long long)mi->offset);
        } else {
            fprintf(out, "    mov %s, %s\n",
                    mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM",
                    mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM");
        }
        break;

    case MINST_PUSH:
        fprintf(out, "    push %s\n",
                mi->src < REG_COUNT ? reg_name((X86Reg)mi->src) : "MEM");
        break;

    case MINST_POP:
        fprintf(out, "    pop %s\n",
                mi->dst < REG_COUNT ? reg_name((X86Reg)mi->dst) : "MEM");
        break;

    case MINST_NOP:
        break;

    default:
        fprintf(out, "    # unhandled opcode %d\n", mi->op);
        break;
    }
}

void emit_func(FILE *out, MachFunc *mf, RegAllocResult *ra) {
    fprintf(out, "    .globl %s\n", mf->name);
    fprintf(out, "    .type %s, @function\n", mf->name);
    fprintf(out, "%s:\n", mf->name);

    /* Prologue */
    emit_prologue(out, mf, ra);

    for (uint32_t bi = 0; bi < mf->nblocks; bi++) {
        MachBlock *mb = &mf->blocks[bi];

        /* Emit block label */
        fprintf(out, ".L%u:\n", mb->label);

        for (uint32_t ii = 0; ii < mb->ninsts; ii++) {
            MachInst *mi = &mb->insts[ii];
            if (mi->op == MINST_LABEL) continue; /* skip label insts, we emit them above */
            emit_inst(out, mi, ra);
        }
    }

    /* If the last block doesn't have a return, add one */
    fprintf(out, "    mov rax, 0\n");
    emit_epilogue(out, ra);
}

void emit_program(FILE *out, MachFunc *funcs, uint32_t nfuncs,
                  RegAllocResult **ras) {
    /* Assembly header */
    fprintf(out, "    .intel_syntax noprefix\n");
    fprintf(out, "    .text\n");

    /* Emit each function */
    for (uint32_t i = 0; i < nfuncs; i++) {
        fprintf(out, "\n");
        emit_func(out, &funcs[i], ras[i]);
    }

    /* Emit runtime helpers */
    fprintf(out, "\n");
    fprintf(out, "    .section .rodata\n");
    fprintf(out, ".print_fmt:\n");
    fprintf(out, "    .string \"%%lld\\n\"\n");
    fprintf(out, "\n");
    fprintf(out, "    .text\n");
    fprintf(out, "    .globl __vortech_print_i64\n");
    fprintf(out, "__vortech_print_i64:\n");
    fprintf(out, "    push rbp\n");
    fprintf(out, "    mov rbp, rsp\n");
    fprintf(out, "    mov rsi, rdi\n");
    fprintf(out, "    lea rdi, [rip + .print_fmt]\n");
    fprintf(out, "    xor eax, eax\n");
    fprintf(out, "    call printf\n");
    fprintf(out, "    mov rsp, rbp\n");
    fprintf(out, "    pop rbp\n");
    fprintf(out, "    ret\n");
}
