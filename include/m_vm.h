#ifndef M_VM_H
#define M_VM_H

#include <stdint.h>
#include <stdbool.h>

/* 指令集定义 (ISA) */
#define M_LIT  0x30  // 立即数压栈
#define M_VAR  0x31  // 读取变量
#define M_LET  0x32  // 快速存入变量
#define M_ADD  0x50  // 加法
#define M_GT   0x41  // 大于判断
#define M_IF   0x12  // 条件跳转
#define M_GATE 0x8F  // 安全网关
#define M_PUT  0x62  // 硬件写入
#define M_HALT 0x80  // 停机

/* 虚拟机参数 */
#define STACK_SIZE 256
#define LOCALS_SIZE 64

typedef struct {
    uint8_t* code;
    int32_t stack[STACK_SIZE];
    int32_t locals[LOCALS_SIZE];
    int sp;
    int pc;
    int code_len;
    bool running;
    bool authenticated;
} M_VM;

/* 外部调用接口 */
void m_vm_init(M_VM* vm, uint8_t* code, int len, int32_t* initial_locals);
void m_vm_run(M_VM* vm);

#endif