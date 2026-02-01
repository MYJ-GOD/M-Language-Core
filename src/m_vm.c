/**
 * M Language Virtual Machine - M-VM
 * 
 * Implements the M-Token specification with full varint encoding.
 * Supports: functions, conditionals, loops, arrays, and hardware IO.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "m_vm.h"

/* =============================================
 * Varint Encoding/Decoding Implementation
 * ============================================= */

bool m_vm_decode_uvarint(const uint8_t* code, int* pc, int len, uint32_t* out) {
    if (!code || !pc || !out) return false;
    
    int p = *pc;
    uint32_t res = 0;
    int shift = 0;
    bool terminated = false;

    while (p < len) {
        uint8_t b = code[p++];
        res |= (uint32_t)(b & 0x7F) << shift;

        if ((b & 0x80) == 0) { 
            terminated = true; 
            break; 
        }

        shift += 7;
        if (shift >= 32) {
            return false;
        }
    }

    if (!terminated) return false;

    *pc = p;
    *out = res;
    return true;
}

int m_vm_encode_uvarint(uint32_t n, uint8_t* out) {
    int i = 0;
    while (n > 0x7F) {
        out[i++] = (uint8_t)(n & 0x7F) | 0x80;
        n >>= 7;
    }
    out[i++] = (uint8_t)n;
    return i;
}

int32_t m_vm_decode_zigzag(uint32_t n) {
    return (int32_t)((n >> 1) ^ -(int32_t)(n & 1));
}

uint32_t m_vm_encode_zigzag(int32_t n) {
    return (uint32_t)((n << 1) ^ (n >> 31));
}

/* =============================================
 * Helper Macros
 * ============================================= */

#define SET_FAULT(vm, code) do { \
    (vm)->fault = (code); \
    (vm)->running = false; \
} while(0)

#define NEED(vm, k) do { \
    if ((vm)->sp + 1 < (k)) { SET_FAULT((vm), M_FAULT_STACK_UNDERFLOW); return; } \
} while(0)

#define SPACE(vm, k) do { \
    if ((vm)->sp + (k) >= STACK_SIZE) { SET_FAULT((vm), M_FAULT_STACK_OVERFLOW); return; } \
} while(0)

#define CHECK_LOCALS(vm, idx) do { \
    if ((idx) < 0 || (idx) >= LOCALS_SIZE) { SET_FAULT((vm), M_FAULT_LOCALS_OOB); return; } \
} while(0)

#define CHECK_GLOBALS(vm, idx) do { \
    if ((idx) < 0 || (idx) >= GLOBALS_SIZE) { SET_FAULT((vm), M_FAULT_GLOBALS_OOB); return; } \
} while(0)

#define CHECK_RET_PUSH(vm) do { \
    if ((vm)->rp + 1 >= RET_STACK_SIZE) { SET_FAULT((vm), M_FAULT_RET_STACK_OVERFLOW); return; } \
} while(0)

#define CHECK_RET_POP(vm) do { \
    if ((vm)->rp < 0) { SET_FAULT((vm), M_FAULT_RET_STACK_UNDERFLOW); return; } \
} while(0)

#define CHECK_FRAME_PUSH(vm) do { \
    if ((vm)->frame_sp + 1 >= RET_STACK_SIZE) { SET_FAULT((vm), M_FAULT_RET_STACK_OVERFLOW); return; } \
} while(0)

#define CHECK_FRAME_POP(vm) do { \
    if ((vm)->frame_sp < 0) { SET_FAULT((vm), M_FAULT_RET_STACK_UNDERFLOW); return; } \
} while(0)

#define CHECK_PC(vm, addr) do { \
    if ((addr) < 0 || (addr) >= (vm)->code_len) { SET_FAULT((vm), M_FAULT_PC_OOB); return; } \
} while(0)

#define TOP(vm) ((vm)->stack[(vm)->sp])
#define POP(vm) ((vm)->stack[(vm)->sp--])
#define PUSH(vm, val) do { (vm)->stack[++(vm)->sp] = (val); } while(0)

/* =============================================
 * Value Operations
 * ============================================= */

static M_Value make_int(int32_t i) {
    M_Value v;
    v.type = M_TYPE_INT;
    v.u.i = i;
    return v;
}

static M_Value make_bool(bool b) {
    M_Value v;
    v.type = M_TYPE_BOOL;
    v.u.b = b;
    return v;
}

