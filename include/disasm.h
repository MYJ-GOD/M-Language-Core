#ifndef M_DISASM_H
#define M_DISASM_H

#include <stdint.h>
#include <stdbool.h>
#include "m_vm.h"

/**
 * M Language Disassembler
 * 
 * Converts M-Token bytecode to human-readable format
 */

/**
 * Disassemble bytecode to readable format
 * @param code Bytecode
 * @param len Bytecode length
 * @return Disassembly string (static buffer, not thread-safe)
 */
const char* m_disasm(const uint8_t* code, int len);

/**
 * Print stack snapshot
 */
void m_disasm_print_stack(M_Value* stack, int sp);

/**
 * Print execution trace summary
 */
void m_disasm_print_trace(M_SimResult* result);

/**
 * Full disassembly report (disassembly + trace)
 */
void m_disasm_full_report(const uint8_t* code, int len, M_SimResult* result);

#endif /* M_DISASM_H */
