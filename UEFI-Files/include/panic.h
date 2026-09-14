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
} panic_context_t;

void kernel_panic(const char *reason, const panic_context_t *ctx) __attribute__((noreturn));

#endif /* PANIC_H */
