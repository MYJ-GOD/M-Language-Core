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
#define M_WH   13   /* While loop: <cond>,WH,B<body>,E (Core) */
#define M_FR   14   /* For loop: <init>,<cond>,<inc>,FR,B<body>,E (Extension) */
#define M_RT   16   /* Return: RT,<value> */
#define M_CL   17   /* Call: CL,<func_id>,<argc>,<arg0>..<argN> */
#define M_PH   18   /* Placeholder (for alignment/padding) */

/* --- Extension Control Flow (100-199, not frozen) --- */
/* JZ, JMP, JNZ are Extension instructions for lowering structured control flow */
#define M_JZ   101  /* Jump if zero: <cond>,JZ,<svarint offset> (Extension) */
#define M_JMP  100  /* Unconditional jump: JMP,<svarint offset> (Extension) */
#define M_JNZ  102  /* Jump if not zero: <cond>,JNZ,<svarint offset> (Extension) */

/* --- Extension Loop Constructs (NOT ABI - for lowering only) --- */
#define M_DO    140 /* DO,<body>,WHILE,<cond> (Internal IR - NOT ABI) */
#define M_DWHL  141 /* Jump back if cond true (Internal IR - NOT ABI) */
#define M_WHIL  142 /* While loop IR (Internal IR - NOT ABI, use Core WH) */

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

/* --- Arithmetic / Bitwise (50-58, Core) --- */
#define M_ADD  50   /* Addition: a,b -> a+b */
#define M_SUB  51   /* Subtraction: a,b -> a-b */
#define M_MUL  52   /* Multiplication: a,b -> a*b */
#define M_DIV  53   /* Division: a,b -> a/b */
#define M_AND  54   /* Bitwise AND: a,b -> a&b */
#define M_OR   55   /* Bitwise OR: a,b -> a|b */
#define M_XOR  56   /* Bitwise XOR: a,b -> a^b */
#define M_SHL  57   /* Shift left: a,b -> a<<b */
#define M_SHR  58   /* Shift right: a,b -> a>>b */

/* --- Arithmetic Extension (110-119) --- */
#define M_MOD  110  /* Modulo: a,b -> a%b (Extension - C semantics, sign matches a) */
#define M_NEG  111  /* Negate: a -> -a (Extension) */
#define M_NOT  112  /* Bitwise NOT: a -> ~a (Extension) */
#define M_NEQ  113  /* Not equal: a,b -> (a!=b) (Extension) */

/* --- Array Operations (60-63) --- */
/* Note: These follow the M-Token specification exactly */
#define M_LEN    60   /* Array length: <array_ref> -> <length> */
#define M_GET    61   /* Array get: <array_ref>,<index> -> <element> */
#define M_PUT    62   /* Array put: <array_ref>,<index>,<value> -> <array_ref> */
#define M_SWP    63   /* Swap: a,b -> b,a */

/* --- Stack Operations (64-66) --- */
#define M_DUP  64   /* Duplicate top: a -> a,a */
#define M_DRP  65   /* Drop top: a -> (pop) */
#define M_ROT  66   /* Rotate top 3: a,b,c -> b,c,a */

/* --- Legacy Aliases (DEPRECATED - will be removed v2.0) --- */
/* These are kept for backward compatibility only - use M_GET/M_PUT/M_SWP instead */
#define M_GET_ALIAS  67   /* DEPRECATED: Use M_GET=61 */
#define M_PUT_ALIAS  68   /* DEPRECATED: Use M_PUT=62 */
#define M_SWP_ALIAS  69   /* DEPRECATED: Use M_SWP=63 */

/* --- Legacy Array Operations (120-122) --- */
/* For backward compatibility */
#define M_NEWARR 120  /* Array create: <size> -> <array_ref> (Extension) */
#define M_IDX    121  /* Array index: <array_ref>,<index> -> <element> (Extension) */
#define M_STO    122  /* Array store: <array_ref>,<index>,<value> -> <array_ref> (Extension) */

/* --- Platform/Hardware Extensions (200-239) --- */
#define M_ALLOC  200  /* Allocate: <size> -> <ptr> (Platform Extension) */
#define M_FREE   201  /* Free: <ptr> -> (Platform Extension) */

/* --- Hardware IO (70-79) --- */
#define M_IOW  70   /* IO Write: IOW,<device_id>,<value> */
#define M_IOR  71   /* IO Read: IOR,<device_id> -> <value> */

/* --- System (80-89) --- */
#define M_GTWAY 80  /* Gateway/Authorization: GATEWAY,<key> */
#define M_WAIT  81  /* Wait/Delay: WAIT,<milliseconds> */
#define M_HALT  82  /* Halt execution */
#define M_TRACE 83  /* Trace/Debug: TRACE,<level> */
#define M_GC    130 /* Manual GC trigger: GC (Extension) */
#define M_BP    131 /* Breakpoint: BP,<id> (Extension) */
#define M_STEP  132 /* Single step: STEP (Extension) */

/* --- VM Configuration --- */
#define STACK_SIZE     256
#define RET_STACK_SIZE 32
#define LOCALS_SIZE    64
#define GLOBALS_SIZE   128
#define MAX_STEPS      1000000
#define MAX_TRACE      1024
#define CALL_DEPTH_MAX 32     /* Maximum call depth for safety */

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
    M_FAULT_BAD_ARG,        /* Invalid argument (e.g., negative size) */
    M_FAULT_OOM,            /* Out of memory */
    M_FAULT_ASSERT_FAILED,
    M_FAULT_BREAKPOINT,     /* Breakpoint hit */
    M_FAULT_DEBUG_STEP,     /* Single-step pause */
    M_FAULT_CALL_DEPTH_LIMIT /* Call depth exceeded limit */
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
    int64_t  stack_top;
    int      sp;
} M_TraceEntry;

