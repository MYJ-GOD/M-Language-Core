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

/* Program 4: Function call - define add(a,b), call add(5,3) */
static ByteBuf build_function_demo(void) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    
    /* === Function definition === */
    int fn_start = b.len;
    emit_op(&b, M_FN); emit_uvar(&b, 2);       /* FN arity=2 */
    emit_op(&b, M_B);                            /* B (block begin) */
    
    /* Function body: a + b */
    emit_op(&b, M_V);    emit_uvar(&b, 0);      /* V 0 (a) */
    emit_op(&b, M_V);    emit_uvar(&b, 1);      /* V 1 (b) */
    emit_op(&b, M_ADD);                          /* a + b */
    emit_op(&b, M_RT);                           /* RT (return) */
    
    emit_op(&b, M_E);                            /* E (block end) */
    
    /* === Main program === */
    /* Call function with args: CL,<addr>,<argc>,<arg0>,<arg1> */
    emit_op(&b, M_LIT);  emit_uvar(&b, 3);      /* arg1 = 3 */
    emit_op(&b, M_LIT);  emit_uvar(&b, 5);      /* arg0 = 5 */
    emit_op(&b, M_CL);  emit_uvar(&b, fn_start); /* func_id */
    emit_uvar(&b, 2);                            /* argc = 2 */
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
    m_vm_init(&vm, prog->buf, prog->len, io_write, io_read, sleep_ms, trace_fn);
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
    ByteBuf p1 = build_arithmetic_demo();
    ByteBuf p2 = build_comparison_demo();
    ByteBuf p3 = build_variables_demo();
    ByteBuf p4 = build_function_demo();
    ByteBuf p5 = build_loop_demo();
    ByteBuf p6 = build_bitwise_demo();
    ByteBuf p7 = build_stack_demo();
    ByteBuf p8 = build_io_demo();
    
    run_with_disasm("Arithmetic (5 + 3 * 2)", &p1, false);
    run_with_disasm("Comparison (10 > 5)", &p2, false);
    run_with_disasm("Variables (let x=10, y=x+5)", &p3, false);
    run_with_disasm("Function (add 5 + 3)", &p4, true);
    run_with_disasm("Loop (sum 1 to 5)", &p5, true);
    run_with_disasm("Bitwise (5 & 3, 5 | 3)", &p6, false);
    run_with_disasm("Stack operations", &p7, false);
    run_with_disasm("IO with authorization", &p8, false);
    
    printf("\n");
    printf("+================================================================+\n");
    printf("|                     All Tests Complete!                          |\n");
    printf("+================================================================+\n");
    
    return 0;
}
