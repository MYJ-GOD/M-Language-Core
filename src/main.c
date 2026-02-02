/**
 * M Language Virtual Machine - Test Suite (M-Token Edition)
 * 
 * 测试新的 M-Token 规范:
 * - 全 varint 编码
 * - 结构化控制流 (FN, IF, B, E)
 * - 高级操作 (数组、位运算)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "m_vm.h"
#include "disasm.h"

/* =============================================
 * IO Hooks
 * ============================================= */

static void io_write(uint8_t device_id, M_Value value) {
    printf("[IOW] dev=%u val=%d\n", (unsigned)device_id, (int)value.u.i);
}

static M_Value io_read(uint8_t device_id) {
    printf("[IOR] dev=%u\n", (unsigned)device_id);
    M_Value v; v.type = M_TYPE_INT; v.u.i = 42; return v;
}

static void sleep_ms(int32_t ms) {
    printf("[WAIT] %d ms\n", ms);
}

static void trace_fn(uint32_t level, const char* msg) {
    printf("[TRACE:%u] %s\n", (unsigned)level, msg);
}

/* =============================================
 * Bytecode Builder (Varint only)
 * ============================================= */

typedef struct {
    uint8_t buf[512];
    int len;
} ByteBuf;

static void emit_uvar(ByteBuf* b, uint32_t u) {
    b->len += m_vm_encode_uvarint(u, &b->buf[b->len]);
}

static void emit_svar(ByteBuf* b, int32_t s) {
    emit_uvar(b, m_vm_encode_zigzag(s));
}

static void emit_op(ByteBuf* b, uint32_t op) {
    emit_uvar(b, op);  /* All opcodes are varint */
}

/* =============================================
 * Test Programs
 * ============================================= */

/* Program 1: Simple arithmetic - 5 + 3 * 2 = 11 */
static ByteBuf build_arithmetic_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);      /* LIT 5 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);      /* LIT 3 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 2);      /* LIT 2 */
    emit_op(&b, M_MUL);                          /* 3 * 2 = 6 */
    emit_op(&b, M_ADD);                          /* 5 + 6 = 11 */
    emit_op(&b, M_HALT);
    return b;
}

/* Program 2: Comparison - 10 > 5 ? 1 : 0 */
static ByteBuf build_comparison_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    emit_op(&b, M_LIT);  emit_uvar(&b, 10);     /* LIT 10 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);      /* LIT 5 */
    emit_op(&b, M_GT);                           /* 10 > 5 = true */
    emit_op(&b, M_HALT);
    return b;
}

/* Program 3: Variables - let x = 10; let y = x + 5; result = y */
static ByteBuf build_variables_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    emit_op(&b, M_LIT);  emit_uvar(&b, 10);     /* LIT 10 */
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* LET 0 (x=10) */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);      /* LIT 5 */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* V 0 (x) */
    emit_op(&b, M_ADD);                          /* x + 5 = 15 */
    emit_op(&b, M_LET);  emit_uvar(&b, 1);      /* LET 1 (y=15) */
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* V 1 (y) */
    emit_op(&b, M_HALT);
    return b;
}

/* Program 4: Nested function call demo
 * Demonstrates nested function calls with proper frame management.
 *
 * Functions:
 *   add(a, b) = a + b
 *   double(x) = add(x, x)  [nested call]
 *   main = double(5) + double(3) = 25 + 6 = 31
 */
