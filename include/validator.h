#ifndef M_VALIDATOR_H
#define M_VALIDATOR_H

#include <stdbool.h>
#include <stdint.h>

/* Validator result */
typedef struct {
    bool     valid;
    int      fault_code;
    int      pc;
    char     msg[128];
} M_ValidatorResult;

/* Initialize validator result */
void m_validator_result_init(M_ValidatorResult* result);

/* Main validation entry point */
M_ValidatorResult m_validate(const uint8_t* code, int len);

/* Individual validation checks */
bool m_validate_opcodes(const uint8_t* code, int len, M_ValidatorResult* result);
bool m_validate_varints(const uint8_t* code, int len, M_ValidatorResult* result);
bool m_validate_blocks(const uint8_t* code, int len, M_ValidatorResult* result);
bool m_validate_locals(const uint8_t* code, int len, M_ValidatorResult* result);

#endif /* M_VALIDATOR_H */

