/**
 * M Language Disassembler - M-Token Edition
 * 
 * Features:
 * 1. Disassemble M-Token bytecode to human-readable mnemonics
 * 2. Show byte offset and raw bytes for each instruction
 * 3. Support structured control flow (FN, IF, B, E)
 * 4. Stack state visualization
 * 5. Execution trace analysis
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include "m_vm.h"

/* =============================================
 * Disassembler Configuration
 * ============================================= */

#define MAX_DISASM_LEN     8192
#define MAX_LABELS         256
#define MAX_INDENT         16

/* Label types */
typedef enum {
    LABEL_NONE = 0,
    LABEL_FUNC,         /* Function entry */
    LABEL_BLOCK,        /* Block entry */
    LABEL_JUMP_IN,      /* Jump target */
    LABEL_CALL_TARGET   /* Call target */
} LabelType;

/* Address label */
typedef struct {
    int      addr;           /* Address */
    char     name[32];       /* Label name */
    LabelType type;          /* Label type */
    bool     used;           /* Whether referenced */
} Label;

/* Disassembly context */
typedef struct {
    const uint8_t* code;     /* Bytecode */
    int           len;       /* Bytecode length */
    Label         labels[MAX_LABELS]; /* Label table */
    int           label_count;
    char          output[MAX_DISASM_LEN]; /* Output buffer */
    int           out_pos;
    int           indent;         /* Current indent level */
} DisasmContext;

/* =============================================
 * Label Management
 * ============================================= */

static void disasm_add_label(DisasmContext* ctx, int addr, const char* name, LabelType type) {
    if (ctx->label_count >= MAX_LABELS) return;
    
    Label* l = &ctx->labels[ctx->label_count++];
    l->addr = addr;
    l->type = type;
    l->used = false;
    strncpy_s(l->name, sizeof(l->name), name, sizeof(l->name) - 1);
}

static Label* disasm_find_label(DisasmContext* ctx, int addr) {
    for (int i = 0; i < ctx->label_count; i++) {
        if (ctx->labels[i].addr == addr) {
            ctx->labels[i].used = true;
            return &ctx->labels[i];
        }
    }
    return NULL;
}

/* =============================================
 * Pass 1: Scan for labels
 * ============================================= */

static void disasm_scan_labels(DisasmContext* ctx) {
    ctx->label_count = 0;
    
    int pc = 0;
    while (pc < ctx->len) {
        int start_pc = pc;
        uint32_t op = 0;
        
        if (!m_vm_decode_uvarint(ctx->code, &pc, ctx->len, &op)) {
            break;
        }
        
        switch (op) {
            case M_FN: {
                /* FN,<arity>,B,<body>,E - function definition */
                uint32_t arity = 0;
                int pc2 = pc;
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &arity);
                /* Skip B */
                uint32_t tok = 0;
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &tok);
                /* Find matching E */
                int depth = 1;
                while (depth > 0 && pc2 < ctx->len) {
                    uint32_t t = 0;
                    int next = pc2;
                    m_vm_decode_uvarint(ctx->code, &next, ctx->len, &t);
                    if (t == M_B) depth++;
                    else if (t == M_E) depth--;
                    if (depth > 0) pc2 = next;
                }
                char name[32];
                snprintf(name, sizeof(name), "func_%d", start_pc);
                disasm_add_label(ctx, start_pc, name, LABEL_FUNC);
                pc = pc2;
                break;
            }
            case M_CL: {
                /* CL,<func_id>,<argc> - function call */
                uint32_t func_id = 0;
                int pc2 = pc;
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &func_id);
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &func_id); /* argc */
                char name[32];
                snprintf(name, sizeof(name), "func_%u", (unsigned)func_id);
                disasm_add_label(ctx, (int)func_id, name, LABEL_CALL_TARGET);
                pc = pc2;
                break;
            }
            case M_IF: {
                /* <cond>,IF,B,<then>,E,B,<else>,E - conditional */
                int pc2 = pc;
                /* Skip B */
                uint32_t tok = 0;
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &tok);
                /* Find then block E */
                int depth = 1;
                while (depth > 0 && pc2 < ctx->len) {
                    uint32_t t = 0;
                    int next = pc2;
                    m_vm_decode_uvarint(ctx->code, &next, ctx->len, &t);
                    if (t == M_B) depth++;
                    else if (t == M_E) depth--;
                    if (depth > 0) pc2 = next;
                }
                /* Add else label */
                int else_addr = pc2 + 1;
                char name[32];
                snprintf(name, sizeof(name), "else_%d", start_pc);
                disasm_add_label(ctx, else_addr, name, LABEL_BLOCK);
                pc = pc2;
                break;
            }
            case M_JZ: {
                /* <cond>,JZ,<target> - jump if zero */
                int target = 0;
                int pc2 = pc;
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &target);
                char name[32];
                snprintf(name, sizeof(name), "L%d", target);
                disasm_add_label(ctx, target, name, LABEL_JUMP_IN);
                pc = pc2;
                break;
            }
            case M_B: {
                /* Block start, add label */
                char name[32];
                snprintf(name, sizeof(name), "L%d", start_pc);
                disasm_add_label(ctx, start_pc, name, LABEL_BLOCK);
                break;
            }
            case M_LIT:
            case M_V:
            case M_LET:
            case M_SET:
            case M_LT:
            case M_GT:
            case M_LE:
            case M_GE:
            case M_EQ:
            case M_ADD:
            case M_SUB:
            case M_MUL:
            case M_DIV:
            case M_MOD:
            case M_AND:
            case M_OR:
            case M_XOR:
            case M_SHL:
            case M_SHR:
            case M_GTWAY:
            case M_WAIT:
            case M_IOW:
            case M_IOR:
            case M_ALLOC:
            case M_TRACE:
            case M_PH:
            case M_HALT:
            case M_DWHL:
            case M_GC:
            case M_BP: {
                /* These instructions have operands */
                uint32_t val = 0;
                int pc2 = pc;
                m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &val);
                pc = pc2;
                break;
            }
            case M_NEWARR:
            case M_LEN:
            case M_IDX:
            case M_STO:
            case M_GET:
            case M_PUT:
            case M_SWP:
            case M_DUP:
            case M_DRP:
            case M_ROT:
            case M_RT:
            case M_E:
            case M_FREE:
            case M_DO:
                /* These instructions have no operands */
                break;
            default:
                break;
        }
    }
}