static int32_t to_int(M_Value v) {
    switch (v.type) {
        case M_TYPE_INT: return v.u.i;
        case M_TYPE_FLOAT: return (int32_t)v.u.f;
        case M_TYPE_BOOL: return v.u.b ? 1 : 0;
        default: return 0;
    }
}

static bool to_bool(M_Value v) {
    switch (v.type) {
        case M_TYPE_INT: return v.u.i != 0;
        case M_TYPE_FLOAT: return v.u.f != 0.0f;
        case M_TYPE_BOOL: return v.u.b;
        default: return false;
    }
}

/* =============================================
 * Instruction Handlers
 * ============================================= */

/* --- Stack Operations --- */

static void h_dup(M_VM* v) {
    NEED(v, 1);
    SPACE(v, 1);
    PUSH(v, TOP(v));
}

static void h_drp(M_VM* v) {
    NEED(v, 1);
    (void)POP(v);
}

static void h_swp(M_VM* v) {
    NEED(v, 2);
    M_Value a = v->stack[v->sp - 1];
    M_Value b = v->stack[v->sp];
    v->stack[v->sp - 1] = b;
    v->stack[v->sp] = a;
}

static void h_rot(M_VM* v) {
    NEED(v, 3);
    M_Value a = v->stack[v->sp - 2];
    M_Value b = v->stack[v->sp - 1];
    M_Value c = v->stack[v->sp];
    v->stack[v->sp - 2] = b;
    v->stack[v->sp - 1] = c;
    v->stack[v->sp] = a;
}

/* --- Literal & Variables --- */

static void h_lit(M_VM* v) {
    int pc = v->pc;
    uint32_t enc = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &enc)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    SPACE(v, 1);
    v->pc = pc;
    PUSH(v, make_int((int32_t)enc));
}

static void h_v(M_VM* v) {
    /* Variable reference: V,<index> - local variable */
    int pc = v->pc;
    uint32_t idx = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &idx)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    SPACE(v, 1);
    CHECK_LOCALS(v, (int)idx);
    PUSH(v, v->locals[idx]);
}

static void h_let(M_VM* v) {
    int pc = v->pc;
    uint32_t idx = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &idx)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    NEED(v, 1);
    CHECK_LOCALS(v, idx);
    v->pc = pc;
    v->locals[idx] = POP(v);
}

static void h_set(M_VM* v) {
    int pc = v->pc;
    uint32_t idx = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &idx)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    NEED(v, 1);
    CHECK_GLOBALS(v, idx);
    v->pc = pc;
    v->globals[idx] = POP(v);
}

/* --- Arithmetic --- */

static void h_add(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a + b));
}

static void h_sub(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a - b));
}

static void h_mul(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a * b));
}

static void h_div(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    if (b == 0) { SET_FAULT(v, M_FAULT_DIV_BY_ZERO); return; }
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a / b));
}

static void h_and(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a & b));
}

static void h_or(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a | b));
}

static void h_xor(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a ^ b));
}

static void h_shl(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a << b));
}

static void h_shr(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a >> b));
}

/* --- Comparison --- */

static void h_lt(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a < b ? 1 : 0));
}

static void h_gt(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a > b ? 1 : 0));
}

static void h_le(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a <= b ? 1 : 0));
}

static void h_ge(M_VM* v) {
    NEED(v, 2);
    int32_t b = to_int(POP(v));
    int32_t a = to_int(POP(v));
    PUSH(v, make_int(a >= b ? 1 : 0));
}

static void h_eq(M_VM* v) {
    NEED(v, 2);
    M_Value b = POP(v);
    M_Value a = POP(v);
    int32_t result = 0;
    if (a.type == b.type) {
        switch (a.type) {
            case M_TYPE_INT: result = (a.u.i == b.u.i) ? 1 : 0; break;
            case M_TYPE_FLOAT: result = (a.u.f == b.u.f) ? 1 : 0; break;
            case M_TYPE_BOOL: result = (a.u.b == b.u.b) ? 1 : 0; break;
            default: result = 0;
        }
    }
    PUSH(v, make_int(result));
}

/* --- Array Operations --- */

static void h_len(M_VM* v) {
    NEED(v, 1);
    M_Value arr = POP(v);
    if (arr.type == M_TYPE_ARRAY) {
        PUSH(v, make_int(arr.u.arr.len));
    } else {
        PUSH(v, make_int(0));
    }
}

