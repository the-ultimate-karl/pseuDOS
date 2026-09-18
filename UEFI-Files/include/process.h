#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>

/* Process States */
typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    PROCESS_STATE_KILLED
} process_state_t;

/* Privilege Levels */
typedef enum {
    PRIV_USER = 0,
    PRIV_KERNEL = 1
} process_privilege_t;

/* Process Execution Context (Registers + Stack Frame) */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags, rsp, ss;
} process_context_t;

#define MAX_PROCESSES 64
#define PROCESS_NAME_MAX 32
#define PROCESS_STACK_SIZE 65536 /* 64 KB stack */

typedef struct process {
    uint32_t pid;
    uint32_t ppid;
    char name[PROCESS_NAME_MAX];
    process_state_t state;
    process_privilege_t privilege_level;
    uint64_t cr3;

    process_context_t context;

    void *kernel_stack_base;
    uint64_t kernel_stack;    /* Top of kernel stack (RSP0) */
    void *user_stack_base;
    uint64_t user_stack;      /* Top of user stack */

    void *image_base;
    size_t image_size;

    void (*entry)(void);
    int exit_code;
    uint64_t time_slice;
    uint64_t total_ticks;
    char cwd[256];
} process_t;

/* Core Process API */
void process_init(void);
process_t *process_create(const char *name, void (*entry)(void), process_privilege_t priv);
process_t *process_get_current(void);
void process_set_current(process_t *proc);
process_t *process_get_by_pid(uint32_t pid);
process_t *process_get_by_slot(int slot);
int process_get_slot(const process_t *proc);
int process_kill(uint32_t pid);
void process_exit(int exit_code);
void process_dump_list(void);
void process_free_resources(process_t *proc);

#endif /* PROCESS_H */