static ByteBuf build_nested_function_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* === Function: add(a, b) at offset 0 === */
    int fn_add = b.len;
    emit_op(&b, M_FN); emit_uvar(&b, 2);       /* FN arity=2 */
    emit_op(&b, M_B);                            /* B (block begin) */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* V 0 (a) */
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* V 1 (b) */
    emit_op(&b, M_ADD);                          /* a + b */
    emit_op(&b, M_RT);                           /* RT (return) */
    emit_op(&b, M_E);                            /* E (block end) */
    
    /* === Function: double(x) = add(x, x) at offset fn_double === */
    int fn_double = b.len;
    emit_op(&b, M_FN); emit_uvar(&b, 1);       /* FN arity=1 */
    emit_op(&b, M_B);                            /* B (block begin) */
    
    /* Push argument twice for add(x, x) */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* V 0 (x) - first arg */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* V 0 (x) - second arg */
    
    /* Call add(x, x) - nested call! */
    emit_op(&b, M_CL); emit_uvar(&b, fn_add);  /* CL to add */
    emit_uvar(&b, 2);                            /* argc = 2 */
    
    emit_op(&b, M_RT);                           /* RT (return double result) */
    emit_op(&b, M_E);                            /* E (block end) */
    
    /* === Main program === */
    /* double(5) */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);      /* arg0 = 5 */
    emit_op(&b, M_CL);  emit_uvar(&b, fn_double); /* CL to double */
    emit_uvar(&b, 1);                            /* argc = 1 */
    
    /* double(3) */
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);      /* arg0 = 3 */
    emit_op(&b, M_CL);  emit_uvar(&b, fn_double); /* CL to double */
    emit_uvar(&b, 1);                            /* argc = 1 */
    
    /* Add results: double(5) + double(3) = 25 + 6 = 31 */
    emit_op(&b, M_ADD);
    emit_op(&b, M_HALT);
    
    return b;
}

/* Program 5: Loop - sum 1 to 5 = 15, using JZ/GOTO instead of WH */
static ByteBuf build_loop_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* i = 5, sum = 0 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* x = 5 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);
    emit_op(&b, M_LET);  emit_uvar(&b, 1);      /* sum = 0 */
    
    /* L_cond: check x > 0 */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* x */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);      /* 0 */
    emit_op(&b, M_GT);                           /* x > 0 */
    
    /* Save position for JZ target, emit placeholder */
    int jz_placeholder = b.len;
    emit_op(&b, M_JZ);  emit_uvar(&b, 0);       /* placeholder: JZ L_end */
    
    /* L_body: sum += x; x-- */
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* sum */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* x */
    emit_op(&b, M_ADD);                          /* sum + x */
    emit_op(&b, M_LET);  emit_uvar(&b, 1);      /* sum = result */
    
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* x */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);      /* 1 */
    emit_op(&b, M_SUB);                          /* x - 1 */
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* x = result */
    
    /* Save position for JMP target, emit placeholder */
    int jmp_placeholder = b.len;
    emit_op(&b, M_JMP); emit_uvar(&b, 0);       /* placeholder: JMP L_cond */
    
    /* L_end: output result */
    int loop_end = b.len;
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* sum */
    emit_op(&b, M_HALT);
    
    /* Backpatch: calculate offsets */
    /* JZ target = loop_end (absolute) */
    int jz_target = loop_end;
    /* JMP target = 8 (position of cond check: V 0) */
    int jmp_target = 8;
    
    /* Backpatch JZ target */
    uint8_t* jz_ptr = &b.buf[jz_placeholder + 1];  /* after JZ opcode */
    int jz_len = 0;
    int32_t jz_val = jz_target;
    do {
        jz_ptr[jz_len] = (uint8_t)(jz_val & 0x7F);
        jz_val >>= 7;
        if (jz_val > 0) jz_ptr[jz_len] |= 0x80;
        jz_len++;
    } while (jz_val > 0);
    
    /* Backpatch JMP target */
    uint8_t* jmp_ptr = &b.buf[jmp_placeholder + 1];  /* after JMP opcode */
    int jmp_len = 0;
    int32_t jmp_val = jmp_target;
    do {
        jmp_ptr[jmp_len] = (uint8_t)(jmp_val & 0x7F);
        jmp_val >>= 7;
        if (jmp_val > 0) jmp_ptr[jmp_len] |= 0x80;
        jmp_len++;
    } while (jmp_val > 0);
    
    return b;
}