static void h_get(M_VM* v) {
    NEED(v, 2);
    int32_t idx = to_int(POP(v));
    M_Value arr = POP(v);
    if (arr.type == M_TYPE_ARRAY && idx >= 0 && idx < arr.u.arr.len) {
        PUSH(v, make_int(arr.u.arr.data[idx]));
    } else {
        SET_FAULT(v, M_FAULT_INDEX_OOB);
    }
}

static void h_put(M_VM* v) {
    NEED(v, 3);
    int32_t val = to_int(POP(v));
    int32_t idx = to_int(POP(v));
    M_Value arr = POP(v);
    if (arr.type == M_TYPE_ARRAY && idx >= 0 && idx < arr.u.arr.len) {
        arr.u.arr.data[idx] = val;
        PUSH(v, arr);
    } else {
        SET_FAULT(v, M_FAULT_INDEX_OOB);
    }
}

/* --- Control Flow --- */

static void h_b(M_VM* v) {
    /* Block begin - no operation, just a marker */
}

static void h_e(M_VM* v) {
    /* Block end - no operation, just a marker */
}

static void h_if(M_VM* v) {
    /* Format: <cond>,IF,B,<then>,E,B,<else>,E */
    NEED(v, 1);
    M_Value cond = POP(v);
    int start_pc = v->pc;
    
    /* Skip IF opcode */
    int pc = start_pc;
    uint32_t op = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &op);
    v->pc = pc;
    
    if (!to_bool(cond)) {
        /* Condition false, skip then branch, execute else */
        /* Find E,B (end of then, start of else) */
        int depth = 1;
        while (depth > 0 && pc < v->code_len) {
            uint32_t tok = 0;
            int next = pc;
            m_vm_decode_uvarint(v->code, &next, v->code_len, &tok);
            
            if (tok == M_B) depth++;
            else if (tok == M_E) depth--;
            
            if (depth > 0) pc = next;
        }
        /* Skip to else branch (after the E of then) */
        pc = v->pc = pc + 1;  /* Skip E */
        
        /* Skip else branch B */
        uint32_t next_tok = 0;
        m_vm_decode_uvarint(v->code, &pc, v->code_len, &next_tok);
        v->pc = pc;
    }
    /* If condition true, continue with then branch normally */
}

static void h_jz(M_VM* v) {
    /* Jump if zero: <cond>,JZ,<target_addr> */
    NEED(v, 1);
    int32_t cond = to_int(POP(v));
    int pc = v->pc;
    uint32_t target = 0;
    
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &target)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    
    if (cond == 0) {
        CHECK_PC(v, target);
        v->pc = (int)target;
    }
}

static void h_jmp(M_VM* v) {
    /* Unconditional jump: JMP,<target_addr> */
    int pc = v->pc;
    uint32_t target = 0;
    
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &target)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    
    CHECK_PC(v, target);
    v->pc = (int)target;
}

static void h_rt(M_VM* v) {
    CHECK_RET_POP(v);
    int32_t ret_addr = v->ret_stack[v->rp--];
    CHECK_PC(v, ret_addr);
    
    NEED(v, 1);
    M_Value ret_val = POP(v);
    
    /* Restore previous locals frame */
    CHECK_FRAME_POP(v);
    memcpy(v->locals, v->locals_frames[v->frame_sp--], sizeof(v->locals));
    
    v->pc = (int)ret_addr;
    PUSH(v, ret_val);
}

static void h_fn(M_VM* v) {
    /* Format: FN,<arity>,B<body>,E - function definition */
    /* This is a no-op at runtime - just skip the function body */
    int pc = v->pc;
    uint32_t arity = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &arity);
    
    /* Skip B */
    uint32_t tok = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &tok);
    
    /* Find matching E */
    int depth = 1;
    while (depth > 0 && pc < v->code_len) {
        uint32_t t = 0;
        int next = pc;
        m_vm_decode_uvarint(v->code, &next, v->code_len, &t);
        if (t == M_B) depth++;
        else if (t == M_E) depth--;
        if (depth > 0) pc = next;
    }
    v->pc = pc + 1;  /* Skip E */
}