/* =============================================
 * Output Helper Functions
 * ============================================= */

static void disasm_puts(DisasmContext* ctx, const char* s) {
    int left = MAX_DISASM_LEN - ctx->out_pos - 1;
    if (left <= 0) return;
    
    int len = strlen(s);
    if (len > left) len = left;
    
    memcpy(ctx->output + ctx->out_pos, s, len);
    ctx->out_pos += len;
    ctx->output[ctx->out_pos] = '\0';
}

static void disasm_printf(DisasmContext* ctx, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    disasm_puts(ctx, buf);
}

static void disasm_indent(DisasmContext* ctx) {
    for (int i = 0; i < ctx->indent && i < MAX_INDENT; i++) {
        disasm_puts(ctx, "    ");
    }
}

/* =============================================
 * Bytes to Hex String
 * ============================================= */

static void disasm_bytes_to_hex(DisasmContext* ctx, int start, int end) {
    char hex[128];
    int pos = 0;
    hex[pos++] = '[';
    
    for (int i = start; i < end && i < ctx->len; i++) {
        if (i > start && (i - start) % 16 == 0) {
            hex[pos++] = ' ';
        }
        snprintf(hex + pos, sizeof(hex) - pos, "%02X ", (unsigned)ctx->code[i]);
        pos += 3;
    }
    
    if (pos > 1 && hex[pos - 1] == ' ') hex[pos - 1] = ']';
    else hex[pos++] = ']';
    
    hex[pos] = '\0';
    disasm_puts(ctx, hex);
}

/* =============================================
 * Disassemble Single Instruction
 * ============================================= */