/* =============================================
 * WHILE Loop Compiler Lowering
 * ============================================= */

/**
 * Backpatch a uvarint operand at a given offset
 * Re-encodes the varint at the specified position.
 */
static void backpatch_uvar(ByteBuf* b, int offset, int32_t value) {
    uint8_t* ptr = &b->buf[offset];
    int len = 0;
    int32_t val = value;
    do {
        ptr[len] = (uint8_t)(val & 0x7F);
        val >>= 7;
        if (val > 0) ptr[len] |= 0x80;
        len++;
    } while (val > 0);
}

/**
 * Emit a WHILE loop - compiler lowering from high-level to JZ/JMP
 *
 * High-level syntax:
 *   WHILE <cond> { <body> }
 *
 * Lowered to bytecode:
 *   L_cond:
 *     <cond>
 *     JZ L_end
 *   L_body:
 *     <body>
 *     JMP L_cond
 *   L_end:
 *
 * @param b              Bytecode buffer
 * @param cond_start     Output: PC of condition check start
 * @param loop_end       Output: PC after loop ends (can be NULL)
 */
static void emit_while_loop(ByteBuf* b, int* cond_start, int* loop_end) {
    *cond_start = b->len;  /* L_cond position */

    /* Placeholder for condition - will be filled by caller if needed */
    /* In this simple version, condition is emitted inline */
}

/**
 * Emit condition check: x > 0
 * Helper for build_while_demo
 */
static void emit_while_cond_gt_zero(ByteBuf* bb, int var_idx) {
    emit_op(bb, M_V);    emit_uvar(bb, var_idx);  /* var */
    emit_op(bb, M_LIT);  emit_uvar(bb, 0);        /* 0 */
    emit_op(bb, M_GT);                           /* var > 0 */
}

/**
 * Emit loop body: sum += x; x--
 * var_sum = var_sum + var_x
 * var_x = var_x - 1
 */
static void emit_while_body_incr_decr(ByteBuf* bb, int var_sum, int var_x) {
    /* sum = sum + x */
    emit_op(bb, M_V);    emit_uvar(bb, var_sum);  /* sum */
    emit_op(bb, M_V);    emit_uvar(bb, var_x);    /* x */
    emit_op(bb, M_ADD);                          /* sum + x */
    emit_op(bb, M_LET);  emit_uvar(bb, var_sum);  /* sum = result */

    /* x = x - 1 */
    emit_op(bb, M_V);    emit_uvar(bb, var_x);    /* x */
    emit_op(bb, M_LIT);  emit_uvar(bb, 1);        /* 1 */
    emit_op(bb, M_SUB);                          /* x - 1 */
    emit_op(bb, M_LET);  emit_uvar(bb, var_x);    /* x = result */
}

/**
 * Build a complete WHILE loop bytecode
 * Demonstrates compiler lowering: WHILE → JZ/JMP
 */
static void build_while_with_body(ByteBuf* b,
                                   void (*emit_cond)(ByteBuf*, int),
                                   void (*emit_body)(ByteBuf*, int, int),
                                   int var_idx,
                                   int var_sum, int var_x) {
    int cond_start = b->len;  /* L_cond position */

    /* Emit condition */
    emit_cond(b, var_idx);

    /* Emit JZ L_end (placeholder) */
    int jz_placeholder = b->len;
    emit_op(b, M_JZ);
    emit_uvar(b, 0);

    /* Emit loop body */
    emit_body(b, var_sum, var_x);

    /* Emit JMP L_cond */
    int jmp_placeholder = b->len;
    emit_op(b, M_JMP);
    emit_uvar(b, 0);

    /* Backpatch JZ target = L_end */
    int loop_end = b->len;
    backpatch_uvar(b, jz_placeholder + 1, loop_end);

    /* Backpatch JMP target = L_cond */
    backpatch_uvar(b, jmp_placeholder + 1, cond_start);
}