static void h_cl(M_VM* v) {
    /* Call: CL,<func_id>,<argc>,<arg0>..<argN> */
    /* func_id is the FN address, we need to skip to the function body (after B) */
    int pc = v->pc;
    uint32_t func_id = 0;
    uint32_t argc = 0;
    
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &func_id) ||
        !m_vm_decode_uvarint(v->code, &pc, v->code_len, &argc)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    
    NEED(v, (int)argc);

    /* Save locals frame */
    CHECK_FRAME_PUSH(v);
    v->frame_sp++;
    memcpy(v->locals_frames[v->frame_sp], v->locals, sizeof(v->locals));
    memset(v->locals, 0, sizeof(v->locals));

    /* Bind arguments into locals[0..argc-1] */
    for (uint32_t i = 0; i < argc; i++) {
        v->locals[i] = POP(v);
    }

    /* Push return address (after CL instruction) */
    CHECK_RET_PUSH(v);
    v->ret_stack[++v->rp] = pc;
    
    /* Jump to function body (skip FN and B) */
    int fn_pc = (int)func_id;
    uint32_t op = 0;
    int next_pc = fn_pc;
    
    /* Skip FN opcode */
    m_vm_decode_uvarint(v->code, &next_pc, v->code_len, &op);
    /* Skip arity */
    m_vm_decode_uvarint(v->code, &next_pc, v->code_len, &op);
    /* Skip B, now next_pc is the function body start */
    m_vm_decode_uvarint(v->code, &next_pc, v->code_len, &op);
    
    CHECK_PC(v, next_pc);
    v->pc = next_pc;
}

static void h_halt(M_VM* v) {
    v->running = false;
}

static void h_gtway(M_VM* v) {
    int pc = v->pc;
    uint32_t key = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &key)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->authorized = (key == M_GATEWAY_KEY);
    v->pc = pc;
    if (!v->authorized) {
        SET_FAULT(v, M_FAULT_UNAUTHORIZED);
    }
}

static void h_wait(M_VM* v) {
    int pc = v->pc;
    uint32_t ms = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &ms)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    if (v->sleep_ms) v->sleep_ms((int32_t)ms);
    v->pc = pc;
}

static void h_iow(M_VM* v) {
    int pc = v->pc;
    uint32_t dev = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &dev)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    NEED(v, 1);
    if (!v->authorized) {
        SET_FAULT(v, M_FAULT_UNAUTHORIZED);
        return;
    }
    M_Value val = POP(v);
    if (v->io_write) v->io_write((uint8_t)dev, val);
    v->authorized = false;
    v->pc = pc;
}

static void h_ior(M_VM* v) {
    int pc = v->pc;
    uint32_t dev = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &dev)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    SPACE(v, 1);
    M_Value val = (v->io_read) ? v->io_read((uint8_t)dev) : make_int(0);
    v->pc = pc;
    PUSH(v, val);
}

static void h_trace(M_VM* v) {
    int pc = v->pc;
    uint32_t level = 0;
    if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &level)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    if (v->trace) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Trace level %u: sp=%d", (unsigned)level, v->sp);
        v->trace(level, msg);
    }
    v->pc = pc;
}

/* Placeholder */
static void h_ph(M_VM* v) {
    /* No operation */
}

/* =============================================
 * Gas Costs
 * ============================================= */

static const uint32_t GAS_COST[256] = {
    [M_LIT]   = 2,
    [M_V]     = 2,
    [M_LET]   = 2,
    [M_SET]   = 3,
    [M_ADD]   = 1,
    [M_SUB]   = 1,
    [M_MUL]   = 3,
    [M_DIV]   = 5,
    [M_AND]   = 1,
    [M_OR]    = 1,
    [M_XOR]   = 1,
    [M_SHL]   = 1,
    [M_SHR]   = 1,
    [M_LT]    = 1,
    [M_GT]    = 1,
    [M_LE]    = 1,
    [M_GE]    = 1,
    [M_EQ]    = 1,
    [M_DUP]   = 1,
    [M_DRP]   = 1,
    [M_SWP]   = 1,
    [M_ROT]   = 1,
    [M_LEN]   = 2,
    [M_GET]   = 2,
    [M_PUT]   = 3,
    [M_B]     = 0,
    [M_E]     = 0,
    [M_IF]    = 1,
    [M_RT]    = 2,
    [M_CL]    = 5,
    [M_HALT]  = 0,
    [M_GTWAY] = 1,
    [M_WAIT]  = 1,
    [M_IOW]   = 5,
    [M_IOR]   = 3,
    [M_TRACE] = 1,
    [M_PH]    = 0
};

