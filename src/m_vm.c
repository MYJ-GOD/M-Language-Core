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

bool m_vm_decode_uvarint64(const uint8_t* code, int* pc, int len, uint64_t* out) {
    if (!code || !pc || !out) return false;
    
    int p = *pc;
    uint64_t res = 0;
    int shift = 0;
    bool terminated = false;

    while (p < len) {
        uint8_t b = code[p++];
        res |= (uint64_t)(b & 0x7F) << shift;

        if ((b & 0x80) == 0) { 
            terminated = true; 
            break; 
        }

        shift += 7;
        if (shift >= 64) {
            return false;
        }
    }

    if (!terminated) return false;
    
    *pc = p;
    *out = res;
    return true;
}

/* Decode a signed varint offset (for jump instructions).
 * Encoding rules: ZigZag + unsigned varint (per spec).
 */
bool m_vm_decode_svarint(const uint8_t* code, int* pc, int len, int32_t* out) {
    if (!code || !pc || !out) return false;
    uint32_t u = 0;
    if (!m_vm_decode_uvarint(code, pc, len, &u)) return false;
    *out = m_vm_decode_zigzag(u);
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

int m_vm_encode_uvarint64(uint64_t n, uint8_t* out) {
    int i = 0;
    while (n > 0x7FULL) {
        out[i++] = (uint8_t)(n & 0x7FULL) | 0x80;
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

/* 64-bit ZigZag helpers (for i64 core type) */
int64_t m_vm_decode_zigzag64(uint64_t n) {
    return (int64_t)((n >> 1) ^ (uint64_t)-(int64_t)(n & 1));
}

uint64_t m_vm_encode_zigzag64(int64_t n) {
    return (uint64_t)((n << 1) ^ (n >> 63));
}

/* =============================================
 * Opcode Token Map (opcode index <-> byte offset)
 * ============================================= */

static bool skip_operands(const uint8_t* code, int len, uint32_t op, int* pc) {
    if (!pc) return false;
    switch (op) {
        case M_LIT: {
            uint64_t val = 0;
            return m_vm_decode_uvarint64(code, pc, len, &val);
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
            uint32_t val = 0;
            return m_vm_decode_uvarint(code, pc, len, &val);
        }
        case M_CL: {
            uint32_t func_id = 0;
            uint32_t argc = 0;
            return m_vm_decode_uvarint(code, pc, len, &func_id) &&
                   m_vm_decode_uvarint(code, pc, len, &argc);
        }
        case M_FN: {
            uint32_t arity = 0;
            return m_vm_decode_uvarint(code, pc, len, &arity);
        }
        case M_JZ:
        case M_JNZ:
        case M_JMP:
        case M_DWHL:
        case M_WHIL: {
            int32_t off = 0;
            return m_vm_decode_svarint(code, pc, len, &off);
        }
        default:
            return true;
    }
}

static bool build_token_map(M_VM* v) {
    if (!v || !v->code || v->code_len <= 0) return false;

    int pc = 0;
    int count = 0;
    while (pc < v->code_len) {
        uint32_t op = 0;
        if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &op)) return false;
        count++;
        if (!skip_operands(v->code, v->code_len, op, &pc)) return false;
    }

    v->token_offsets = (int*)malloc((size_t)count * sizeof(int));
    v->byte_to_token = (int*)malloc((size_t)v->code_len * sizeof(int));
    if (!v->token_offsets || !v->byte_to_token) {
        free(v->token_offsets);
        free(v->byte_to_token);
        v->token_offsets = NULL;
        v->byte_to_token = NULL;
        return false;
    }

    for (int i = 0; i < v->code_len; i++) {
        v->byte_to_token[i] = -1;
    }

    pc = 0;
    int idx = 0;
    while (pc < v->code_len) {
        v->token_offsets[idx] = pc;
        v->byte_to_token[pc] = idx;

        uint32_t op = 0;
        if (!m_vm_decode_uvarint(v->code, &pc, v->code_len, &op)) {
            free(v->token_offsets);
            free(v->byte_to_token);
            v->token_offsets = NULL;
            v->byte_to_token = NULL;
            return false;
        }
        if (!skip_operands(v->code, v->code_len, op, &pc)) {
            free(v->token_offsets);
            free(v->byte_to_token);
            v->token_offsets = NULL;
            v->byte_to_token = NULL;
            return false;
        }
        idx++;
    }

    v->token_count = count;
    return true;
}

/* =============================================
 * Structured Loop Lowering (WH/FR -> JZ/JMP)
 * ============================================= */

typedef struct {
    uint32_t op;
    int start;
    int end;
    uint64_t imm_u64;
    uint32_t imm_u32;
    uint32_t imm_u32_b;
    int32_t imm_s32;
    uint8_t imm_mask; /* bit0:u32, bit1:u32_b, bit2:u64, bit3:s32 */
} Tok;

typedef struct {
    int start_idx;
    int end_idx;
} Range;

typedef enum {
    LOOP_WH = 1,
    LOOP_FR = 2
} LoopType;

typedef struct {
    LoopType type;
    int loop_idx;
    int cond_start_idx;
    int cond_end_idx;
    int body_start_idx; /* first token inside body (after B) */
    int body_end_idx;   /* token index of matching E */
    int inc_start_idx;  /* FR only: first inc token */
    int inc_end_idx;    /* FR only: last inc token */
} LoopInfo;

typedef enum {
    OT_NONE = 0,
    OT_U32,
    OT_U32_U32,
    OT_U64,
    OT_JUMP_ORIG,   /* jump with original target index */
    OT_JUMP_OUT     /* jump with resolved target index */
} OutOpType;

typedef struct {
    uint32_t op;
    OutOpType type;
    uint32_t u32;
    uint32_t u32_b;
    uint64_t u64;
    int target_orig;
    int target_out;
} OutTok;

static bool read_token(const uint8_t* code, int len, int* pc, Tok* out) {
    if (!code || !pc || !out) return false;
    int p = *pc;
    uint32_t op = 0;
    if (!m_vm_decode_uvarint(code, &p, len, &op)) return false;

    out->op = op;
    out->start = *pc;
    out->imm_mask = 0;

    switch (op) {
        case M_LIT: {
            uint64_t val = 0;
            if (!m_vm_decode_uvarint64(code, &p, len, &val)) return false;
            out->imm_u64 = val;
            out->imm_mask |= 0x4;
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
            uint32_t val = 0;
            if (!m_vm_decode_uvarint(code, &p, len, &val)) return false;
            out->imm_u32 = val;
            out->imm_mask |= 0x1;
            break;
        }
        case M_CL: {
            uint32_t func_id = 0;
            uint32_t argc = 0;
            if (!m_vm_decode_uvarint(code, &p, len, &func_id)) return false;
            if (!m_vm_decode_uvarint(code, &p, len, &argc)) return false;
            out->imm_u32 = func_id;
            out->imm_u32_b = argc;
            out->imm_mask |= 0x3;
            break;
        }
        case M_JZ:
        case M_JNZ:
        case M_JMP:
        case M_DWHL:
        case M_WHIL: {
            int32_t off = 0;
            if (!m_vm_decode_svarint(code, &p, len, &off)) return false;
            out->imm_s32 = off;
            out->imm_mask |= 0x8;
            break;
        }
        default:
            break;
    }

    out->end = p;
    *pc = p;
    return true;
}

static int tok_stack_pop(Range* stack, int* sp) {
    if (*sp < 0) return -1;
    (*sp)--;
    return 0;
}

static Range tok_stack_peek(Range* stack, int sp) {
    Range r; r.start_idx = -1; r.end_idx = -1;
    if (sp < 0) return r;
    return stack[sp];
}

static bool tok_stack_push(Range* stack, int* sp, int start_idx, int end_idx) {
    if (*sp + 1 >= STACK_SIZE) return false;
    (*sp)++;
    stack[*sp].start_idx = start_idx;
    stack[*sp].end_idx = end_idx;
    return true;
}

static bool tok_stack_dup(Range* stack, int* sp) {
    if (*sp < 0) return false;
    if (*sp + 1 >= STACK_SIZE) return false;
    stack[*sp + 1] = stack[*sp];
    (*sp)++;
    return true;
}

static bool tok_stack_swp(Range* stack, int sp) {
    if (sp < 1) return false;
    Range a = stack[sp];
    Range b = stack[sp - 1];
    stack[sp] = b;
    stack[sp - 1] = a;
    return true;
}

static bool tok_stack_rot(Range* stack, int sp) {
    if (sp < 2) return false;
    Range a = stack[sp - 2];
    Range b = stack[sp - 1];
    Range c = stack[sp];
    stack[sp - 2] = b;
    stack[sp - 1] = c;
    stack[sp] = a;
    return true;
}

static bool add_outtok(OutTok** out, int* count, int* cap, OutTok t) {
    if (*count + 1 > *cap) {
        int new_cap = (*cap == 0) ? 256 : (*cap * 2);
        OutTok* n = (OutTok*)realloc(*out, (size_t)new_cap * sizeof(OutTok));
        if (!n) return false;
        *out = n;
        *cap = new_cap;
    }
    (*out)[(*count)++] = t;
    return true;
}

static bool lower_structured(M_VM* v) {
    if (!v || !v->code || v->code_len <= 0) return false;

    /* Tokenize */
    Tok* toks = NULL;
    int tok_cap = 0;
    int tok_count = 0;
    int pc = 0;
    while (pc < v->code_len) {
        Tok t;
        if (!read_token(v->code, v->code_len, &pc, &t)) {
            free(toks);
            return false;
        }
        if (tok_count + 1 > tok_cap) {
            int new_cap = (tok_cap == 0) ? 256 : tok_cap * 2;
            Tok* n = (Tok*)realloc(toks, (size_t)new_cap * sizeof(Tok));
            if (!n) { free(toks); return false; }
            toks = n;
            tok_cap = new_cap;
        }
        toks[tok_count++] = t;
    }

    /* Pass 1: identify WH/FR loops using stack-origin ranges */
    LoopInfo* loops = NULL;
    int loop_cap = 0;
    int loop_count = 0;
    int* loop_at = (int*)malloc((size_t)tok_count * sizeof(int));
    if (!loop_at) { free(toks); return false; }
    for (int i = 0; i < tok_count; i++) loop_at[i] = -1;

    Range stack[STACK_SIZE];
    int sp = -1;

    for (int i = 0; i < tok_count; i++) {
        uint32_t op = toks[i].op;

        /* Stack effect simulation (linear) */
        switch (op) {
            case M_LIT:
            case M_V:
            case M_IOR:
                if (!tok_stack_push(stack, &sp, i, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_LEN:
            case M_NEG:
            case M_NOT:
                if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                if (!tok_stack_push(stack, &sp, i, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_DUP:
                if (!tok_stack_dup(stack, &sp)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_DRP:
                if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_SWP:
                if (!tok_stack_swp(stack, sp)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_ROT:
                if (!tok_stack_rot(stack, sp)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_GET:
            case M_IDX: {
                Range b = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                Range a = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                int s = (a.start_idx < b.start_idx) ? a.start_idx : b.start_idx;
                if (!tok_stack_push(stack, &sp, s, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            }
            case M_PUT:
            case M_STO: {
                Range c = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                Range b = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                Range a = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                int s = a.start_idx;
                if (b.start_idx < s) s = b.start_idx;
                if (c.start_idx < s) s = c.start_idx;
                if (!tok_stack_push(stack, &sp, s, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            }
            case M_NEWARR:
            case M_ALLOC: {
                Range a = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                if (!tok_stack_push(stack, &sp, a.start_idx, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            }
            case M_FREE:
            case M_LET:
            case M_SET:
            case M_IOW:
                if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_ADD: case M_SUB: case M_MUL: case M_DIV:
            case M_AND: case M_OR: case M_XOR: case M_SHL: case M_SHR:
            case M_LT:  case M_GT:  case M_LE:  case M_GE:  case M_EQ:  case M_NEQ:
            case M_MOD: {
                Range b = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                Range a = tok_stack_peek(stack, sp); if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                int s = (a.start_idx < b.start_idx) ? a.start_idx : b.start_idx;
                if (!tok_stack_push(stack, &sp, s, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            }
            case M_CL: {
                uint32_t argc = toks[i].imm_u32_b;
                Range s = { .start_idx = i, .end_idx = i };
                for (uint32_t k = 0; k < argc; k++) {
                    Range a = tok_stack_peek(stack, sp);
                    if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                    if (a.start_idx < s.start_idx) s.start_idx = a.start_idx;
                }
                if (!tok_stack_push(stack, &sp, s.start_idx, i)) { free(toks); free(loop_at); free(loops); return false; }
                break;
            }
            case M_RT:
                if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                break;
            case M_IF:
            case M_WH:
            case M_FR:
            case M_JZ:
            case M_JNZ:
                if (tok_stack_pop(stack, &sp) != 0) { free(toks); free(loop_at); free(loops); return false; }
                break;
            default:
                break;
        }

        /* Capture WH/FR loop info */
        if (op == M_WH || op == M_FR) {
            Range cond = tok_stack_peek(stack, sp + 1); /* cond was popped above for WH/FR */
            int cond_start = cond.start_idx;
            int cond_end = cond.end_idx;
            if (cond_start < 0 || cond_end < 0) { free(toks); free(loop_at); free(loops); return false; }
            if (i + 1 >= tok_count || toks[i + 1].op != M_B) { free(toks); free(loop_at); free(loops); return false; }

            int depth = 0;
            int j = i + 1;
            for (; j < tok_count; j++) {
                if (toks[j].op == M_B) depth++;
                else if (toks[j].op == M_E) depth--;
                if (depth == 0) break;
            }
            if (j >= tok_count || toks[j].op != M_E) { free(toks); free(loop_at); free(loops); return false; }

            LoopInfo info;
            memset(&info, 0, sizeof(info));
            info.type = (op == M_WH) ? LOOP_WH : LOOP_FR;
            info.loop_idx = i;
            info.cond_start_idx = cond_start;
            info.cond_end_idx = cond_end;
            info.body_start_idx = i + 2;
            info.body_end_idx = j;
            info.inc_start_idx = -1;
            info.inc_end_idx = -1;

            if (op == M_FR) {
                int inc_start = cond_end + 1;
                int inc_end = i - 1;
                if (inc_start <= inc_end) {
                    info.inc_start_idx = inc_start;
                    info.inc_end_idx = inc_end;
                }
            }

            if (loop_count + 1 > loop_cap) {
                int new_cap = (loop_cap == 0) ? 16 : loop_cap * 2;
                LoopInfo* n = (LoopInfo*)realloc(loops, (size_t)new_cap * sizeof(LoopInfo));
                if (!n) { free(toks); free(loop_at); free(loops); return false; }
                loops = n;
                loop_cap = new_cap;
            }
            loops[loop_count] = info;
            loop_at[i] = loop_count;
            loop_count++;
        }
    }

    if (loop_count == 0) {
        free(toks);
        free(loop_at);
        free(loops);
        return true; /* nothing to lower */
    }

    /* Mark tokens to skip (FR inc ranges) */
    bool* skip = (bool*)calloc((size_t)tok_count, sizeof(bool));
    if (!skip) { free(toks); free(loop_at); free(loops); return false; }
    for (int i = 0; i < loop_count; i++) {
        LoopInfo* li = &loops[i];
        if (li->type == LOOP_FR && li->inc_start_idx >= 0 && li->inc_end_idx >= li->inc_start_idx) {
            for (int k = li->inc_start_idx; k <= li->inc_end_idx; k++) {
                skip[k] = true;
            }
        }
    }

    /* Pass 2: build lowered token list */
    OutTok* out = NULL;
    int out_cap = 0;
    int out_count = 0;
    int* orig_to_out = (int*)malloc((size_t)tok_count * sizeof(int));
    if (!orig_to_out) { free(toks); free(loop_at); free(loops); free(skip); return false; }
    for (int i = 0; i < tok_count; i++) orig_to_out[i] = -1;

    for (int i = 0; i < tok_count; i++) {
        int li_idx = loop_at[i];
        if (li_idx >= 0) {
            LoopInfo* li = &loops[li_idx];
            int cond_out = orig_to_out[li->cond_start_idx];
            if (cond_out < 0) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }

            /* Emit JZ placeholder */
            OutTok jz; memset(&jz, 0, sizeof(jz));
            jz.op = M_JZ;
            jz.type = OT_JUMP_OUT;
            int jz_index = out_count;
            if (!add_outtok(&out, &out_count, &out_cap, jz)) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }

            /* Emit body tokens (exclude outer B/E) */
            for (int k = li->body_start_idx; k < li->body_end_idx; k++) {
                OutTok ot; memset(&ot, 0, sizeof(ot));
                ot.op = toks[k].op;
                if (toks[k].imm_mask & 0x4) { ot.type = OT_U64; ot.u64 = toks[k].imm_u64; }
                else if ((toks[k].imm_mask & 0x3) == 0x3) { ot.type = OT_U32_U32; ot.u32 = toks[k].imm_u32; ot.u32_b = toks[k].imm_u32_b; }
                else if (toks[k].imm_mask & 0x1) { ot.type = OT_U32; ot.u32 = toks[k].imm_u32; }
                else if (toks[k].imm_mask & 0x8) {
                    ot.type = OT_JUMP_ORIG;
                    int base = k + 1;
                    int target = base + toks[k].imm_s32;
                    ot.target_orig = target;
                } else {
                    ot.type = OT_NONE;
                }
                if (!add_outtok(&out, &out_count, &out_cap, ot)) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }
                orig_to_out[k] = out_count - 1;
            }

            /* Emit FR inc tokens after body */
            if (li->type == LOOP_FR && li->inc_start_idx >= 0 && li->inc_end_idx >= li->inc_start_idx) {
                for (int k = li->inc_start_idx; k <= li->inc_end_idx; k++) {
                    OutTok ot; memset(&ot, 0, sizeof(ot));
                    ot.op = toks[k].op;
                    if (toks[k].imm_mask & 0x4) { ot.type = OT_U64; ot.u64 = toks[k].imm_u64; }
                    else if ((toks[k].imm_mask & 0x3) == 0x3) { ot.type = OT_U32_U32; ot.u32 = toks[k].imm_u32; ot.u32_b = toks[k].imm_u32_b; }
                    else if (toks[k].imm_mask & 0x1) { ot.type = OT_U32; ot.u32 = toks[k].imm_u32; }
                    else if (toks[k].imm_mask & 0x8) {
                        ot.type = OT_JUMP_ORIG;
                        int base = k + 1;
                        int target = base + toks[k].imm_s32;
                        ot.target_orig = target;
                    } else {
                        ot.type = OT_NONE;
                    }
                    if (!add_outtok(&out, &out_count, &out_cap, ot)) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }
                    orig_to_out[k] = out_count - 1;
                }
            }

            /* Emit JMP back to cond_start */
            OutTok jmp; memset(&jmp, 0, sizeof(jmp));
            jmp.op = M_JMP;
            jmp.type = OT_JUMP_OUT;
            jmp.target_out = cond_out;
            if (!add_outtok(&out, &out_count, &out_cap, jmp)) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }

            /* Patch JZ target to next token index */
            out[jz_index].target_out = out_count;

            i = li->body_end_idx; /* skip to matching E */
            continue;
        }

        if (skip[i]) continue;

        OutTok ot; memset(&ot, 0, sizeof(ot));
        ot.op = toks[i].op;
        if (toks[i].imm_mask & 0x4) { ot.type = OT_U64; ot.u64 = toks[i].imm_u64; }
        else if ((toks[i].imm_mask & 0x3) == 0x3) { ot.type = OT_U32_U32; ot.u32 = toks[i].imm_u32; ot.u32_b = toks[i].imm_u32_b; }
        else if (toks[i].imm_mask & 0x1) { ot.type = OT_U32; ot.u32 = toks[i].imm_u32; }
        else if (toks[i].imm_mask & 0x8) {
            ot.type = OT_JUMP_ORIG;
            int base = i + 1;
            int target = base + toks[i].imm_s32;
            ot.target_orig = target;
        } else {
            ot.type = OT_NONE;
        }
        if (!add_outtok(&out, &out_count, &out_cap, ot)) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }
        orig_to_out[i] = out_count - 1;
    }

    /* Encode output */
    uint8_t* new_code = (uint8_t*)malloc((size_t)v->code_len * 4 + 64);
    if (!new_code) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); return false; }
    int new_len = 0;

    for (int i = 0; i < out_count; i++) {
        OutTok* ot = &out[i];
        new_len += m_vm_encode_uvarint(ot->op, &new_code[new_len]);

        switch (ot->type) {
            case OT_U32:
                new_len += m_vm_encode_uvarint(ot->u32, &new_code[new_len]);
                break;
            case OT_U32_U32:
                new_len += m_vm_encode_uvarint(ot->u32, &new_code[new_len]);
                new_len += m_vm_encode_uvarint(ot->u32_b, &new_code[new_len]);
                break;
            case OT_U64:
                new_len += m_vm_encode_uvarint64(ot->u64, &new_code[new_len]);
                break;
            case OT_JUMP_ORIG: {
                int target_out = (ot->target_orig >= 0 && ot->target_orig < tok_count) ? orig_to_out[ot->target_orig] : -1;
                if (target_out < 0) { free(toks); free(loop_at); free(loops); free(skip); free(orig_to_out); free(out); free(new_code); return false; }
                int offset = target_out - (i + 1);
                new_len += m_vm_encode_uvarint(m_vm_encode_zigzag(offset), &new_code[new_len]);
                break;
            }
            case OT_JUMP_OUT: {
                int offset = ot->target_out - (i + 1);
                new_len += m_vm_encode_uvarint(m_vm_encode_zigzag(offset), &new_code[new_len]);
                break;
            }
            case OT_NONE:
            default:
                break;
        }
    }

    /* Swap in lowered code */
    if (v->code_owned) {
        free(v->code_owned);
        v->code_owned = NULL;
    }
    v->code_owned = new_code;
    v->code = new_code;
    v->code_len = new_len;

    free(toks);
    free(loop_at);
    free(loops);
    free(skip);
    free(orig_to_out);
    free(out);
    return true;
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
    if ((vm)->sp + (k) >= (vm)->stack_limit) { SET_FAULT((vm), M_FAULT_STACK_OVERFLOW); return; } \
    if ((vm)->sp + (k) >= STACK_SIZE) { SET_FAULT((vm), M_FAULT_STACK_OVERFLOW); return; } \
} while(0)

#define CHECK_LOCALS(vm, idx) do { \
    if ((idx) < 0 || (idx) >= LOCALS_SIZE) { SET_FAULT((vm), M_FAULT_LOCALS_OOB); return; } \
} while(0)

#define CHECK_GLOBALS(vm, idx) do { \
    if ((idx) < 0 || (idx) >= GLOBALS_SIZE) { SET_FAULT((vm), M_FAULT_GLOBALS_OOB); return; } \
} while(0)

#define CHECK_RET_PUSH(vm) do { \
    if ((size_t)(vm)->rp + 1 >= RET_STACK_SIZE) { SET_FAULT((vm), M_FAULT_RET_STACK_OVERFLOW); return; } \
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
    int _addr = (int)(addr); \
    if (_addr < 0 || _addr >= (vm)->code_len) { SET_FAULT((vm), M_FAULT_PC_OOB); return; } \
} while(0)

#define TOP(vm) ((vm)->stack[(vm)->sp])
#define POP(vm) ((vm)->stack[(vm)->sp--])
#define PUSH(vm, val) do { (vm)->stack[++(vm)->sp] = (val); } while(0)

/* Capability helpers (device_id 0..255) */
static void caps_clear(M_VM* v) {
    memset(v->caps, 0, sizeof(v->caps));
}

static bool caps_has(M_VM* v, uint32_t id) {
    if (id > 255) return false;
    return (v->caps[id >> 3] & (uint8_t)(1u << (id & 7))) != 0;
}

static void caps_add(M_VM* v, uint32_t id) {
    if (id > 255) return;
    v->caps[id >> 3] |= (uint8_t)(1u << (id & 7));
}

/* =============================================
 * Value Operations
 * ============================================= */

static M_Value make_int(int64_t i) {
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

static int64_t to_int(M_Value v) {
    switch (v.type) {
        case M_TYPE_INT: return v.u.i;
        case M_TYPE_FLOAT: return (int64_t)v.u.f;
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
    /* Use memcpy to ensure correct struct copy */
    memcpy(&v->stack[v->sp + 1], &v->stack[v->sp], sizeof(M_Value));
    v->sp++;
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

/* --- Memory Management --- */

static void h_alloc(M_VM* v) {
    /* ALLOC: <size> -> <ptr>
     * Allocate memory on the heap and push a pointer to it.
     * The pointer is stored as a reference type.
     * Bytecode format: ALLOC, <size>
     */
    NEED(v, 1);
    int64_t size = to_int(POP(v));

    if (size <= 0) { SET_FAULT(v, M_FAULT_BAD_ARG); return; }
    if (size > 1000000) { SET_FAULT(v, M_FAULT_BAD_ARG); return; }  /* Sanity limit */

    void* ptr = malloc((size_t)size);
    if (!ptr) { SET_FAULT(v, M_FAULT_OOM); return; }

    /* Track allocation for cleanup */
    AllocNode* node = (AllocNode*)malloc(sizeof(AllocNode));
    if (!node) {
        free(ptr);
        SET_FAULT(v, M_FAULT_OOM);
        return;
    }
    node->ptr = ptr;
    node->next = v->alloc_head;
    v->alloc_head = node;

    /* Push pointer as a reference */
    M_Value ref;
    ref.type = M_TYPE_REF;
    ref.u.ref = ptr;
    PUSH(v, ref);
}

static void h_free(M_VM* v) {
    /* FREE: <ptr> -> (pop)
     * Free previously allocated memory.
     */
    NEED(v, 1);
    M_Value ref = POP(v);

    if (ref.type != M_TYPE_REF || ref.u.ref == NULL) {
        SET_FAULT(v, M_FAULT_TYPE_MISMATCH);
        return;
    }

    /* Find and remove from allocation tracking */
    AllocNode** p = &v->alloc_head;
    while (*p) {
        if ((*p)->ptr == ref.u.ref) {
            AllocNode* to_free = *p;
            *p = (*p)->next;
            free(to_free);
            break;
        }
        p = &(*p)->next;
    }

    /* Free the memory */
    free(ref.u.ref);
}

/* =============================================
 * Garbage Collection (Mark-Sweep)
 * ============================================= */

void m_vm_gc_enable(M_VM* v, bool enable) {
    v->gc_enabled = enable;
}

void m_vm_set_gc_threshold(M_VM* v, int threshold) {
    v->gc_threshold = threshold > 0 ? threshold : 100;
}

/* Mark a value as reachable during GC */
static void gc_mark_value(M_VM* v, M_Value val, AllocNode** marked, int* marked_count, int marked_cap) {
    void* ptr = NULL;
    if (val.type == M_TYPE_REF) {
        ptr = val.u.ref;
    } else if (val.type == M_TYPE_ARRAY) {
        ptr = val.u.array_ptr;
    } else {
        return;
    }

    if (ptr == NULL) return;
    if (*marked_count >= marked_cap) return;

    /* Check if already marked */
    for (int i = 0; i < *marked_count; i++) {
        if (marked[i]->ptr == ptr) {
            return;
        }
    }

    /* Find and mark this allocation */
    AllocNode* node = v->alloc_head;
    while (node) {
        if (node->ptr == ptr) {
            marked[*marked_count] = node;
            (*marked_count)++;
            break;
        }
        node = node->next;
    }

    /* If this is an array, traverse its elements */
    if (val.type == M_TYPE_ARRAY && val.u.array_ptr != NULL) {
        M_Array* arr = val.u.array_ptr;
        for (int64_t i = 0; i < arr->len; i++) {
            if (*marked_count >= marked_cap) return;
            gc_mark_value(v, arr->data[i], marked, marked_count, marked_cap);
        }
    }
}

/* Mark all reachable values from stacks and locals */
static void gc_mark_all(M_VM* v, AllocNode** marked, int* marked_count, int marked_cap) {
    /* Mark stack values */
    for (int i = 0; i <= v->sp; i++) {
        gc_mark_value(v, v->stack[i], marked, marked_count, marked_cap);
    }
    
    /* Mark return stack (addresses are not refs, skip) */
    
    /* Mark locals */
    for (int i = 0; i < LOCALS_SIZE; i++) {
        gc_mark_value(v, v->locals[i], marked, marked_count, marked_cap);
    }
    
    /* Mark locals frames */
    for (int f = 0; f <= v->frame_sp; f++) {
        for (int i = 0; i < LOCALS_SIZE; i++) {
            gc_mark_value(v, v->locals_frames[f][i], marked, marked_count, marked_cap);
        }
    }
    
    /* Mark globals */
    for (int i = 0; i < GLOBALS_SIZE; i++) {
        gc_mark_value(v, v->globals[i], marked, marked_count, marked_cap);
    }
}

void m_vm_gc(M_VM* v) {
    /* Count current allocations */
    int alloc_count = 0;
    AllocNode* node = v->alloc_head;
    while (node) {
        alloc_count++;
        node = node->next;
    }
    
    if (alloc_count == 0) {
        return;  /* Nothing to collect */
    }
    
    /* Allocate marked array */
    AllocNode** marked = (AllocNode**)calloc((size_t)alloc_count, sizeof(AllocNode*));
    if (!marked) {
        return;  /* Out of memory, can't GC */
    }
    
    int marked_count = 0;
    
    /* Mark all reachable values */
    gc_mark_all(v, marked, &marked_count, alloc_count);
    
    /* Sweep unreachable allocations */
    AllocNode** p = &v->alloc_head;
    while (*p) {
        /* Check if this node is marked (reachable) */
        bool is_marked = false;
        for (int i = 0; i < marked_count; i++) {
            if (marked[i] == *p) {
                is_marked = true;
                break;
            }
        }
        
        if (!is_marked) {
            /* Unreachable - free it */
            AllocNode* to_free = *p;
            free(to_free->ptr);
            *p = to_free->next;
            free(to_free);
        } else {
            p = &(*p)->next;
        }
    }
    
    free(marked);
    
    /* Reset allocation counter */
    v->alloc_count = 0;
    
    /* Call trace hook if available */
    if (v->trace) {
        v->trace(1, "GC completed");
    }
}

/* Trigger GC check (called after each ALLOC) */
static void gc_check(M_VM* v) {
    if (!v->gc_enabled) {
        return;
    }
    
    v->alloc_count++;
    
    if (v->alloc_count >= v->gc_threshold) {
        m_vm_gc(v);
    }
}

static void h_gc(M_VM* v) {
    /* Manual GC trigger: GC */
    m_vm_gc(v);
}

/* =============================================
 * Debugging Support
 * ============================================= */

/* Breakpoint tracking */
#define MAX_BREAKPOINTS 16
typedef struct {
    int pc;
    int id;
    bool active;
} Breakpoint;

static Breakpoint breakpoints[MAX_BREAKPOINTS];
static int breakpoint_count = 0;

void m_vm_single_step(M_VM* v, bool enable) {
    v->single_step = enable;
}

int m_vm_set_breakpoint(M_VM* v, int pc, int id) {
    if (breakpoint_count >= MAX_BREAKPOINTS) {
        return -1;  /* No room */
    }
    
    /* Check if breakpoint already exists at this PC */
    for (int i = 0; i < breakpoint_count; i++) {
        if (breakpoints[i].pc == pc) {
            breakpoints[i].id = id;
            breakpoints[i].active = true;
            return id;
        }
    }
    
    /* Add new breakpoint */
    breakpoints[breakpoint_count].pc = pc;
    breakpoints[breakpoint_count].id = id;
    breakpoints[breakpoint_count].active = true;
    breakpoint_count++;
    
    return id;
}

int m_vm_clear_breakpoint(M_VM* v, int pc) {
    for (int i = 0; i < breakpoint_count; i++) {
        if (breakpoints[i].pc == pc) {
            breakpoints[i].active = false;
            return breakpoints[i].id;
        }
    }
    return -1;
}

void m_vm_clear_all_breakpoints(M_VM* v) {
    for (int i = 0; i < breakpoint_count; i++) {
        breakpoints[i].active = false;
    }
}

/* Check if there's a breakpoint at current PC */
static int check_breakpoint(M_VM* v) {
    for (int i = 0; i < breakpoint_count; i++) {
        if (breakpoints[i].active && breakpoints[i].pc == v->pc) {
            return breakpoints[i].id;
        }
    }
    return -1;
}

static void h_bp(M_VM* v) {
    /* Breakpoint: BP,<id> - set a breakpoint at this location */
    int pc = v->pc;
    uint32_t id = 0;
    int next_pc = pc;
    
    if (m_vm_decode_uvarint(v->code, &next_pc, v->code_len, &id)) {
        v->pc = next_pc;
        m_vm_set_breakpoint(v, pc, (int)id);
    }
}

static void h_step(M_VM* v) {
    /* Single step: STEP - enable single-step mode for one instruction */
    v->single_step = true;
}

/* =============================================
 * JIT Compilation (Stub implementations)
 * ============================================= */

void m_vm_jit_enable(M_VM* v, bool enable) {
    (void)v; (void)enable;
}

void m_vm_jit_set_threshold(M_VM* v, int threshold) {
    (void)v; (void)threshold;
}

bool m_vm_jit_compile(M_VM* v, int start_pc, int end_pc) {
    (void)v; (void)start_pc; (void)end_pc;
    return false;
}

/* --- Stack Operations --- */

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
    /* Literal: LIT,<zigzag_i64>
     * Uses zigzag decoding to support negative numbers
     */
    int pc = v->pc;
    uint64_t enc = 0;
    if (!m_vm_decode_uvarint64(v->code, &pc, v->code_len, &enc)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    SPACE(v, 1);
    v->pc = pc;
    /* Use zigzag decode to support negative integers */
    PUSH(v, make_int(m_vm_decode_zigzag64(enc)));
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
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a + b));
}

static void h_sub(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a - b));
}

static void h_mul(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a * b));
}

static void h_div(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    if (b == 0) { SET_FAULT(v, M_FAULT_DIV_BY_ZERO); return; }
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a / b));
}

