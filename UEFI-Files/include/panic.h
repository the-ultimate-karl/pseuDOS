#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr2;
    uint64_t cr3;
    uint64_t vector;
    uint64_t error_code;
    const char *vector_name;

    /* Extended CPU register context */
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;
    uint64_t cs, ss;
} panic_context_t;

void kernel_panic(const char *reason, const panic_context_t *ctx) __attribute__((noreturn));

#endif /* PANIC_H */