/* Program 11: WHILE loop - using compiler lowering demonstration
 * Same logic as loop_demo but using emit_while_loop helper
 * Computes sum of 1 to 5 = 15
 */
static ByteBuf build_while_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));

    /* sum = 0, x = 5 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* sum = 0 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LET);  emit_uvar(&b, 1);      /* x = 5 */

    /* WHILE (x > 0) { sum += x; x-- } */
    build_while_with_body(&b,
        emit_while_cond_gt_zero,    /* condition: x > 0 */
        emit_while_body_incr_decr,  /* body: sum+=x, x-- */
        1,                          /* var_idx for condition */
        0, 1);                      /* sum=0, x=1 */

    /* Output result */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* sum */
    emit_op(&b, M_HALT);

    return b;
}

/* Program 11b: DO-WHILE loop demo (execute body first, then check condition)
 * DO-WHILE: do { body } while (cond);
 * Format: DO, <body>, <cond>, DWHL, <do_start_addr>
 */
static ByteBuf build_do_while_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* sum = 0, i = 5 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* sum = 0 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LET);  emit_uvar(&b, 1);      /* i = 5 */
    
    /* DO loop start */
    int do_start = b.len;
    emit_op(&b, M_DO);                           /* DO marker */
    
    /* Loop body: sum += i, i-- */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* sum */
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* i */
    emit_op(&b, M_ADD);                          /* sum + i */
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* sum = result */
    
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* i */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);      /* 1 */
    emit_op(&b, M_SUB);                          /* i - 1 */
    emit_op(&b, M_LET);  emit_uvar(&b, 1);      /* i = result */
    
    /* Condition: i > 0 */
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* i */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);      /* 0 */
    emit_op(&b, M_GT);                           /* i > 0 */
    
    /* DWHL: if cond != 0, jump to DO */
    emit_op(&b, M_DWHL); emit_uvar(&b, do_start); /* DWHL, jump to DO */
    
    /* Return sum */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* sum */
    emit_op(&b, M_HALT);
    
    return b;
}

/* Program 14: Stack overflow protection demo
 * This demo shows that the VM correctly detects stack overflow.
 * 
 * We create a function that calls itself recursively without returning.
 * The return stack will overflow and trigger RET_STACK_OVERFLOW fault.
 */
static ByteBuf build_stack_overflow_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* Function: recurse() - calls itself infinitely */
    int fn_recurse = b.len;
    emit_op(&b, M_FN); emit_uvar(&b, 0);       /* FN arity=0 */
    emit_op(&b, M_B);                            /* B */
    
    /* Push a value (simulate work) */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);      /* push 1 */
    emit_op(&b, M_DRP);                          /* drop */
    
    /* Recursive call to self */
    emit_op(&b, M_CL); emit_uvar(&b, fn_recurse); /* recurse() */
    emit_uvar(&b, 0);                            /* argc = 0 */
    
    emit_op(&b, M_RT);                           /* RT (never reached) */
    emit_op(&b, M_E);                            /* E */
    
    /* Main: call the recursive function */
    emit_op(&b, M_CL); emit_uvar(&b, fn_recurse); /* recurse() */
    emit_uvar(&b, 0);                            /* argc = 0 */
    
    /* This should never be reached due to stack overflow */
    emit_op(&b, M_LIT);  emit_uvar(&b, 999);
    emit_op(&b, M_HALT);
    
    return b;
}

/* =============================================
 * GC and Debugging Demos
 * ============================================= */

/* Program 15: Garbage Collection demo
 * Demonstrates automatic memory reclamation.
 * Allocates many small objects, then triggers GC.
 */
static ByteBuf build_gc_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* Allocate several small objects */
    for (int i = 0; i < 5; i++) {
        emit_op(&b, M_LIT);  emit_uvar(&b, 16);     /* size = 16 */
        emit_op(&b, M_ALLOC);                        /* ALLOC opcode */
        emit_uvar(&b, 16);                           /* size */
        emit_op(&b, M_DRP);                          /* drop ref (creates garbage) */
    }
    
    /* Trigger manual GC */
    emit_op(&b, M_GC);                               /* Manual GC trigger */
    
    /* Return success */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);
    emit_op(&b, M_HALT);
    
    return b;
}

