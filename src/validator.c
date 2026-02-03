#include <string.h>
#include "validator.h"
#include "m_vm.h"

/* Helper to set validator result */
static void set_result(M_ValidatorResult* result, bool valid, int fault_code, int pc, const char* msg) {
    result->valid = valid;
    result->fault_code = fault_code;
    result->pc = pc;
    if (msg) {
        strncpy(result->msg, msg, sizeof(result->msg) - 1);
        result->msg[sizeof(result->msg) - 1] = '\0';
    } else {
        result->msg[0] = '\0';
    }
}

void m_validator_result_init(M_ValidatorResult* result) {
    set_result(result, true, 0, 0, NULL);
}

static bool is_valid_opcode(uint32_t op) {
    /* Allow Core + Extension + Platform + Experimental (0-255) per spec */
    return op <= 255;
}

static bool has_handler(uint32_t op) {
    /* Check if opcode has a handler in the TABLE */
    /* This is a simplified check - real implementation would check TABLE[op] != NULL */
    return true;  /* For now, accept all valid opcodes */
}

typedef struct {
    uint32_t op;
    int start;
    int end;
    uint64_t u64;
    uint32_t u32;
    uint32_t u32_b;
    int32_t s32;
    uint8_t mask; /* bit0:u32, bit1:u32_b, bit2:u64, bit3:s32 */
} ValTok;

static bool read_valtok(const uint8_t* code, int len, int* pc, ValTok* out) {
    int p = *pc;
    uint32_t op = 0;
    if (!m_vm_decode_uvarint(code, &p, len, &op)) return false;
    out->op = op;
    out->start = *pc;
    out->mask = 0;

    switch (op) {
        case M_LIT: {
            uint64_t v = 0;
            if (!m_vm_decode_uvarint64(code, &p, len, &v)) return false;
            out->u64 = v;
            out->mask |= 0x4;
            break;
        }
        case M_V:
        case M_LET:
        case M_SET:
        case M_GTWAY:
        case M_WAIT:
        case M_IOW:
        case M_IOR:
        case M_TRACE:
        case M_BP: {
            uint32_t v = 0;
            if (!m_vm_decode_uvarint(code, &p, len, &v)) return false;
            out->u32 = v;
            out->mask |= 0x1;
            break;
        }
        case M_CL: {
            uint32_t f = 0;
            uint32_t a = 0;
            if (!m_vm_decode_uvarint(code, &p, len, &f)) return false;
            if (!m_vm_decode_uvarint(code, &p, len, &a)) return false;
            out->u32 = f;
            out->u32_b = a;
            out->mask |= 0x3;
            break;
        }
        case M_JZ:
        case M_JNZ:
        case M_JMP:
        case M_DWHL:
        case M_WHIL: {
            int32_t off = 0;
            if (!m_vm_decode_svarint(code, &p, len, &off)) return false;
            out->s32 = off;
            out->mask |= 0x8;
            break;
        }
        default:
            break;
    }

    out->end = p;
    *pc = p;
    return true;
}

static bool build_val_tokens(const uint8_t* code, int len, ValTok** out, int* out_count) {
    int pc = 0;
    int cap = 0;
    int count = 0;
    ValTok* toks = NULL;
    while (pc < len) {
        ValTok t;
        if (!read_valtok(code, len, &pc, &t)) { free(toks); return false; }
        if (count + 1 > cap) {
            int new_cap = (cap == 0) ? 256 : cap * 2;
            ValTok* n = (ValTok*)realloc(toks, (size_t)new_cap * sizeof(ValTok));
            if (!n) { free(toks); return false; }
            toks = n;
            cap = new_cap;
        }
        toks[count++] = t;
    }
    *out = toks;
    *out_count = count;
    return true;
}

static int find_matching_e(ValTok* toks, int count, int b_idx) {
    int depth = 0;
    for (int i = b_idx; i < count; i++) {
        if (toks[i].op == M_B) depth++;
        else if (toks[i].op == M_E) depth--;
        if (depth == 0) return i;
    }
    return -1;
}

static void caps_and(uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 32; i++) a[i] &= b[i];
}