/* =============================================
 * Instruction Jump Table
 * ============================================= */

typedef void (*handler)(M_VM*);

static const handler TABLE[256] = {
    [M_LIT]   = h_lit,
    [M_V]     = h_v,
    [M_LET]   = h_let,
    [M_SET]   = h_set,
    [M_ADD]   = h_add,
    [M_SUB]   = h_sub,
    [M_MUL]   = h_mul,
    [M_DIV]   = h_div,
    [M_AND]   = h_and,
    [M_OR]    = h_or,
    [M_XOR]   = h_xor,
    [M_SHL]   = h_shl,
    [M_SHR]   = h_shr,
    [M_LT]    = h_lt,
    [M_GT]    = h_gt,
    [M_LE]    = h_le,
    [M_GE]    = h_ge,
    [M_EQ]    = h_eq,
    [M_DUP]   = h_dup,
    [M_DRP]   = h_drp,
    [M_SWP]   = h_swp,
    [M_ROT]   = h_rot,
    [M_LEN]   = h_len,
    [M_GET]   = h_get,
    [M_PUT]   = h_put,
    [M_B]     = h_b,
    [M_E]     = h_e,
    [M_IF]    = h_if,
    [M_JZ]    = h_jz,
    [M_JMP]   = h_jmp,
    [M_FN]    = h_fn,
    [M_RT]    = h_rt,
    [M_CL]    = h_cl,
    [M_HALT]  = h_halt,
    [M_GTWAY] = h_gtway,
    [M_WAIT]  = h_wait,
    [M_IOW]   = h_iow,
    [M_IOR]   = h_ior,
    [M_TRACE] = h_trace,
    [M_PH]    = h_ph
};

/* =============================================
 * Core Interface Implementation
 * ============================================= */

void m_vm_init(M_VM* vm, uint8_t* code, int len,
               void (*io_w)(uint8_t, M_Value),
               M_Value (*io_r)(uint8_t),
               void (*sleep)(int32_t),
               void (*trace)(uint32_t, const char*)) {
    memset(vm, 0, sizeof(M_VM));
    vm->code = code;
    vm->code_len = len;
    vm->pc = 0;
    vm->sp = -1;
    vm->rp = -1;
    vm->running = false;
    vm->authorized = false;
    vm->fault = M_FAULT_NONE;
    vm->steps = 0;
    vm->step_limit = MAX_STEPS;
    vm->gas = 0;
    vm->gas_limit = 0;
    vm->local_count = 0;
    vm->frame_sp = -1;
    
    vm->io_write = io_w;
    vm->io_read = io_r;
    vm->sleep_ms = sleep;
    vm->trace = trace;
}

void m_vm_set_step_limit(M_VM* vm, uint64_t limit) { 
    vm->step_limit = limit; 
}

void m_vm_set_gas_limit(M_VM* vm, uint64_t limit) { 
    vm->gas_limit = limit; 
}

void m_vm_reset(M_VM* vm) {
    uint8_t* code = vm->code;
    int len = vm->code_len;
    void (*io_w)(uint8_t, M_Value) = vm->io_write;
    M_Value (*io_r)(uint8_t) = vm->io_read;
    void (*sleep)(int32_t) = vm->sleep_ms;
    void (*trace)(uint32_t, const char*) = vm->trace;
    uint64_t step_limit = vm->step_limit;
    uint64_t gas_limit = vm->gas_limit;

    memset(vm, 0, sizeof(M_VM));
    vm->code = code;
    vm->code_len = len;
    vm->pc = 0;
    vm->sp = -1;
    vm->rp = -1;
    vm->running = false;
    vm->authorized = false;
    vm->fault = M_FAULT_NONE;
    vm->steps = 0;
    vm->step_limit = step_limit;
    vm->gas = 0;
    vm->gas_limit = gas_limit;
    vm->local_count = 0;
    vm->frame_sp = -1;

    vm->io_write = io_w;
    vm->io_read = io_r;
    vm->sleep_ms = sleep;
    vm->trace = trace;
}

M_VM_State m_vm_get_state(M_VM* vm) {
    if (vm->fault != M_FAULT_NONE) return M_STATE_FAULT;
    if (vm->running) return M_STATE_RUNNING;
    return M_STATE_STOPPED;
}