/* Program 16: Breakpoint demo
 * Demonstrates breakpoint functionality.
 */
static ByteBuf build_breakpoint_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* Set breakpoint at this location */
    emit_op(&b, M_BP); emit_uvar(&b, 1);  /* BP, id=1 */
    
    /* Simple computation */
    emit_op(&b, M_LIT);  emit_uvar(&b, 10);
    emit_op(&b, M_LIT);  emit_uvar(&b, 20);
    emit_op(&b, M_ADD);
    
    emit_op(&b, M_HALT);
    
    return b;
}

/* Program 17: Single-step demo
 * Demonstrates single-step execution mode.
 */
static ByteBuf build_single_step_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* Enable single-step for debugging */
    emit_op(&b, M_STEP);                             /* Enable single-step */
    
    /* Simple computation */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);
    emit_op(&b, M_ADD);                              /* 5 + 3 = 8 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 2);
    emit_op(&b, M_MUL);                              /* 8 * 2 = 16 */
    
    emit_op(&b, M_HALT);
    
    return b;
}

/* =============================================
 * FOR Loop Compiler Lowering
 * ============================================= */

/**
 * FOR loop builder - compiles high-level FOR to JZ/JMP
 *
 * High-level:
 *   FOR (init; cond; update) { body }
 *
 * Bytecode:
 *   <init>
 * L_cond:
 *   <cond>
 *   JZ L_end
 * L_body:
 *   <body>
 * L_update:
 *   <update>
 *   JMP L_cond
 * L_end:
 */
static void build_for_loop(ByteBuf* b,
                            void (*emit_init)(ByteBuf*),
                            void (*emit_cond)(ByteBuf*),
                            void (*emit_body)(ByteBuf*),
                            void (*emit_update)(ByteBuf*)) {
    /* 1. Emit initialization (once before loop) */
    if (emit_init) emit_init(b);

    /* 2. L_cond: Emit condition check */
    int cond_start = b->len;
    if (emit_cond) emit_cond(b);

    /* 3. Emit JZ L_end */
    int jz_placeholder = b->len;
    emit_op(b, M_JZ);
    emit_uvar(b, 0);

    /* 4. L_body: Emit loop body */
    if (emit_body) emit_body(b);

    /* 5. L_update: Emit update, then jump back to condition */
    if (emit_update) emit_update(b);

    /* 6. Emit JMP L_cond */
    int jmp_placeholder = b->len;
    emit_op(b, M_JMP);
    emit_uvar(b, 0);

    /* 7. L_end: Loop ends here */
    int loop_end = b->len;

    /* 8. Backpatch */
    backpatch_uvar(b, jz_placeholder + 1, loop_end);
    backpatch_uvar(b, jmp_placeholder + 1, cond_start);
}

/* FOR loop helper: emit init i = 0 */
static void emit_for_init_i0(ByteBuf* bb) {
    emit_op(bb, M_LIT);  emit_uvar(bb, 0);
    emit_op(bb, M_LET);  emit_uvar(bb, 1);  /* i = 0 */
}

/* FOR loop helper: emit cond i < 5 */
static void emit_for_cond_i_lt_5(ByteBuf* bb) {
    emit_op(bb, M_V);    emit_uvar(bb, 1);  /* i */
    emit_op(bb, M_LIT);  emit_uvar(bb, 5);  /* 5 */
    emit_op(bb, M_LT);                       /* i < 5 */
}