static bool caps_has(const uint8_t* caps, uint32_t id) {
    if (id > 255) return false;
    return (caps[id >> 3] & (uint8_t)(1u << (id & 7))) != 0;
}

static void caps_add(uint8_t* caps, uint32_t id) {
    if (id > 255) return;
    caps[id >> 3] |= (uint8_t)(1u << (id & 7));
}

static bool validate_range(ValTok* toks, int count, int start, int end,
                           int* sp, uint8_t* caps, M_ValidatorResult* result) {
    for (int i = start; i < end; i++) {
        uint32_t op = toks[i].op;

        switch (op) {
            case M_IF: {
                if (*sp < 1) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at IF"); return false; }
                (*sp)--;
                if (i + 1 >= count || toks[i + 1].op != M_B) { set_result(result, false, M_FAULT_BAD_ENCODING, toks[i].start, "IF missing B"); return false; }
                int then_e = find_matching_e(toks, count, i + 1);
                if (then_e < 0) { set_result(result, false, M_FAULT_BAD_ENCODING, toks[i].start, "IF missing then E"); return false; }
                if (then_e + 1 >= count || toks[then_e + 1].op != M_B) { set_result(result, false, M_FAULT_BAD_ENCODING, toks[i].start, "IF missing else B"); return false; }
                int else_e = find_matching_e(toks, count, then_e + 1);
                if (else_e < 0) { set_result(result, false, M_FAULT_BAD_ENCODING, toks[i].start, "IF missing else E"); return false; }

                int sp_then = *sp;
                int sp_else = *sp;
                uint8_t caps_then[32]; memcpy(caps_then, caps, 32);
                uint8_t caps_else[32]; memcpy(caps_else, caps, 32);
                if (!validate_range(toks, count, i + 2, then_e, &sp_then, caps_then, result)) return false;
                if (!validate_range(toks, count, then_e + 2, else_e, &sp_else, caps_else, result)) return false;
                if (sp_then != sp_else) { set_result(result, false, M_FAULT_BAD_ARG, toks[i].start, "IF branch stack mismatch"); return false; }
                caps_and(caps_then, caps_else);
                memcpy(caps, caps_then, 32);
                *sp = sp_then;
                i = else_e;
                break;
            }
            case M_WH:
            case M_FR: {
                if (*sp < 1) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at WH/FR"); return false; }
                (*sp)--;
                if (i + 1 >= count || toks[i + 1].op != M_B) { set_result(result, false, M_FAULT_BAD_ENCODING, toks[i].start, "WH/FR missing B"); return false; }
                int body_e = find_matching_e(toks, count, i + 1);
                if (body_e < 0) { set_result(result, false, M_FAULT_BAD_ENCODING, toks[i].start, "WH/FR missing E"); return false; }
                int sp_body = *sp;
                uint8_t caps_body[32]; memcpy(caps_body, caps, 32);
                if (!validate_range(toks, count, i + 2, body_e, &sp_body, caps_body, result)) return false;
                if (sp_body != *sp) { set_result(result, false, M_FAULT_BAD_ARG, toks[i].start, "Loop body stack effect != 0"); return false; }
                /* caps after loop are those available before loop (loop may not run) */
                i = body_e;
                break;
            }
            case M_JZ:
            case M_JNZ:
            case M_JMP: {
                int target = (i + 1) + toks[i].s32;
                if (target < 0 || target >= count) { set_result(result, false, M_FAULT_PC_OOB, toks[i].start, "Jump target out of bounds"); return false; }
                if (op != M_JMP) {
                    if (*sp < 1) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at JZ/JNZ"); return false; }
                    (*sp)--;
                }
                break;
            }
            case M_LIT:
            case M_V:
                (*sp)++;
                break;
            case M_LEN:
            case M_NEG:
            case M_NOT:
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow"); return false; }
                /* pop then push (net 0) */
                break;
            case M_DUP:
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at DUP"); return false; }
                (*sp)++;
                break;
            case M_DRP:
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at DRP"); return false; }
                (*sp)--;
                break;
            case M_SWP:
                if (*sp < 1) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at SWP"); return false; }
                break;
            case M_ROT:
                if (*sp < 2) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at ROT"); return false; }
                break;
            case M_GET:
            case M_IDX:
                if (*sp < 1) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at GET/IDX"); return false; }
                /* pop2 push1 => sp-- */
                (*sp)--;
                break;
            case M_PUT:
            case M_STO:
                if (*sp < 2) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at PUT/STO"); return false; }
                /* pop3 push1 => sp -=2 */
                (*sp) -= 2;
                break;
            case M_NEWARR:
            case M_ALLOC:
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at NEWARR/ALLOC"); return false; }
                /* pop1 push1 => no change */
                break;
            case M_FREE:
            case M_LET:
            case M_SET:
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at op"); return false; }
                (*sp)--;
                break;
            case M_ADD: case M_SUB: case M_MUL: case M_DIV:
            case M_AND: case M_OR: case M_XOR: case M_SHL: case M_SHR:
            case M_LT:  case M_GT:  case M_LE:  case M_GE:  case M_EQ:  case M_NEQ:
            case M_MOD:
                if (*sp < 1) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at binary op"); return false; }
                (*sp)--;
                break;
            case M_CL:
                if (*sp < (int)toks[i].u32_b) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at CL"); return false; }
                (*sp) -= (int)toks[i].u32_b;
                (*sp)++;
                break;
            case M_RT:
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at RT"); return false; }
                (*sp)--;
                break;
            case M_GTWAY:
                if (toks[i].u32 > 255) { set_result(result, false, M_FAULT_BAD_ARG, toks[i].start, "GTWAY cap_id out of range"); return false; }
                caps_add(caps, toks[i].u32);
                break;
            case M_IOW:
                if (!caps_has(caps, toks[i].u32)) { set_result(result, false, M_FAULT_UNAUTHORIZED, toks[i].start, "IOW without capability"); return false; }
                if (*sp < 0) { set_result(result, false, M_FAULT_STACK_UNDERFLOW, toks[i].start, "Stack underflow at IOW"); return false; }
                (*sp)--;
                break;
            case M_IOR:
                if (!caps_has(caps, toks[i].u32)) { set_result(result, false, M_FAULT_UNAUTHORIZED, toks[i].start, "IOR without capability"); return false; }
                (*sp)++;
                break;
            default:
                break;
        }
    }
    return true;
}

