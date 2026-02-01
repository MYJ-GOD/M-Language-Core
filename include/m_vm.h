#ifndef M_VM_PRO_H
#define M_VM_PRO_H

#include <stdint.h>
#include <stdbool.h>

/* =============================================
 * M Language Bytecode VM (M-VM)
 * ============================================= */

/* =============================================
 * M-Token Opcode Specification (Full Varint Encoding)
 * =============================================
 * All tokens use varint encoding
 * Format: FN,<arity>,B,<body>,E
 * Scoping: DeBruijn indices
 * Evaluation: Stack/SSA hybrid
 * ============================================= */

/* --- Control Flow (10-18) --- */
#define M_FN   15   /* Function definition: FN,<arity>,B,<body>,E */
#define M_B    10   /* Block begin */
#define M_E    11   /* Block end */
#define M_IF   12   /* Conditional: <cond>,IF,B,<then>,E,B,<else>,E */
#define M_JZ   13   /* Jump if zero: <cond>,JZ,<target_addr> */
#define M_JMP  14   /* Unconditional jump: JMP,<target_addr> */
#define M_RT   16   /* Return: RT,<value> */
#define M_CL   17   /* Call: CL,<func_id>,<argc>,<arg0>..<argN> */
#define M_PH   18   /* Placeholder (for alignment/padding) */

/* --- Data Operations (30-39) --- */
#define M_LIT  30   /* Literal: LIT,<varint|dict_id> */
#define M_V    31   /* Variable reference: V,<index> (DeBruijn) */
#define M_LET  32   /* Local variable assignment: LET,<index>,<value> */
#define M_SET  33   /* Global variable assignment: SET,<name_id>,<value> */

/* --- Comparison (40-49) --- */
#define M_LT   40   /* Less than: a,b -> a<b (1:true, 0:false) */
#define M_GT   41   /* Greater than */
#define M_LE   42   /* Less than or equal */
#define M_GE   43   /* Greater than or equal */
#define M_EQ   44   /* Equal */

/* --- Arithmetic (50-59) --- */
#define M_ADD  50   /* Addition: a,b -> a+b */
#define M_SUB  51   /* Subtraction: a,b -> a-b */
#define M_MUL  52   /* Multiplication: a,b -> a*b */
#define M_DIV  53   /* Division: a,b -> a/b */
#define M_AND  54   /* Bitwise AND: a,b -> a&b */
#define M_OR   55   /* Bitwise OR: a,b -> a|b */
#define M_XOR  56   /* Bitwise XOR: a,b -> a^b */
#define M_SHL  57   /* Shift left: a,b -> a<<b */
#define M_SHR  58   /* Shift right: a,b -> a>>b */

/* --- Array Operations (60-63) --- */
#define M_LEN  60   /* Array length: <array_ref> -> <length> */
#define M_GET  61   /* Array get: <array_ref>,<index> -> <element> */
#define M_PUT  62   /* Array put: <array_ref>,<index>,<value> -> <array_ref> */
#define M_SWP  63   /* Swap: a,b -> b,a (swap top two) */

/* --- Stack Operations (64-69) --- */
#define M_DUP  64   /* Duplicate top: a -> a,a */
#define M_DRP  65   /* Drop top: a -> (pop) */
#define M_ROT  66   /* Rotate top 3: a,b,c -> b,c,a */

/* --- Hardware IO (70-79) --- */
#define M_IOW  70   /* IO Write: IOW,<device_id>,<value> */
#define M_IOR  71   /* IO Read: IOR,<device_id> -> <value> */

/* --- System (80-89) --- */
#define M_GTWAY 80  /* Gateway/Authorization: GATEWAY,<key> */
#define M_WAIT  81  /* Wait/Delay: WAIT,<milliseconds> */
#define M_HALT  82  /* Halt execution */
#define M_TRACE 83  /* Trace/Debug: TRACE,<level> */

/* --- VM Configuration --- */
#define STACK_SIZE     256
#define RET_STACK_SIZE 32
#define LOCALS_SIZE    64
#define GLOBALS_SIZE   128
#define MAX_STEPS      1000000
#define MAX_TRACE      1024

/* --- Fault Codes --- */
typedef enum {
    M_FAULT_NONE = 0,
    M_FAULT_STACK_OVERFLOW,
    M_FAULT_STACK_UNDERFLOW,
    M_FAULT_RET_STACK_OVERFLOW,
    M_FAULT_RET_STACK_UNDERFLOW,
    M_FAULT_LOCALS_OOB,
    M_FAULT_GLOBALS_OOB,
    M_FAULT_PC_OOB,
    M_FAULT_DIV_BY_ZERO,
    M_FAULT_MOD_BY_ZERO,
    M_FAULT_UNKNOWN_OP,
    M_FAULT_STEP_LIMIT,
    M_FAULT_GAS_EXHAUSTED,
    M_FAULT_BAD_ENCODING,
    M_FAULT_UNAUTHORIZED,
    M_FAULT_TYPE_MISMATCH,
    M_FAULT_INDEX_OOB,
    M_FAULT_ASSERT_FAILED
} M_Fault;