/* FOR loop helper: emit body sum += i */
static void emit_for_body_sum_i(ByteBuf* bb) {
    emit_op(bb, M_V);    emit_uvar(bb, 0);  /* sum */
    emit_op(bb, M_V);    emit_uvar(bb, 1);  /* i */
    emit_op(bb, M_ADD);                      /* sum + i */
    emit_op(bb, M_LET);  emit_uvar(bb, 0);  /* sum = result */
}

/* FOR loop helper: emit update i++ */
static void emit_for_update_i_inc(ByteBuf* bb) {
    emit_op(bb, M_V);    emit_uvar(bb, 1);  /* i */
    emit_op(bb, M_LIT);  emit_uvar(bb, 1);  /* 1 */
    emit_op(bb, M_ADD);                      /* i + 1 */
    emit_op(bb, M_LET);  emit_uvar(bb, 1);  /* i = result */
}

/* Program 12: FOR loop - compiler lowering demonstration
 * Computes sum of 0 to 4 = 10
 * FOR (i=0; i<5; i++) { sum += i }
 */
static ByteBuf build_for_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));

    /* sum = 0 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);
    emit_op(&b, M_LET);  emit_uvar(&b, 0);      /* sum = 0 */

    /* FOR (i=0; i<5; i++) { sum += i } */
    build_for_loop(&b,
        emit_for_init_i0,      /* init: i = 0 */
        emit_for_cond_i_lt_5,  /* cond: i < 5 */
        emit_for_body_sum_i,   /* body: sum += i */
        emit_for_update_i_inc  /* update: i++ */
    );

    /* Output result */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* sum */
    emit_op(&b, M_HALT);

    return b;
}

/* =============================================
 * Memory Management (ALLOC/FREE)
 * ============================================= */

/* Program 13: Memory allocation and deallocation demo
 * ALLOC: <size> -> <ptr>  (allocate heap memory)
 * FREE:  <ptr> -> (free memory)
 *
 * This demo allocates memory, writes a value, reads it back, then frees.
 */
static ByteBuf build_memory_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* Allocate 16 bytes */
    emit_op(&b, M_LIT);  emit_uvar(&b, 16);     /* size = 16 */
    emit_op(&b, M_ALLOC);                        /* ALLOC opcode */
    emit_uvar(&b, 16);                           /* ALLOC size parameter */
    
    /* Free the memory */
    emit_op(&b, M_FREE);                         /* FREE the memory */
    
    /* Return 1 to indicate success */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);
    emit_op(&b, M_HALT);
    
    return b;
}

/* Program 6: Bit operations - 5 AND 3 = 1, 5 OR 3 = 7 */
static ByteBuf build_bitwise_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* 5 AND 3 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);
    emit_op(&b, M_AND);                          /* 5 & 3 = 1 */
    emit_op(&b, M_DUP);                          /* duplicate for second op */
    
    /* 5 OR 3 */
    emit_op(&b, M_DRP);                          /* drop previous result */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);
    emit_op(&b, M_OR);                           /* 5 | 3 = 7 */
    
    emit_op(&b, M_HALT);
    return b;
}

/* Program 7: Stack operations */
static ByteBuf build_stack_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);      /* 1 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 2);      /* 2 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);      /* 3 */
    emit_op(&b, M_DUP);                          /* dup: 1,2,3,3 */
    emit_op(&b, M_SWP);                          /* swap: 1,2,3,3 -> 1,2,3,3 */
    emit_op(&b, M_DRP);                          /* drop: 1,2,3 */
    emit_op(&b, M_HALT);
    return b;
}

/* Program 8: Full authorized IO demo */
static ByteBuf build_io_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    emit_op(&b, M_GTWAY); emit_uvar(&b, M_GATEWAY_KEY); /* authorize */
    emit_op(&b, M_LIT);  emit_uvar(&b, 100);            /* value */
    emit_op(&b, M_IOW);  emit_uvar(&b, 1);              /* dev=1 */
    emit_op(&b, M_IOR);  emit_uvar(&b, 1);              /* read dev=1 */
    emit_op(&b, M_HALT);
    return b;
}