static void h_mod(M_VM* v) {
    /* Modulo: a,b -> a%b (C semantics: sign matches a) */
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    if (b == 0) { SET_FAULT(v, M_FAULT_MOD_BY_ZERO); return; }
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a % b));
}

static void h_and(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a & b));
}

static void h_or(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a | b));
}

static void h_xor(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a ^ b));
}

static void h_shl(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v)) & 63;  /* Apply mask per spec: b & 63 */
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a << b));
}

static void h_shr(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v)) & 63;  /* Apply mask per spec: b & 63 */
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a >> b));
}

static void h_neg(M_VM* v) {
    /* Negate: a -> -a */
    NEED(v, 1);
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(-a));
}

static void h_not(M_VM* v) {
    /* Bitwise NOT: a -> ~a */
    NEED(v, 1);
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(~a));
}

/* --- Comparison --- */

static void h_lt(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a < b ? 1 : 0));
}

static void h_gt(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a > b ? 1 : 0));
}

static void h_le(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a <= b ? 1 : 0));
}

static void h_ge(M_VM* v) {
    NEED(v, 2);
    int64_t b = to_int(POP(v));
    int64_t a = to_int(POP(v));
    PUSH(v, make_int(a >= b ? 1 : 0));
}

static void h_eq(M_VM* v) {
    NEED(v, 2);
    M_Value b = POP(v);
    M_Value a = POP(v);
    int64_t result = 0;
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

static void h_neq(M_VM* v) {
    /* Not equal: a,b -> (a!=b) */
    NEED(v, 2);
    M_Value b = POP(v);
    M_Value a = POP(v);
    int64_t result = 0;
    if (a.type == b.type) {
        switch (a.type) {
            case M_TYPE_INT: result = (a.u.i != b.u.i) ? 1 : 0; break;
            case M_TYPE_FLOAT: result = (a.u.f != b.u.f) ? 1 : 0; break;
            case M_TYPE_BOOL: result = (a.u.b != b.u.b) ? 1 : 0; break;
            default: result = 0;
        }
    }
    PUSH(v, make_int(result));
}

/* --- Array Operations --- */

static void h_len(M_VM* v) {
    NEED(v, 1);
    M_Value arr = POP(v);
    if (arr.type == M_TYPE_ARRAY && arr.u.array_ptr != NULL) {
        /* Push length using memcpy */
        M_Value len_val;
        len_val.type = M_TYPE_INT;
        len_val.u.i = arr.u.array_ptr->len;
        v->sp++;
        memcpy(&v->stack[v->sp], &len_val, sizeof(M_Value));
    } else {
        SET_FAULT(v, M_FAULT_TYPE_MISMATCH);
    }
}

static void h_newarr(M_VM* v) {
    /* NEWARR: <size> -> <array_ref> */
    NEED(v, 1);
    int64_t size = to_int(POP(v));
    if (size < 0) { SET_FAULT(v, M_FAULT_BAD_ARG); return; }
    if (size > 1000000) { SET_FAULT(v, M_FAULT_BAD_ARG); return; }  /* Sanity limit */

    /* Allocate array header + data */
    size_t header_size = sizeof(M_Array);
    size_t data_size = (size_t)size * sizeof(M_Value);
    M_Array* arr = (M_Array*)malloc(header_size + data_size);

    if (!arr) { SET_FAULT(v, M_FAULT_OOM); return; }

    arr->len = size;
    arr->cap = size;
    for (int64_t i = 0; i < size; i++) {
        arr->data[i].type = M_TYPE_INT;
        arr->data[i].u.i = 0;
    }

    /* Track allocation */
    AllocNode* node = (AllocNode*)malloc(sizeof(AllocNode));
    if (!node) {
        free(arr);
        SET_FAULT(v, M_FAULT_OOM);
        return;
    }
    node->ptr = arr;
    node->next = v->alloc_head;
    v->alloc_head = node;

    /* Push array reference using memcpy */
    M_Value arr_val;
    arr_val.type = M_TYPE_ARRAY;
    arr_val.u.array_ptr = arr;
    v->sp++;
    memcpy(&v->stack[v->sp], &arr_val, sizeof(M_Value));
}

static void h_idx(M_VM* v) {
    /* IDX: <array_ref>,<index> -> <element> */
    NEED(v, 2);
    int64_t idx = to_int(POP(v));
    M_Value arr = POP(v);

    if (arr.type != M_TYPE_ARRAY || arr.u.array_ptr == NULL) {
        SET_FAULT(v, M_FAULT_TYPE_MISMATCH);
        return;
    }
    if (idx < 0 || idx >= arr.u.array_ptr->len) {
        SET_FAULT(v, M_FAULT_INDEX_OOB);
        return;
    }

    PUSH(v, arr.u.array_ptr->data[idx]);
}

static void h_sto(M_VM* v) {
    /* STO: <array_ref>,<index>,<value> -> <array_ref> */
    NEED(v, 3);
    M_Value val = POP(v);
    int64_t idx = to_int(POP(v));
    M_Value arr = POP(v);

    if (arr.type != M_TYPE_ARRAY || arr.u.array_ptr == NULL) {
        SET_FAULT(v, M_FAULT_TYPE_MISMATCH);
        return;
    }
    if (idx < 0 || idx >= arr.u.array_ptr->len) {
        SET_FAULT(v, M_FAULT_INDEX_OOB);
        return;
    }

    arr.u.array_ptr->data[idx] = val;
    /* Use memcpy to ensure correct struct copy */
    v->sp++;
    memcpy(&v->stack[v->sp], &arr, sizeof(M_Value));
}

static void h_get(M_VM* v) {
    NEED(v, 2);
    int64_t idx = to_int(POP(v));
    M_Value arr = POP(v);

    if (arr.type != M_TYPE_ARRAY || arr.u.array_ptr == NULL) {
        SET_FAULT(v, M_FAULT_TYPE_MISMATCH);
        return;
    }
    if (idx < 0 || idx >= arr.u.array_ptr->len) {
        SET_FAULT(v, M_FAULT_INDEX_OOB);
        return;
    }
    PUSH(v, arr.u.array_ptr->data[idx]);
}

static void h_put(M_VM* v) {
    NEED(v, 3);
    M_Value val = POP(v);
    int64_t idx = to_int(POP(v));
    M_Value arr = POP(v);

    if (arr.type != M_TYPE_ARRAY || arr.u.array_ptr == NULL) {
        SET_FAULT(v, M_FAULT_TYPE_MISMATCH);
        return;
    }
    if (idx < 0 || idx >= arr.u.array_ptr->len) {
        SET_FAULT(v, M_FAULT_INDEX_OOB);
        return;
    }

    arr.u.array_ptr->data[idx] = val;
    PUSH(v, arr);
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

static void h_wh(M_VM* v) {
    /* While loop: <cond>,WH,B<body>,E
     * Format: cond, WH, B, body..., E
     * Execution:
     * 1. Pop condition from stack
     * 2. If cond is false (0), skip body and jump past E
     * 3. If cond is true, execute body, then jump back to check condition
     */
    NEED(v, 1);
    int cond_pc = v->pc - 1;  /* PC of condition (before WH opcode) */
    M_Value cond = POP(v);
    
    /* Skip WH opcode */
    int pc = v->pc;
    uint32_t op = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &op);
    
    /* Skip B opcode */
    uint32_t tok = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &tok);
    v->pc = pc;
    
    if (!to_bool(cond)) {
        /* Condition false, skip entire loop body */
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
    /* If condition true, fall through to body.
     * After body executes, we need to re-evaluate condition.
     * This requires the body to end with a jump back to condition check.
     * For now, we just fall through - proper WH loops need compiler support
     * to insert the backward jump.
     */
}

static void h_fr(M_VM* v) {
    /* For loop: <init>,<cond>,<inc>,FR,B<body>,E
     * Stack: [init...], cond, inc, FR, B, body..., E
     *
     * Execution:
     * 1. <init> has already been executed before FR
     * 2. Pop cond (condition expression result)
     * 3. If cond is false, skip entire loop body
     * 4. If cond is true, execute body, then inc, then repeat
     *
     * Note: For a complete for loop, the bytecode should look like:
     *   <init>
     *   L_cond:
     *   <cond>
     *   JZ L_end
     *   B
     *   <body>
     *   E
     *   <inc>
     *   JMP L_cond
     *   L_end:
     *
     * The FR instruction is a marker for structured loops.
     * Currently, we just skip past the body for safety.
     * Full runtime support requires the compiler to insert jumps.
     */
    NEED(v, 1);
    int64_t cond = to_int(POP(v));

    /* Skip FR opcode */
    int pc = v->pc;
    uint32_t op = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &op);

    /* Skip B opcode */
    uint32_t tok = 0;
    m_vm_decode_uvarint(v->code, &pc, v->code_len, &tok);
    v->pc = pc;

    if (cond == 0) {
        /* Condition false, skip entire loop body */
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
    /* If condition true, fall through to body.
     * Note: A complete implementation would track the condition PC
     * and insert a backward jump after the body+inc.
     * This requires compiler support for proper bytecode generation.
     */
}

static void h_jz(M_VM* v) {
    /* Jump if zero: <cond>,JZ,<offset>
     * Jump to offset if condition is false (zero).
     * Offset is signed varint encoded (opcode index units, relative to next opcode).
     */
    NEED(v, 1);
    int64_t cond = to_int(POP(v));
    int pc = v->pc;
    int32_t offset = 0;
    
    if (!m_vm_decode_svarint(v->code, &pc, v->code_len, &offset)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    
    if (cond == 0) {
        int base = v->last_op_index + 1;
        int target_index = base + offset;
        if (!v->token_offsets || v->last_op_index < 0 ||
            target_index < 0 || target_index >= v->token_count) {
            SET_FAULT(v, M_FAULT_PC_OOB);
            return;
        }
        v->pc = v->token_offsets[target_index];
    }
}

static void h_jnz(M_VM* v) {
    /* Jump if not zero: <cond>,JNZ,<offset>
     * Jump to offset if condition is true (non-zero).
     * Offset is signed varint encoded (opcode index units, relative to next opcode).
     */
    NEED(v, 1);
    int64_t cond = to_int(POP(v));
    int pc = v->pc;
    int32_t offset = 0;
    
    if (!m_vm_decode_svarint(v->code, &pc, v->code_len, &offset)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    
    if (cond != 0) {
        int base = v->last_op_index + 1;
        int target_index = base + offset;
        if (!v->token_offsets || v->last_op_index < 0 ||
            target_index < 0 || target_index >= v->token_count) {
            SET_FAULT(v, M_FAULT_PC_OOB);
            return;
        }
        v->pc = v->token_offsets[target_index];
    }
}

static void h_do(M_VM* v) {
    /* DO marker - no operands, no side effects */
    (void)v;
}

static void h_dwhl(M_VM* v) {
    /* DO-WHILE loop: jump back to DO if condition is true
     * Format: DO,<body>,DWHL,<offset>
     * When we reach DWHL, we pop the condition and jump back if true.
     * The offset is signed varint encoded (opcode index units, relative to next opcode).
     */
    NEED(v, 1);
    int64_t cond = to_int(POP(v));
    int pc = v->pc;
    int32_t offset = 0;
    
    if (!m_vm_decode_svarint(v->code, &pc, v->code_len, &offset)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    
    if (cond != 0) {
        /* Jump back to DO (opcode index units) */
        int base = v->last_op_index + 1;
        int target_index = base + offset;
        if (!v->token_offsets || v->last_op_index < 0 ||
            target_index < 0 || target_index >= v->token_count) {
            SET_FAULT(v, M_FAULT_PC_OOB);
            return;
        }
        v->pc = v->token_offsets[target_index];
    }
}

static void h_whil(M_VM* v) {
    /* WHILE loop: check condition, jump to body if true
     * Format: <condition>, WHILE,<offset>
     * The condition should be on top of the stack.
     * If cond is false (zero), jump to loop end (opcode index units, relative to next opcode).
     * If cond is true, fall through to body.
     */
    NEED(v, 1);
    int64_t cond = to_int(POP(v));
    int pc = v->pc;
    int32_t offset = 0;
    
    if (!m_vm_decode_svarint(v->code, &pc, v->code_len, &offset)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    
    if (cond == 0) {
        /* Condition false, jump to loop end (opcode index units) */
        int base = v->last_op_index + 1;
        int target_index = base + offset;
        if (!v->token_offsets || v->last_op_index < 0 ||
            target_index < 0 || target_index >= v->token_count) {
            SET_FAULT(v, M_FAULT_PC_OOB);
            return;
        }
        v->pc = v->token_offsets[target_index];
    }
}

static void h_jmp(M_VM* v) {
    /* Unconditional jump: JMP,<offset> */
    int pc = v->pc;
    int32_t offset = 0;
    
    if (!m_vm_decode_svarint(v->code, &pc, v->code_len, &offset)) {
        SET_FAULT(v, M_FAULT_BAD_ENCODING);
        return;
    }
    v->pc = pc;
    {
        int base = v->last_op_index + 1;
        int target_index = base + offset;
        if (!v->token_offsets || v->last_op_index < 0 ||
            target_index < 0 || target_index >= v->token_count) {
            SET_FAULT(v, M_FAULT_PC_OOB);
            return;
        }
        v->pc = v->token_offsets[target_index];
    }
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
    
    v->call_depth--;  /* Decrement call depth */
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

    /* Check call depth limit */
    if (v->call_depth >= v->call_depth_limit) {
        SET_FAULT(v, M_FAULT_CALL_DEPTH_LIMIT);
        return;
    }
    v->call_depth++;

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
    v->pc = pc;
    if (key > 255) {
        SET_FAULT(v, M_FAULT_BAD_ARG);
        return;
    }
    caps_add(v, key);
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
    if (!caps_has(v, dev)) {
        SET_FAULT(v, M_FAULT_UNAUTHORIZED);
        return;
    }
    M_Value val = POP(v);
    if (v->io_write) v->io_write((uint8_t)dev, val);
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
    if (!caps_has(v, dev)) {
        SET_FAULT(v, M_FAULT_UNAUTHORIZED);
        return;
    }
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
        snprintf(msg, sizeof(msg), "Trace level %u: pc=%d op=%s(%u) sp=%d",
                 (unsigned)level,
                 v->last_pc,
                 m_vm_opcode_name(v->last_op),
                 (unsigned)v->last_op,
                 v->sp);
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
    [M_LIT]    = 2,
    [M_V]      = 2,
    [M_LET]    = 2,
    [M_SET]    = 3,
    [M_ADD]    = 1,
    [M_SUB]    = 1,
    [M_MUL]    = 3,
    [M_DIV]    = 5,
    [M_AND]    = 1,     /* Core - per spec (was M_MOD, now AND at 54) */
    [M_OR]     = 1,
    [M_XOR]    = 1,
    [M_SHL]    = 1,
    [M_SHR]    = 1,
    [M_LT]     = 1,
    [M_GT]     = 1,
    [M_LE]     = 1,
    [M_GE]     = 1,
    [M_EQ]     = 1,
    [M_DUP]    = 1,
    [M_DRP]    = 1,
    [M_ROT]    = 1,
    /* Array operations - both规范 defined and legacy */
    [M_LEN]    = 2,
    [M_GET]    = 2,
    [M_PUT]    = 3,
    [M_SWP]    = 1,
    [M_GET_ALIAS] = 2,   /* Legacy alias */
    [M_PUT_ALIAS] = 3,   /* Legacy alias */
    [M_SWP_ALIAS] = 1,   /* Legacy alias */
    [M_ALLOC]  = 5,      /* Memory allocation */
    [M_FREE]   = 2,      /* Memory free */
    [M_NEWARR] = 5,
    [M_IDX]    = 2,
    [M_STO]    = 3,
    [M_B]      = 0,
    [M_E]      = 0,
    [M_IF]     = 1,
    [M_WH]     = 1,      /* While loop */
    [M_FR]     = 1,      /* For loop (placeholder) */
    [M_RT]     = 2,
    [M_CL]     = 5,
    [M_HALT]   = 0,
    [M_GTWAY]  = 1,
    [M_WAIT]   = 1,
    [M_IOW]    = 5,
    [M_IOR]    = 3,
    [M_TRACE]  = 1,
    [M_PH]     = 0,
    [M_GC]     = 10,    /* GC costs more (scans all memory) */
    [M_BP]     = 1,
    [M_STEP]   = 0,
    [M_ALLOC]  = 5,     /* Platform extension */
    [M_FREE]   = 2,
    /* Extension instructions (100-199) */
    [M_JZ]     = 1,
    [M_JNZ]    = 1,     /* Jump if not zero */
    [M_JMP]    = 1,
    /* Extension arithmetic (110+) */
    [M_MOD]    = 5,     /* Modulo - moved to Extension 110 */
    [M_NEG]    = 1,     /* Negate */
    [M_NOT]    = 1,     /* Bitwise NOT */
    [M_NEQ]    = 1,     /* Not equal - moved to Extension 113 */
    /* Extension loop constructs (legacy) */
    [M_DWHL]   = 1,
    [M_DO]     = 0,
    [M_WHIL]   = 1
};

/* =============================================
 * Instruction Jump Table
 * ============================================= */

typedef void (*handler)(M_VM*);

static const handler TABLE[256] = {
    [M_LIT]    = h_lit,
    [M_V]      = h_v,
    [M_LET]    = h_let,
    [M_SET]    = h_set,
    [M_ADD]    = h_add,
    [M_SUB]    = h_sub,
    [M_MUL]    = h_mul,
    [M_DIV]    = h_div,
    [M_AND]    = h_and,   /* Core - per spec (was M_MOD, now AND at 54) */
    [M_OR]     = h_or,
    [M_XOR]    = h_xor,
    [M_SHL]    = h_shl,
    [M_SHR]    = h_shr,
    [M_LT]     = h_lt,
    [M_GT]     = h_gt,
    [M_LE]     = h_le,
    [M_GE]     = h_ge,
    [M_EQ]     = h_eq,
    [M_DUP]    = h_dup,
    [M_DRP]    = h_drp,
    [M_ROT]    = h_rot,
    /* Array operations - both规范 defined (61-63) and legacy (67-69) */
    [M_LEN]    = h_len,
    [M_GET]    = h_get,
    [M_PUT]    = h_put,
    [M_SWP]    = h_swp,
    [M_GET_ALIAS] = h_get,   /* Legacy alias for backward compatibility */
    [M_PUT_ALIAS] = h_put,   /* Legacy alias for backward compatibility */
    [M_SWP_ALIAS] = h_swp,   /* Legacy alias for backward compatibility */
    /* Legacy array operations (moved to Extension range for clarity) */
    [M_NEWARR] = h_newarr,
    [M_IDX]    = h_idx,
    [M_STO]    = h_sto,
    [M_B]      = h_b,
    [M_E]      = h_e,
    [M_IF]     = h_if,
    [M_WH]     = h_wh,    /* While loop (now implemented) */
    [M_FR]     = h_fr,    /* For loop (placeholder) */
    [M_FN]     = h_fn,
    [M_RT]     = h_rt,
    [M_CL]     = h_cl,
    [M_HALT]   = h_halt,
    [M_GTWAY]  = h_gtway,
    [M_WAIT]   = h_wait,
    [M_IOW]    = h_iow,
    [M_IOR]    = h_ior,
    [M_TRACE]  = h_trace,
    [M_PH]     = h_ph,
    [M_GC]     = h_gc,
    [M_BP]     = h_bp,
    [M_STEP]   = h_step,
    [M_ALLOC]  = h_alloc,
    [M_FREE]   = h_free,
    /* Extension instructions (100-199) */
    [M_JZ]     = h_jz,
    [M_JNZ]    = h_jnz,   /* Jump if not zero (now implemented) */
    [M_JMP]    = h_jmp,
    /* Extension arithmetic (110+) */
    [M_MOD]    = h_mod,   /* Modulo - moved to Extension 110 */
    [M_NEG]    = h_neg,   /* Negate */
    [M_NOT]    = h_not,   /* Bitwise NOT */
    [M_NEQ]    = h_neq,   /* Not equal - moved to Extension 113 */
    /* Extension loop constructs (legacy, superseded by Core WH) */
    [M_DWHL]   = h_dwhl,
    [M_DO]     = h_do,
    [M_WHIL]   = h_whil
};

/* =============================================
 * Core Interface Implementation
 * ============================================= */

void m_vm_init(M_VM* vm, uint8_t* code, int len,
               void* io_w, void* io_r, void* sleep, void* trace) {
    memset(vm, 0, sizeof(M_VM));
    vm->code = code;
    vm->code_len = len;
    vm->pc = 0;
    vm->sp = -1;
    vm->rp = -1;
    vm->running = false;
    vm->authorized = false;
    vm->fault = M_FAULT_NONE;
    vm->last_pc = -1;
    vm->steps = 0;
    vm->step_limit = MAX_STEPS;
    vm->gas = 0;
    vm->gas_limit = 0;
    vm->call_depth = 0;
    vm->call_depth_limit = CALL_DEPTH_MAX;
    vm->stack_limit = STACK_SIZE;
    vm->local_count = 0;
    vm->frame_sp = -1;
    vm->alloc_head = NULL;
    vm->gc_enabled = false;
    vm->gc_threshold = 100;
    vm->last_op_index = -1;
    vm->token_offsets = NULL;
    vm->token_count = 0;
    vm->byte_to_token = NULL;
    vm->code_owned = NULL;
    caps_clear(vm);
    vm->io_write = (void (*)(uint8_t, M_Value))io_w;
    vm->io_read = (M_Value (*)(uint8_t))io_r;
    vm->sleep_ms = (void (*)(int32_t))sleep;
    vm->trace = (void (*)(uint32_t, const char*))trace;

    if (!lower_structured(vm)) {
        vm->fault = M_FAULT_BAD_ENCODING;
        return;
    }

    if (!build_token_map(vm)) {
        vm->fault = M_FAULT_BAD_ENCODING;
    }
}

void m_vm_set_step_limit(M_VM* vm, uint64_t limit) { 
    vm->step_limit = limit; 
}

void m_vm_set_gas_limit(M_VM* vm, uint64_t limit) { 
    vm->gas_limit = limit; 
}

void m_vm_set_call_depth_limit(M_VM* vm, int limit) {
    if (limit < 1) limit = 1;
    if (limit > CALL_DEPTH_MAX) limit = CALL_DEPTH_MAX;
    vm->call_depth_limit = limit;
}

void m_vm_set_stack_limit(M_VM* vm, int limit) {
    if (limit < 0) limit = 0;
    if (limit > STACK_SIZE) limit = STACK_SIZE;
    vm->stack_limit = limit;
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
    int call_depth_limit = vm->call_depth_limit;
    int stack_limit = vm->stack_limit;
    AllocNode* alloc_head = vm->alloc_head;  /* Keep allocations across reset */
    int* token_offsets = vm->token_offsets;
    int token_count = vm->token_count;
    int* byte_to_token = vm->byte_to_token;
    uint8_t* code_owned = vm->code_owned;

    memset(vm, 0, sizeof(M_VM));
    vm->code = code;
    vm->code_len = len;
    vm->pc = 0;
    vm->sp = -1;
    vm->rp = -1;
    vm->running = false;
    vm->authorized = false;
    vm->fault = M_FAULT_NONE;
    vm->last_pc = -1;
    vm->steps = 0;
    vm->step_limit = step_limit;
    vm->gas = 0;
    vm->gas_limit = gas_limit;
    vm->call_depth = 0;
    vm->call_depth_limit = call_depth_limit;
    vm->stack_limit = stack_limit > 0 ? stack_limit : STACK_SIZE;
    vm->local_count = 0;
    vm->frame_sp = -1;
    vm->alloc_head = alloc_head;
    vm->last_op_index = -1;
    vm->token_offsets = token_offsets;
    vm->token_count = token_count;
    vm->byte_to_token = byte_to_token;
    vm->code_owned = code_owned;
    caps_clear(vm);

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
    vm->last_pc = vm->pc;
    if (vm->byte_to_token) {
        vm->last_op_index = (vm->pc >= 0 && vm->pc < vm->code_len) ? vm->byte_to_token[vm->pc] : -1;
        if (vm->last_op_index < 0) {
            SET_FAULT(vm, M_FAULT_BAD_ENCODING);
            return -(int)vm->fault;
        }
    } else {
        vm->last_op_index = -1;
    }
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

    /* Check for single-step mode - pause after executing this instruction */
    if (vm->single_step) {
        vm->single_step = false;
        vm->running = false;
    }

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
    vm->last_pc = -1;
    vm->steps = 0;
    vm->gas = 0;
    vm->authorized = false;
    caps_clear(vm);
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
    if ((size_t)vm->rp + 1 >= (size_t)RET_STACK_SIZE) {
        SET_FAULT(vm, M_FAULT_RET_STACK_OVERFLOW);
        return -1;
    }
    vm->ret_stack[++vm->rp] = vm->code_len;  /* Return to end */

    /* Jump to function */
    if (func_id >= (uint32_t)vm->code_len) {
        SET_FAULT(vm, M_FAULT_PC_OOB);
        return -1;
    }
    vm->pc = (int)func_id;
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
        case M_FAULT_LOCALS_OOB:          return "LOCAL_OOB";
        case M_FAULT_GLOBALS_OOB:         return "GLOBAL_OOB";
        case M_FAULT_PC_OOB:              return "PC_OOB";
        case M_FAULT_DIV_BY_ZERO:         return "DIV_BY_ZERO";
        case M_FAULT_MOD_BY_ZERO:         return "MOD_BY_ZERO";
        case M_FAULT_UNKNOWN_OP:          return "BAD_OPCODE";
        case M_FAULT_STEP_LIMIT:          return "STEP_LIMIT";
        case M_FAULT_GAS_EXHAUSTED:       return "GAS_LIMIT";
        case M_FAULT_BAD_ENCODING:        return "BAD_VARINT";
        case M_FAULT_UNAUTHORIZED:        return "UNAUTHORIZED_IO";
        case M_FAULT_TYPE_MISMATCH:       return "TYPE_MISMATCH";
        case M_FAULT_INDEX_OOB:           return "ARRAY_OOB";
        case M_FAULT_BAD_ARG:             return "BAD_ARG";
        case M_FAULT_OOM:                 return "OOM";
        case M_FAULT_ASSERT_FAILED:       return "ASSERT_FAILED";
        case M_FAULT_BREAKPOINT:          return "BREAKPOINT";
        case M_FAULT_DEBUG_STEP:          return "DEBUG_STEP";
        case M_FAULT_CALL_DEPTH_LIMIT:    return "CALL_DEPTH_LIMIT";
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
        case M_WH:   return "WH";
        case M_FR:   return "FR";
        case M_RT:   return "RT";
        case M_CL:   return "CL";
        case M_PH:   return "PH";

        /* Data */
        case M_LIT:  return "LIT";
        case M_V:    return "V";
        case M_LET:  return "LET";
        case M_SET:  return "SET";

        /* Comparison - Core (40-44) */
        case M_LT:   return "LT";
        case M_GT:   return "GT";
        case M_LE:   return "LE";
        case M_GE:   return "GE";
        case M_EQ:   return "EQ";

        /* Arithmetic - Core (50-58) */
        case M_ADD:  return "ADD";
        case M_SUB:  return "SUB";
        case M_MUL:  return "MUL";
        case M_DIV:  return "DIV";
        case M_AND:  return "AND";   /* Core - per spec (54) */
        case M_OR:   return "OR";
        case M_XOR:  return "XOR";
        case M_SHL:  return "SHL";
        case M_SHR:  return "SHR";

        /* Stack */
        case M_DUP:  return "DUP";
        case M_DRP:  return "DRP";
        case M_ROT:  return "ROT";

        /* Array operations -规范 defined (61-63) */
        case M_LEN:  return "LEN";
        case M_GET:  return "GET";
        case M_PUT:  return "PUT";
        case M_SWP:  return "SWP";

        /* Legacy array operations (for backward compatibility) */
        case M_GET_ALIAS:  return "GET";
        case M_PUT_ALIAS:  return "PUT";
        case M_SWP_ALIAS:  return "SWP";
        case M_NEWARR:     return "NEWARR";
        case M_IDX:        return "IDX";
        case M_STO:        return "STO";

        /* IO */
        case M_IOW:  return "IOW";
        case M_IOR:  return "IOR";

        /* Memory - Platform Extension (200+) */
        case M_ALLOC: return "ALLOC";
        case M_FREE:  return "FREE";

        /* System */
        case M_GTWAY: return "GTWAY";
        case M_WAIT:  return "WAIT";
        case M_HALT:  return "HALT";
        case M_TRACE: return "TRACE";
        case M_GC:    return "GC";
        case M_BP:    return "BP";
        case M_STEP:  return "STEP";

        /* Extension instructions (100-199) */
        case M_JZ:   return "JZ";
        case M_JNZ:  return "JNZ";
        case M_JMP:  return "JMP";

        /* Extension arithmetic (110+) */
        case M_MOD:  return "MOD";   /* Extension - 110 */
        case M_NEG:  return "NEG";   /* Extension - 111 */
        case M_NOT:  return "NOT";   /* Extension - 112 */
        case M_NEQ:  return "NEQ";   /* Extension - 113 (was 45) */

        /* Extension loop constructs */
        case M_DO:   return "DO";
        case M_DWHL: return "DWHL";
        case M_WHIL: return "WHILE";

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

void m_vm_destroy(M_VM* vm) {
    /* Free all allocated memory */
    AllocNode* curr = vm->alloc_head;
    while (curr) {
        free(curr->ptr);
        AllocNode* next = curr->next;
        free(curr);
        curr = next;
    }
    vm->alloc_head = NULL;
    if (vm->token_offsets) {
        free(vm->token_offsets);
        vm->token_offsets = NULL;
    }
    if (vm->byte_to_token) {
        free(vm->byte_to_token);
        vm->byte_to_token = NULL;
    }
    vm->token_count = 0;
    if (vm->code_owned) {
        free(vm->code_owned);
        vm->code_owned = NULL;
    }
}