int m_vm_step(M_VM* vm) {
    if (!vm->running) {
        return vm->fault ? -(int)vm->fault : 1;
    }

    if (vm->pc < 0 || vm->pc >= vm->code_len) {
        SET_FAULT(vm, M_FAULT_PC_OOB);
        return -(int)vm->fault;
    }

    vm->steps++;
    if (vm->step_limit > 0 && vm->steps > vm->step_limit) {
        SET_FAULT(vm, M_FAULT_STEP_LIMIT);
        return -(int)vm->fault;
    }

    /* Fetch instruction (always varint) */
    uint32_t op = 0;
    int pc = vm->pc;
    if (!m_vm_decode_uvarint(vm->code, &pc, vm->code_len, &op)) {
        SET_FAULT(vm, M_FAULT_BAD_ENCODING);
        return -(int)vm->fault;
    }
    vm->pc = pc;
    vm->last_op = op;

    if (op > 255) {
        SET_FAULT(vm, M_FAULT_UNKNOWN_OP);
        return -(int)vm->fault;
    }

    uint8_t op8 = (uint8_t)op;

    if (!TABLE[op8]) {
        SET_FAULT(vm, M_FAULT_UNKNOWN_OP);
        return -(int)vm->fault;
    }

    /* Gas billing */
    if (vm->gas_limit > 0) {
        vm->gas += GAS_COST[op8];
        if (vm->gas > vm->gas_limit) {
            SET_FAULT(vm, M_FAULT_GAS_EXHAUSTED);
            return -(int)vm->fault;
        }
    }

    TABLE[op8](vm);

    if (!vm->running) {
        return vm->fault ? -(int)vm->fault : 1;
    }
    return 0;
}

int m_vm_run(M_VM* vm) {
    vm->pc = 0;
    vm->sp = -1;
    vm->rp = -1;
    memset(vm->locals, 0, sizeof(vm->locals));
    memset(vm->globals, 0, sizeof(vm->globals));
    vm->frame_sp = -1;
    vm->fault = M_FAULT_NONE;
    vm->steps = 0;
    vm->gas = 0;
    vm->authorized = false;
    vm->running = true;

    while (vm->running && vm->pc < vm->code_len) {
        int r = m_vm_step(vm);
        if (r != 0) return r;
    }

    vm->running = false;
    return vm->fault ? -(int)vm->fault : 1;
}

int m_vm_simulate(M_VM* vm, M_SimResult* result) {
    if (!result) return -1;

    memset(result, 0, sizeof(M_SimResult));

    m_vm_reset(vm);
    vm->running = true;

    while (vm->running && vm->pc < vm->code_len) {
        int prev_pc = vm->pc;
        int r = m_vm_step(vm);

        if (result->trace_len < MAX_TRACE) {
            M_TraceEntry* e = &result->trace[result->trace_len++];
            e->step = vm->steps;
            e->pc = prev_pc;
            e->op = vm->last_op;
            e->sp = vm->sp;
            e->stack_top = (vm->sp >= 0) ? vm->stack[vm->sp].u.i : 0;
        }

        if (r != 0) {
            result->halted = true;
            result->fault = vm->fault;
            result->steps = vm->steps;
            result->sp = vm->sp;
            if (vm->sp >= 0) result->result = vm->stack[vm->sp].u.i;
            result->completed = (vm->fault == M_FAULT_NONE);
            return r;
        }
    }

    result->halted = true;
    result->fault = vm->fault;
    result->steps = vm->steps;
    result->sp = vm->sp;
    if (vm->sp >= 0) result->result = vm->stack[vm->sp].u.i;
    result->completed = (vm->fault == M_FAULT_NONE);
    return result->completed ? 1 : -(int)result->fault;
}

/* =============================================
 * High-Level API
 * ============================================= */

int m_vm_call(M_VM* vm, uint32_t func_id, int argc, M_Value* args) {
    /* Push arguments in reverse order */
    for (int i = argc - 1; i >= 0; i--) {
        PUSH(vm, args[i]);
    }
    
    /* Push return address */
    CHECK_RET_PUSH(vm);
    vm->ret_stack[vm->rp] = vm->code_len;  /* Return to end */
    
    /* Jump to function */
    CHECK_PC(vm, func_id);
    vm->pc = func_id;
    vm->running = true;
    
    return 0;
}