/* Program 9: Modulo - test C semantics: a % b (sign matches a) */
static ByteBuf build_mod_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));

    /* Test 1: 10 % 3 = 1 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 10);
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);
    emit_op(&b, M_MOD);                          /* 10 % 3 = 1 */
    emit_op(&b, M_DUP);                          /* duplicate result */

    /* Test 2: -5 % 2 = -1 (C semantics: sign matches a) */
    emit_op(&b, M_DRP);                          /* drop previous */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);       /* -5: encode as signed */
    emit_op(&b, M_LIT);  emit_uvar(&b, 2);
    emit_op(&b, M_SUB);                          /* -5 = 0 - 5 */
    emit_op(&b, M_MOD);                          /* -5 % 2 = -1 */
    emit_op(&b, M_DUP);                          /* duplicate */

    /* Test 3: 5 % -2 = 1 (sign matches a) */
    emit_op(&b, M_DRP);
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);
    emit_op(&b, M_LIT);  emit_uvar(&b, 2);
    emit_op(&b, M_SUB);                          /* 5 % -2: 5 - (5/(-2))*(-2) = 5 - (-2)*(-2) = 5 - 4 = 1 */
    emit_op(&b, M_MOD);                          /* 5 % -2 = 1 */

    emit_op(&b, M_HALT);
    return b;
}

/* Program 10: Array operations - NEWARR, IDX, STO, LEN */
static ByteBuf build_array_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));

    /* Create array of size 3 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);      /* size = 3 */
    emit_op(&b, M_NEWARR);                       /* create array, push ref */

    /* Store values: arr[0] = 42, arr[1] = 99, arr[2] = 77 */
    emit_op(&b, M_DUP);                          /* dup arr ref */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);      /* idx = 0 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 42);     /* val = 42 */
    emit_op(&b, M_STO);                          /* store, push arr */

    emit_op(&b, M_DUP);                          /* dup arr ref */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);      /* idx = 1 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 99);     /* val = 99 */
    emit_op(&b, M_STO);                          /* store, push arr */

    emit_op(&b, M_DUP);                          /* dup arr ref */
    emit_op(&b, M_LIT);  emit_uvar(&b, 2);      /* idx = 2 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 77);     /* val = 77 */
    emit_op(&b, M_STO);                          /* store, push arr */

    /* Get length - dup arr first so we keep the reference */
    emit_op(&b, M_DUP);                          /* dup arr */
    emit_op(&b, M_DUP);                          /* dup arr again (now have 2 refs) */
    emit_op(&b, M_LEN);                          /* pop arr, push len (3) - arr still on stack */

    /* Read back values to verify */
    emit_op(&b, M_DRP);                          /* drop len, keep arr */
    emit_op(&b, M_DUP);                          /* dup arr */
    emit_op(&b, M_LIT);  emit_uvar(&b, 0);      /* idx = 0 */
    emit_op(&b, M_IDX);                          /* push arr[0] = 42 */

    emit_op(&b, M_DRP);                          /* drop arr */
    emit_op(&b, M_DUP);                          /* dup arr */
    emit_op(&b, M_LIT);  emit_uvar(&b, 1);      /* idx = 1 */
    emit_op(&b, M_IDX);                          /* push arr[1] = 99 */

    emit_op(&b, M_HALT);
    return b;
}

/* =============================================
 * Runner with Disassembly
 * ============================================= */