static bool validate_reachability(ValTok* toks, int count, M_ValidatorResult* result) {
    if (count <= 0) return true;

    bool* reachable = (bool*)calloc((size_t)count, sizeof(bool));
    int* queue = (int*)malloc((size_t)count * sizeof(int));
    if (!reachable || !queue) {
        free(reachable);
        free(queue);
        set_result(result, false, M_FAULT_OOM, 0, "Reachability allocation failed");
        return false;
    }

    int qh = 0, qt = 0;
    reachable[0] = true;
    queue[qt++] = 0;

    while (qh < qt) {
        int i = queue[qh++];
        uint32_t op = toks[i].op;

#define ENQUEUE(idx) do { \
    int _idx = (idx); \
    if (_idx >= 0 && _idx < count && !reachable[_idx]) { \
        reachable[_idx] = true; \
        queue[qt++] = _idx; \
    } \
} while(0)

        if (op == M_JMP) {
            int target = (i + 1) + toks[i].s32;
            if (target < 0 || target >= count) {
                set_result(result, false, M_FAULT_PC_OOB, toks[i].start, "Jump target out of bounds");
                free(reachable);
                free(queue);
                return false;
            }
            ENQUEUE(target);
            continue;
        }

        if (op == M_JZ || op == M_JNZ || op == M_DWHL || op == M_WHIL) {
            int target = (i + 1) + toks[i].s32;
            if (target < 0 || target >= count) {
                set_result(result, false, M_FAULT_PC_OOB, toks[i].start, "Jump target out of bounds");
                free(reachable);
                free(queue);
                return false;
            }
            ENQUEUE(target);
            ENQUEUE(i + 1);
            continue;
        }

        if (op == M_HALT || op == M_RT) {
            continue;
        }

        ENQUEUE(i + 1);
    }