/* Authorization key */
#ifndef M_GATEWAY_KEY
#define M_GATEWAY_KEY 2024u
#endif

/* --- VM Running State --- */
typedef enum {
    M_STATE_STOPPED = 0,
    M_STATE_RUNNING,
    M_STATE_FAULT
} M_VM_State;

/* --- Execution Trace Entry --- */
typedef struct {
    uint64_t step;
    int      pc;
    uint32_t op;        /* Full varint opcode */
    int32_t  stack_top;
    int      sp;
} M_TraceEntry;

/* --- Simulation Result --- */
typedef struct {
    bool     completed;
    bool     halted;
    M_Fault  fault;
    uint64_t steps;
    int32_t  result;
    int      sp;
    M_TraceEntry trace[MAX_TRACE];
    int      trace_len;
} M_SimResult;

/* --- Value Types --- */
typedef enum {
    M_TYPE_INT = 0,
    M_TYPE_FLOAT,
    M_TYPE_BOOL,
    M_TYPE_ARRAY,
    M_TYPE_STRING,
    M_TYPE_REF
} M_Type;

/* --- M Value (tagged union) --- */
typedef struct M_Value {
    M_Type   type;
    union {
        int32_t  i;
        float    f;
        bool     b;
        struct {
            int32_t* data;
            int32_t  len;
        } arr;
        struct {
            const char* str;
            int32_t    len;
        } s;
        void*    ref;
    } u;
} M_Value;

/* --- VM Structure --- */
typedef struct M_VM {
    /* Storage */
    uint8_t* code;
    int      code_len;
    int      pc;

    /* Stacks */
    M_Value  stack[STACK_SIZE];
    int      sp;
    int      ret_stack[RET_STACK_SIZE];
    int      rp;

    /* Variables */
    M_Value  locals[LOCALS_SIZE];
    int      local_count;     /* Current scope depth */
    M_Value  locals_frames[RET_STACK_SIZE][LOCALS_SIZE];
    int      frame_sp;

    /* Globals */
    M_Value  globals[GLOBALS_SIZE];

    /* State */
    bool     running;
    bool     authorized;

    /* Execution limits */
    uint64_t steps;
    uint64_t step_limit;
    uint64_t gas;
    uint64_t gas_limit;

    /* Fault tracking */
    M_Fault  fault;
    uint32_t last_op;

    /* External hooks */
    void (*io_write)(uint8_t device_id, M_Value value);
    M_Value (*io_read)(uint8_t device_id);
    void (*sleep_ms)(int32_t ms);
    void (*trace)(uint32_t level, const char* msg);
} M_VM;

/* =============================================
 * Varint Encoding/Decoding
 * ============================================= */

bool m_vm_decode_uvarint(const uint8_t* code, int* pc, int len, uint32_t* out);

int m_vm_encode_uvarint(uint32_t n, uint8_t* out);

int32_t m_vm_decode_zigzag(uint32_t n);

uint32_t m_vm_encode_zigzag(int32_t n);

/* =============================================
 * Core Interface
 * ============================================= */

void m_vm_init(M_VM* vm, uint8_t* code, int len,
               void (*io_w)(uint8_t, M_Value),
               M_Value (*io_r)(uint8_t),
               void (*sleep)(int32_t),
               void (*trace)(uint32_t, const char*));

void m_vm_set_step_limit(M_VM* vm, uint64_t limit);

void m_vm_set_gas_limit(M_VM* vm, uint64_t limit);

void m_vm_reset(M_VM* vm);

M_VM_State m_vm_get_state(M_VM* vm);

int m_vm_run(M_VM* vm);

int m_vm_step(M_VM* vm);

int m_vm_simulate(M_VM* vm, M_SimResult* result);

const char* m_vm_fault_string(M_Fault fault);

const char* m_vm_opcode_name(uint32_t op);

int m_vm_stack_snapshot(M_VM* vm, M_Value* out_stack);

/* =============================================
 * High-Level API (M-Token format support)
 * ============================================= */

/* Execute a function call: CL,<func_id>,<argc>,<args...> */
int m_vm_call(M_VM* vm, uint32_t func_id, int argc, M_Value* args);

/* Execute a block: B...E */
int m_vm_exec_block(M_VM* vm, int start_pc, int end_pc);

/* Evaluate conditional: <cond>,IF,B,<then>,E,B,<else>,E */
int m_vm_exec_if(M_VM* vm, int start_pc, int* consumed);

/* Evaluate while loop: <cond>,WH,B,<body>,E */
int m_vm_exec_while(M_VM* vm, int start_pc, int* consumed);

/* Evaluate for loop: <init>,<cond>,<inc>,FR,B,<body>,E */
int m_vm_exec_for(M_VM* vm, int start_pc, int* consumed);

/* Define function: FN,<arity>,B,<body>,E */
int m_vm_define_function(M_VM* vm, uint32_t arity, int start_pc, int end_pc);

#endif
