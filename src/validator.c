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
        
        if (op == M_V || op == M_LET) {
            uint32_t idx = 0;
            int arg_pc = next;
            if (!m_vm_decode_uvarint(code, &arg_pc, len, &idx)) {
                set_result(result, false, M_FAULT_BAD_ENCODING, pc, "Invalid local index encoding");
                return false;
            }
            
            if (idx >= LOCALS_SIZE) {
                set_result(result, false, M_FAULT_LOCALS_OOB, pc, "Local index out of bounds");
                return false;
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
    
    return result;
}