#undef ENQUEUE

    for (int i = 0; i < count; i++) {
        if (!reachable[i]) {
            set_result(result, false, M_FAULT_BAD_ARG, toks[i].start, "Unreachable code");
            free(reachable);
            free(queue);
            return false;
        }
    }

    free(reachable);
    free(queue);
    return true;
}

bool m_validate_opcodes(const uint8_t* code, int len, M_ValidatorResult* result) {
    int pc = 0;
    
    while (pc < len) {
        uint32_t op = 0;
        int next = pc;
        
        /* Try to decode opcode */
        if (!m_vm_decode_uvarint(code, &next, len, &op)) {
            set_result(result, false, M_FAULT_BAD_ENCODING, pc, "Invalid opcode encoding");
            return false;
        }
        
        /* Check opcode range */
        if (!is_valid_opcode(op)) {
            set_result(result, false, M_FAULT_UNKNOWN_OP, pc, "Unknown opcode");
            return false;
        }
        
        pc = next;
    }
    
    return true;
}

bool m_validate_varints(const uint8_t* code, int len, M_ValidatorResult* result) {
    int pc = 0;
    
    while (pc < len) {
        uint32_t op = 0;
        int next = pc;
        
        if (!m_vm_decode_uvarint(code, &next, len, &op)) {
            set_result(result, false, M_FAULT_BAD_ENCODING, pc, "Invalid varint encoding");
            return false;
        }
        
        pc = next;
    }
    
    return true;
}

static int count_args_for_opcode(uint32_t op) {
    /* Return number of arguments for each opcode */
    /* -1 means variable, 0 means no args, positive means fixed count */
    switch (op) {
        /* Control flow - no args */
        case M_B: case M_E: case M_PH:
        /* System - no args */
        case M_HALT: case M_GC: case M_STEP:
        /* Stack ops - no args */
        case M_DUP: case M_DRP: case M_ROT:
            return 0;
        
        /* Literal - 1 arg (zigzag i64) */
        case M_LIT:
            return 1;
        
        /* Variable access - 1 arg (index) */
        case M_V: case M_LET:
            return 1;
        
        /* Global set - 2 args (name_id, value) */
        case M_SET:
            return 2;
        
        /* Binary ops - 2 args */
        case M_ADD: case M_SUB: case M_MUL: case M_DIV: case M_AND:
        case M_OR: case M_XOR: case M_SHL: case M_SHR:
        case M_LT: case M_GT: case M_LE: case M_GE: case M_EQ: case M_NEQ:
        case M_MOD:
            return 2;
        
        /* Unary ops - 1 arg */
        case M_NEG: case M_NOT:
            return 1;
        
        /* Array ops - varies */
        case M_LEN:
            return 0;
        case M_GET: case M_IDX:
            return 1;
        case M_PUT: case M_STO:
            return 2;
        case M_SWP:
            return 0;
        
        /* Memory - 1 arg */
        case M_ALLOC: case M_FREE:
            return 1;
        
        /* Control flow with args */
        case M_IF: case M_WH: case M_FR:
            /* These consume condition on stack, no bytecode args */
            return 0;
        
        case M_FN:
            /* FN,<arity>,B<body>,E - 1 arg (arity) */
            return 1;
        
        case M_RT:
            /* RT,<value> - 1 arg */
            return 1;
        
        case M_CL:
            /* CL,<func_id>,<argc>,<args...> - 2 + argc args */
            return -2;  /* Variable - need to read argc */
        
        /* IO - 2 args (device_id, value/data) */
        case M_IOW: case M_IOR:
            return 1;  /* device_id only, value on stack */
        
        /* System with arg */
        case M_GTWAY: case M_WAIT: case M_TRACE: case M_BP:
            return 1;
        
        /* Extension jumps - 1 arg (offset) */
        case M_JZ: case M_JNZ: case M_JMP:
            return 1;
        
        /* Legacy loop constructs */
        case M_DO: case M_DWHL: case M_WHIL:
            return 0;
        
        default:
            return 0;  /* Assume no args for unknown */
    }
}