/* --- Simulation Result --- */
typedef struct {
    bool     completed;
    bool     halted;
    M_Fault  fault;
    uint64_t steps;
    int64_t  result;
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
        int64_t        i;          /* Core numeric type: i64 per spec */
        double         f;          /* Float: double per spec */
        bool           b;
        struct M_Array* array_ptr;  /* Pointer to M_Array */
        struct {
            const char* str;
            int64_t     len;       /* Use int64_t per spec */
        } s;
        void*          ref;
    } u;
} M_Value;

/* --- M Array (dynamic array) --- */
typedef struct M_Array {
    int64_t  len;           /* Use int64_t per spec */
    int64_t  cap;           /* Use int64_t per spec */
    M_Value  data[];        /* Flexible array member - stores M_Value elements */
} M_Array;

/* --- Allocation tracking node --- */
typedef struct AllocNode {
    void*            ptr;
    struct AllocNode* next;
} AllocNode;

/* --- VM Structure --- */
typedef struct M_VM {
    /* Storage */
    uint8_t* code;
    int      code_len;
    int      pc;
    uint8_t* code_owned;   /* Lowered/owned bytecode buffer (if any) */

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

    /* Memory allocation tracking */
    AllocNode* alloc_head;
    int        alloc_count;      /* Total allocations since last GC */
    int        gc_threshold;     /* Trigger GC when count exceeds this */
    bool       gc_enabled;       /* Enable automatic GC */

    /* Debugging state */
    bool       single_step;      /* Single-step mode */
    int        breakpoint_id;    /* Current breakpoint */

    /* State */
    bool     running;
    bool     authorized;
    uint8_t  caps[32];     /* Capability bitmap for device_id 0..255 */

    /* Execution limits */
    uint64_t steps;
    uint64_t step_limit;
    uint64_t gas;
    uint64_t gas_limit;
    int      call_depth;        /* Current call depth */
    int      call_depth_limit;  /* Maximum call depth (default: CALL_DEPTH_MAX) */
    int      stack_limit;       /* Runtime stack limit (<= STACK_SIZE) */

    /* Fault tracking */
    M_Fault  fault;
    int      last_pc;
    uint32_t last_op;
    int      last_op_index;

    /* Opcode token index map (for jump offsets in opcode units) */
    int*     token_offsets;   /* opcode index -> byte offset */
    int      token_count;
    int*     byte_to_token;   /* byte offset -> opcode index (or -1) */

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
bool m_vm_decode_uvarint64(const uint8_t* code, int* pc, int len, uint64_t* out);
bool m_vm_decode_svarint(const uint8_t* code, int* pc, int len, int32_t* out);

int m_vm_encode_uvarint(uint32_t n, uint8_t* out);
int m_vm_encode_uvarint64(uint64_t n, uint8_t* out);

int32_t m_vm_decode_zigzag(uint32_t n);

uint32_t m_vm_encode_zigzag(int32_t n);

int64_t m_vm_decode_zigzag64(uint64_t n);

uint64_t m_vm_encode_zigzag64(int64_t n);

/* =============================================
 * Core Interface
 * ============================================= */

void m_vm_init(M_VM* vm, uint8_t* code, int len, void* io_w, void* io_r, void* sleep, void* trace);

void m_vm_set_step_limit(M_VM* vm, uint64_t limit);

void m_vm_set_gas_limit(M_VM* vm, uint64_t limit);

void m_vm_set_call_depth_limit(M_VM* vm, int limit);

void m_vm_set_stack_limit(M_VM* vm, int limit);

void m_vm_reset(M_VM* vm);

M_VM_State m_vm_get_state(M_VM* vm);

int m_vm_run(M_VM* vm);

int m_vm_step(M_VM* vm);

int m_vm_simulate(M_VM* vm, M_SimResult* result);

const char* m_vm_fault_string(M_Fault fault);

const char* m_vm_opcode_name(uint32_t op);

int m_vm_stack_snapshot(M_VM* vm, M_Value* out_stack);

/* Destroy VM and free all allocated memory */
void m_vm_destroy(M_VM* vm);

/* Garbage Collection */
void m_vm_gc(M_VM* vm);              /* Trigger garbage collection */
void m_vm_gc_enable(M_VM* vm, bool enable);  /* Enable/disable auto GC */
void m_vm_set_gc_threshold(M_VM* vm, int threshold);  /* Set GC threshold */

/* Debugging */
void m_vm_single_step(M_VM* vm, bool enable);  /* Enable/disable single-step */
int m_vm_set_breakpoint(M_VM* vm, int pc, int id);  /* Set breakpoint */
int m_vm_clear_breakpoint(M_VM* vm, int pc);  /* Clear breakpoint */
void m_vm_clear_all_breakpoints(M_VM* vm);  /* Clear all breakpoints */

/* JIT Compilation */
void m_vm_jit_enable(M_VM* vm, bool enable);  /* Enable/disable JIT */
void m_vm_jit_set_threshold(M_VM* vm, int threshold);  /* Set JIT threshold */
bool m_vm_jit_compile(M_VM* vm, int start_pc, int end_pc);  /* Compile bytecode range */

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