static void disasm_one_instruction(DisasmContext* ctx, int pc, int* next_pc) {
    int start_pc = pc;
    uint32_t op = 0;
    
    /* Parse opcode (varint) */
    if (!m_vm_decode_uvarint(ctx->code, &pc, ctx->len, &op)) {
        disasm_indent(ctx);
        disasm_printf(ctx, "<bad opcode at %d>\n", start_pc);
        *next_pc = pc + 1;
        return;
    }
    *next_pc = pc;
    
    const char* opname = m_vm_opcode_name(op);
    
    /* Check for label */
    Label* lbl = disasm_find_label(ctx, start_pc);
    if (lbl) {
        switch (lbl->type) {
            case LABEL_FUNC:
                disasm_printf(ctx, "\n; === Function: %s ===\n", lbl->name);
                disasm_printf(ctx, "%s:\n", lbl->name);
                break;
            case LABEL_BLOCK:
                disasm_printf(ctx, "\n%s:\n", lbl->name);
                break;
            case LABEL_CALL_TARGET:
                disasm_printf(ctx, "\n; Call target: %s\n", lbl->name);
                break;
            case LABEL_JUMP_IN:
                disasm_printf(ctx, "\n%s:\n", lbl->name);
                break;
            default:
                break;
        }
    }
    
    /* Output address and bytes */
    disasm_indent(ctx);
    disasm_printf(ctx, "%4d:  ", start_pc);
    disasm_bytes_to_hex(ctx, start_pc, *next_pc);
    disasm_printf(ctx, "  %-6s", opname);
    
    /* Parse operands */
    switch (op) {
        /* Signed/unsigned args */
        case M_LIT: {
            uint32_t val = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &val)) {
                disasm_printf(ctx, "%u", (unsigned)val);
                *next_pc = after_pc;
            } else {
                disasm_printf(ctx, "<bad>");
            }
            break;
        }
        
        case M_V:
        case M_LET:
        case M_SET: {
            uint32_t idx = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &idx)) {
                disasm_printf(ctx, "%u", (unsigned)idx);
                *next_pc = after_pc;
            }
            break;
        }
        
        /* Comparison ops */
        case M_LT: case M_GT: case M_LE: case M_GE: case M_EQ:
            /* No args */
            break;
        
        /* Arithmetic ops */
        case M_ADD: case M_SUB: case M_MUL: case M_DIV: case M_MOD:
        case M_AND: case M_OR: case M_XOR:
        case M_SHL: case M_SHR:
            /* No args */
            break;

        /* Array ops - stack-based, no immediate args */
        case M_LEN: case M_NEWARR: case M_IDX: case M_STO:
        case M_GET: case M_PUT: case M_SWP:
            /* Args on stack */
            break;
        
        /* Stack ops */
        case M_DUP: case M_DRP: case M_ROT:
            /* No args */
            break;
        
        /* Function call */
        case M_CL: {
            uint32_t func_id = 0;
            uint32_t argc = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &func_id) &&
                m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &argc)) {
                disasm_printf(ctx, "func_%u, %u args", (unsigned)func_id, (unsigned)argc);
                *next_pc = after_pc;
            }
            break;
        }
        
        /* Function return */
        case M_RT:
            /* No args */
            break;
        
        /* Control flow */
        case M_IF: {
            disasm_printf(ctx, "<cond>,B<then>,E,B<else>,E");
            break;
        }
        
        case M_JZ: {
            uint32_t target = 0;
            int pc2 = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &target)) {
                disasm_printf(ctx, "L%u", (unsigned)target);
                *next_pc = pc2;
            } else {
                disasm_printf(ctx, "<target>");
            }
            break;
        }
        
        case M_JMP: {
            uint32_t target = 0;
            int pc2 = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &target)) {
                disasm_printf(ctx, "L%u", (unsigned)target);
                *next_pc = pc2;
            } else {
                disasm_printf(ctx, "<target>");
            }
            break;
        }
        
        case M_DWHL: {
            uint32_t target = 0;
            int pc2 = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &pc2, ctx->len, &target)) {
                disasm_printf(ctx, "L%u, <cond>", (unsigned)target);
                *next_pc = pc2;
            } else {
                disasm_printf(ctx, "<target>, <cond>");
            }
            break;
        }
        
        case M_FN: {
            uint32_t arity = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &arity)) {
                disasm_printf(ctx, "<arity=%u>,B<body>,E", (unsigned)arity);
                *next_pc = after_pc;
            }
            break;
        }
        
        /* Block markers */
        case M_B:
            ctx->indent++;
            disasm_printf(ctx, "; block begin");
            break;
        
        case M_E:
            ctx->indent--;
            disasm_printf(ctx, "; block end");
            break;
        
        case M_DO:
            disasm_printf(ctx, "; do { body } while");
            break;
        
        /* System ops */
        case M_GTWAY: {
            uint32_t key = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &key)) {
                disasm_printf(ctx, "%u", (unsigned)key);
                *next_pc = after_pc;
            }
            break;
        }
        
        case M_WAIT: {
            uint32_t ms = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &ms)) {
                disasm_printf(ctx, "%ums", (unsigned)ms);
                *next_pc = after_pc;
            }
            break;
        }
        
        case M_IOW: {
            uint32_t dev = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &dev)) {
                disasm_printf(ctx, "dev=%u", (unsigned)dev);
                *next_pc = after_pc;
            }
            break;
        }
        
        case M_IOR: {
            uint32_t dev = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &dev)) {
                disasm_printf(ctx, "dev=%u", (unsigned)dev);
                *next_pc = after_pc;
            }
            break;
        }

        case M_ALLOC: {
            uint32_t size = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &size)) {
                disasm_printf(ctx, "%u", (unsigned)size);
                *next_pc = after_pc;
            }
            break;
        }

        case M_FREE:
            break;

        case M_GC:
            disasm_printf(ctx, "; garbage collection");
            break;

        case M_BP: {
            uint32_t id = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &id)) {
                disasm_printf(ctx, "%u", (unsigned)id);
                *next_pc = after_pc;
            }
            break;
        }
        
        case M_STEP:
            disasm_printf(ctx, "; enable single-step");
            break;

        case M_TRACE: {
            uint32_t level = 0;
            int after_pc = *next_pc;
            if (m_vm_decode_uvarint(ctx->code, &after_pc, ctx->len, &level)) {
                disasm_printf(ctx, "level=%u", (unsigned)level);
                *next_pc = after_pc;
            }
            break;
        }
        
        case M_HALT:
        case M_PH:
            break;
        
        default:
            disasm_printf(ctx, "<unknown 0x%02X>", (unsigned)op);
            break;
    }
    
    disasm_puts(ctx, "\n");
}