bool m_validate_blocks(const uint8_t* code, int len, M_ValidatorResult* result) {
    int pc = 0;
    int block_depth = 0;
    
    /* Track structured blocks: IF, WH, FR, FN */
    int struct_depth = 0;
    
    while (pc < len) {
        uint32_t op = 0;
        int next = pc;
        
        if (!m_vm_decode_uvarint(code, &next, len, &op)) {
            set_result(result, false, M_FAULT_BAD_ENCODING, pc, "Invalid varint in block check");
            return false;
        }
        
        switch (op) {
            case M_B:
                block_depth++;
                struct_depth++;
                break;
            case M_E:
                if (block_depth <= 0) {
                    set_result(result, false, M_FAULT_PC_OOB, pc, "Unmatched E");
                    return false;
                }
                block_depth--;
                struct_depth--;
                break;
            default:
                break;
        }
        
        pc = next;
    }
    
    if (block_depth != 0) {
        set_result(result, false, M_FAULT_PC_OOB, pc, "Unmatched B/E");
        return false;
    }
    
    return true;
}

bool m_validate_locals(const uint8_t* code, int len, M_ValidatorResult* result) {
    /* Static local validation is complex - for now, we just check
     * that V and LET indices are within reasonable bounds */
    int pc = 0;
    
    while (pc < len) {
        uint32_t op = 0;
        int next = pc;
        
        if (!m_vm_decode_uvarint(code, &next, len, &op)) {
            set_result(result, false, M_FAULT_BAD_ENCODING, pc, "Invalid varint in locals check");
            return false;
        }
        
        if (op == M_V || op == M_LET || op == M_SET) {
            uint32_t idx = 0;
            int arg_pc = next;
            if (!m_vm_decode_uvarint(code, &arg_pc, len, &idx)) {
                set_result(result, false, M_FAULT_BAD_ENCODING, pc, "Invalid local index encoding");
                return false;
            }
            
            if (op == M_SET) {
                if (idx >= GLOBALS_SIZE) {
                    set_result(result, false, M_FAULT_GLOBALS_OOB, pc, "Global index out of bounds");
                    return false;
                }
            } else {
                if (idx >= LOCALS_SIZE) {
                    set_result(result, false, M_FAULT_LOCALS_OOB, pc, "Local index out of bounds");
                    return false;
                }
            }
        }
        
        pc = next;
    }
    
    return true;
}

M_ValidatorResult m_validate(const uint8_t* code, int len) {
    M_ValidatorResult result;
    m_validator_result_init(&result);
    
    if (!code || len <= 0) {
        set_result(&result, false, M_FAULT_BAD_ENCODING, 0, "Invalid code or length");
        return result;
    }
    
    /* Run all checks */
    if (!m_validate_opcodes(code, len, &result)) {
        return result;
    }
    
    if (!m_validate_varints(code, len, &result)) {
        return result;
    }
    
    if (!m_validate_blocks(code, len, &result)) {
        return result;
    }
    
    if (!m_validate_locals(code, len, &result)) {
        return result;
    }

    /* Structured, jump, stack, and capability checks */
    ValTok* toks = NULL;
    int tok_count = 0;
    if (!build_val_tokens(code, len, &toks, &tok_count)) {
        set_result(&result, false, M_FAULT_BAD_ENCODING, 0, "Tokenization failed");
        return result;
    }
    int sp = 0;
    uint8_t caps[32] = {0};
    if (!validate_range(toks, tok_count, 0, tok_count, &sp, caps, &result)) {
        free(toks);
        return result;
    }
    if (!validate_reachability(toks, tok_count, &result)) {
        free(toks);
        return result;
    }
    free(toks);
    
    return result;
}

M_ValidatorResult m_validate_core_only(const uint8_t* code, int len) {
    M_ValidatorResult result = m_validate(code, len);
    if (!result.valid) return result;

    ValTok* toks = NULL;
    int tok_count = 0;
    if (!build_val_tokens(code, len, &toks, &tok_count)) {
        set_result(&result, false, M_FAULT_BAD_ENCODING, 0, "Tokenization failed");
        return result;
    }
    for (int i = 0; i < tok_count; i++) {
        if (toks[i].op > 99) {
            set_result(&result, false, M_FAULT_UNKNOWN_OP, toks[i].start, "Non-core opcode in core-only validation");
            free(toks);
            return result;
        }
    }
    free(toks);
    return result;
}