int m_vm_exec_block(M_VM* vm, int start_pc, int end_pc) {
    int saved_pc = vm->pc;
    vm->pc = start_pc;
    vm->running = true;
    
    while (vm->running && vm->pc < end_pc) {
        int r = m_vm_step(vm);
        if (r != 0) {
            vm->pc = saved_pc;
            return r;
        }
    }
    
    vm->pc = saved_pc;
    return 0;
}

/* =============================================
 * Fault & Debug
 * ============================================= */

const char* m_vm_fault_string(M_Fault fault) {
    switch (fault) {
        case M_FAULT_NONE:                return "NONE";
        case M_FAULT_STACK_OVERFLOW:      return "STACK_OVERFLOW";
        case M_FAULT_STACK_UNDERFLOW:     return "STACK_UNDERFLOW";
        case M_FAULT_RET_STACK_OVERFLOW:  return "RET_STACK_OVERFLOW";
        case M_FAULT_RET_STACK_UNDERFLOW: return "RET_STACK_UNDERFLOW";
        case M_FAULT_LOCALS_OOB:          return "LOCALS_OOB";
        case M_FAULT_GLOBALS_OOB:         return "GLOBALS_OOB";
        case M_FAULT_PC_OOB:              return "PC_OOB";
        case M_FAULT_DIV_BY_ZERO:         return "DIV_BY_ZERO";
        case M_FAULT_MOD_BY_ZERO:         return "MOD_BY_ZERO";
        case M_FAULT_UNKNOWN_OP:          return "UNKNOWN_OP";
        case M_FAULT_STEP_LIMIT:          return "STEP_LIMIT";
        case M_FAULT_GAS_EXHAUSTED:       return "GAS_EXHAUSTED";
        case M_FAULT_BAD_ENCODING:        return "BAD_ENCODING";
        case M_FAULT_UNAUTHORIZED:        return "UNAUTHORIZED";
        case M_FAULT_TYPE_MISMATCH:       return "TYPE_MISMATCH";
        case M_FAULT_INDEX_OOB:           return "INDEX_OOB";
        case M_FAULT_ASSERT_FAILED:       return "ASSERT_FAILED";
        default:                          return "UNKNOWN";
    }
}

const char* m_vm_opcode_name(uint32_t op) {
    switch (op) {
        /* Control Flow */
        case M_FN:   return "FN";
        case M_B:    return "B";
        case M_E:    return "E";
        case M_IF:   return "IF";
        case M_JZ:   return "JZ";
        case M_JMP:  return "JMP";
        case M_RT:   return "RT";
        case M_CL:   return "CL";
        case M_PH:   return "PH";
        
        /* Data */
        case M_LIT:  return "LIT";
        case M_V:    return "V";
        case M_LET:  return "LET";
        case M_SET:  return "SET";
        
        /* Comparison */
        case M_LT:   return "LT";
        case M_GT:   return "GT";
        case M_LE:   return "LE";
        case M_GE:   return "GE";
        case M_EQ:   return "EQ";
        
        /* Arithmetic */
        case M_ADD:  return "ADD";
        case M_SUB:  return "SUB";
        case M_MUL:  return "MUL";
        case M_DIV:  return "DIV";
        case M_AND:  return "AND";
        case M_OR:   return "OR";
        case M_XOR:  return "XOR";
        case M_SHL:  return "SHL";
        case M_SHR:  return "SHR";
        
        /* Array */
        case M_LEN:  return "LEN";
        case M_GET:  return "GET";
        case M_PUT:  return "PUT";
        case M_SWP:  return "SWP";
        
        /* Stack */
        case M_DUP:  return "DUP";
        case M_DRP:  return "DRP";
        case M_ROT:  return "ROT";
        
        /* IO */
        case M_IOW:  return "IOW";
        case M_IOR:  return "IOR";
        
        /* System */
        case M_GTWAY: return "GTWAY";
        case M_WAIT:  return "WAIT";
        case M_HALT:  return "HALT";
        case M_TRACE: return "TRACE";
        
        default:     return "UNK";
    }
}

int m_vm_stack_snapshot(M_VM* vm, M_Value* out_stack) {
    if (!out_stack) return 0;
    int count = vm->sp + 1;
    if (count < 0) count = 0;
    if (count > STACK_SIZE) count = STACK_SIZE;
    memcpy(out_stack, vm->stack, (size_t)count * sizeof(M_Value));
    return count;
}