/* =============================================
 * Main Disassembly Function
 * ============================================= */

/**
 * Disassemble M-Token bytecode to readable format
 */
const char* m_disasm(const uint8_t* code, int len) {
    static char result[MAX_DISASM_LEN];
    
    DisasmContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.code = code;
    ctx.len = len;
    ctx.indent = 0;
    
    /* Pass 1: scan for labels */
    disasm_scan_labels(&ctx);
    
    /* Header */
    disasm_printf(&ctx, "; ============================================\n");
    disasm_printf(&ctx, ";      M-Token Bytecode Disassembly\n");
    disasm_printf(&ctx, "; ============================================\n");
    disasm_printf(&ctx, "; Length: %d bytes\n", len);
    disasm_printf(&ctx, "; Tokens: All varint encoded\n");
    disasm_printf(&ctx, "; ============================================\n\n");
    
    /* Disassemble each instruction */
    int pc = 0;
    while (pc < len) {
        int next_pc = pc;
        disasm_one_instruction(&ctx, pc, &next_pc);
        pc = next_pc;
    }
    
    disasm_printf(&ctx, "\n; ============================================\n");
    disasm_printf(&ctx, ";           End of Disassembly\n");
    disasm_printf(&ctx, "; ============================================\n");
    
    return ctx.output;
}

/**
 * Print stack snapshot
 */
void m_disasm_print_stack(M_Value* stack, int sp) {
    printf("Stack (sp=%d): [", sp);
    for (int i = 0; i <= sp && i < 16; i++) {
        if (i > 0) printf(", ");
        switch (stack[i].type) {
            case M_TYPE_INT:   printf("%d", (int)stack[i].u.i); break;
            case M_TYPE_FLOAT: printf("%.2f", (double)stack[i].u.f); break;
            case M_TYPE_BOOL:  printf("%s", stack[i].u.b ? "true" : "false"); break;
            case M_TYPE_ARRAY: printf("arr[%d]", stack[i].u.array_ptr ? (int)stack[i].u.array_ptr->len : 0); break;
            default:           printf("?"); break;
        }
    }
    if (sp >= 16) printf(", ...");
    printf("]\n");
}

/**
 * Print execution trace summary
 */
void m_disasm_print_trace(M_SimResult* result) {
    printf("\n");
    printf("+================================================+\n");
    printf("|           Execution Trace Summary              |\n");
    printf("+================================================+\n");
    printf("| Completed:  %-30s   |\n", result->completed ? "YES" : "NO");
    printf("| Halted:     %-30s   |\n", result->halted ? "YES" : "NO");
    printf("| Steps:      %-30llu |\n", (unsigned long long)result->steps);
    printf("| Fault:      %-30s   |\n", m_vm_fault_string(result->fault));
    if (result->sp >= 0) {
        printf("| Result:     %-30d   |\n", (int)result->result);
    }
    printf("+================================================+\n");
    
    printf("\n=== First 15 Trace Entries ===\n");
    int count = result->trace_len;
    if (count > 15) count = 15;
    
    printf("%-6s  %-4s  %-6s  %-4s  %-8s\n", "Step", "PC", "Op", "SP", "Top");
    printf("---------------------------------------------\n");
    for (int i = 0; i < count; i++) {
        M_TraceEntry* e = &result->trace[i];
        printf("%-6llu  %-4d  %-6s  %-4d  %-8d\n",
               (unsigned long long)e->step,
               e->pc,
               m_vm_opcode_name(e->op),
               e->sp,
               (int)e->stack_top);
    }
    
    if (result->trace_len > 15) {
        printf("... and %d more entries\n", result->trace_len - 15);
    }
}

/**
 * Full disassembly report
 */
void m_disasm_full_report(const uint8_t* code, int len, M_SimResult* result) {
    /* Disassemble bytecode */
    const char* disasm = m_disasm(code, len);
    printf("\n%s", disasm);
    
    /* Print trace if available */
    if (result) {
        m_disasm_print_trace(result);
    }
}