static void run_with_disasm(const char* name, ByteBuf* prog, bool do_simulate) {
    printf("\n");
    printf("+============================================================+\n");
    printf("|  Program: %-54s |\n", name);
    printf("+============================================================+\n");
    
    /* Print bytecode size */
    printf("Bytecode size: %d bytes\n\n", prog->len);
    
    /* Disassembly */
    const char* disasm = m_disasm(prog->buf, prog->len);
    printf("%s", disasm);
    
    /* Execute */
    M_VM vm;
    m_vm_init(&vm, prog->buf, prog->len, (void*)io_write, (void*)io_read, (void*)sleep_ms, (void*)trace_fn);
    m_vm_set_step_limit(&vm, 10000);
    
    if (do_simulate) {
        M_SimResult result;
        m_vm_simulate(&vm, &result);
        m_disasm_print_trace(&result);
    } else {
        int r = m_vm_run(&vm);
        printf("\nExecution result: fault=%s, steps=%llu, result=%d\n",
               m_vm_fault_string(vm.fault),
               (unsigned long long)vm.steps,
               (vm.sp >= 0) ? (int)vm.stack[vm.sp].u.i : 0);
    }

    /* Free allocated memory */
    m_vm_destroy(&vm);
}

/* =============================================
 * Main
 * ============================================= */

int main(void) {
    printf("+================================================================+\n");
    printf("|           M Language Virtual Machine - Test Suite                |\n");
    printf("|                    M-Token Edition                               |\n");
    printf("+================================================================+\n");
    
    /* Run all tests */
    /*ByteBuf p1 = build_arithmetic_demo();
    ByteBuf p2 = build_comparison_demo();
    ByteBuf p3 = build_variables_demo();
    ByteBuf p4 = build_nested_function_demo();
    ByteBuf p5 = build_loop_demo();
    ByteBuf p6 = build_bitwise_demo();
    ByteBuf p7 = build_stack_demo();
    ByteBuf p8 = build_io_demo();
    ByteBuf p9 = build_mod_demo();
    ByteBuf p10 = build_array_demo();
    ByteBuf p11 = build_while_demo();*/
    ByteBuf p12 = build_for_demo();
    ByteBuf p13 = build_memory_demo();
    ByteBuf p11b = build_do_while_demo();
    ByteBuf p14 = build_stack_overflow_demo();
    ByteBuf p15 = build_gc_demo();
    ByteBuf p16 = build_breakpoint_demo();
    ByteBuf p17 = build_single_step_demo();

    /*run_with_disasm("Arithmetic (5 + 3 * 2)", &p1, false);
    run_with_disasm("Comparison (10 > 5)", &p2, false);
    run_with_disasm("Nested function calls (double = add(x,x), main = double(5)+double(3))", &p4, true);
    run_with_disasm("Variables (let x=10, y=x+5)", &p3, false);
    run_with_disasm("Function (add 5 + 3)", &p4, true);
    run_with_disasm("Loop (sum 1 to 5)", &p5, true);
    run_with_disasm("Bitwise (5 & 3, 5 | 3)", &p6, false);
    run_with_disasm("Stack operations", &p7, false);
    run_with_disasm("IO with authorization", &p8, false);
    run_with_disasm("Modulo (C semantics: 10%3, -5%2, 5%-2)", &p9, false);
    run_with_disasm("Array (NEWARR, STO, IDX, LEN)", &p10, false);
    run_with_disasm("WHILE Loop (compiler lowering)", &p11, true);*/
    ByteBuf p4 = build_nested_function_demo();
    run_with_disasm("Nested Function Calls (double(x)=add(x,x), main=double(5)+double(3)=31)", &p4, true);
    run_with_disasm("DO-WHILE Loop (do { sum+=i; i-- } while i>0, sum=1..5=15)", &p11b, true);
    run_with_disasm("FOR Loop (compiler lowering)", &p12, true);
    run_with_disasm("Memory ALLOC/FREE", &p13, false);
    run_with_disasm("Stack Overflow Protection (recursive function triggers overflow)", &p14, true);
    run_with_disasm("Garbage Collection (GC)", &p15, true);
    run_with_disasm("Breakpoint Demo", &p16, true);
    run_with_disasm("Single-Step Debugging (STEP)", &p17, true);
    
    printf("\n");
    printf("+================================================================+\n");
    printf("|                     All Tests Complete!                          |\n");
    printf("+================================================================+\n");

    return 0;
}
