#include "process.h"
#include "scheduler.h"
#include "vmm.h"
#include "lib.h"
#include "drivers.h"
#include "klog.h"
#include "gdt.h"
#include "syscall.h"
#include "panic.h"

extern uint64_t g_current_kernel_rsp;

static process_t g_process_table[MAX_PROCESSES];
static process_t *g_current_process = NULL;
static uint32_t g_next_pid = 1;

process_t *process_get_current(void) {
    return g_current_process;
}

void process_set_current(process_t *proc) {
    g_current_process = proc;
    if (proc) {
        if (proc->kernel_stack) {
            g_current_kernel_rsp = proc->kernel_stack;
            tss_set_rsp0(proc->kernel_stack);
        }
        g_current_process_privilege = (uint64_t)proc->privilege_level;
    }
}

process_t *process_get_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table[i].state != PROCESS_STATE_UNUSED && g_process_table[i].pid == pid) {
            return &g_process_table[i];
        }
    }
    return NULL;
}

static void process_trampoline(void) {
    process_t *p = process_get_current();
    if (p && p->entry) {
        p->entry();
    }
    process_exit(0);
}

void process_init(void) {
    memset(g_process_table, 0, sizeof(g_process_table));

    /* Initialize PID 0: Kernel idle/main thread */
    process_t *kproc = &g_process_table[0];
    kproc->pid = 0;
    kproc->ppid = 0;
    strncpy(kproc->name, "kernel", sizeof(kproc->name) - 1);
    kproc->state = PROCESS_STATE_RUNNING;
    kproc->privilege_level = PRIV_KERNEL;
    kproc->cr3 = (uint64_t)(uintptr_t)vmm_get_kernel_pml4();
    kproc->time_slice = DEFAULT_TIME_SLICE;
    kproc->total_ticks = 0;
    strncpy(kproc->cwd, "/", sizeof(kproc->cwd) - 1);

    /* Allocate dedicated 16KB kernel stack for PID 0 */
    kproc->kernel_stack_base = kcalloc(1, PROCESS_STACK_SIZE);
    if (kproc->kernel_stack_base) {
        kproc->kernel_stack = ((uint64_t)(uintptr_t)kproc->kernel_stack_base + PROCESS_STACK_SIZE) & ~0xFULL;
    }

    process_set_current(kproc);
    klog_info("Process manager initialized. PID 0 (kernel) created.");
}

process_t *process_create(const char *name, void (*entry)(void), process_privilege_t priv) {
    if (!entry) return NULL;

    /* Find free slot */
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (g_process_table[i].state == PROCESS_STATE_UNUSED || g_process_table[i].state == PROCESS_STATE_KILLED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        klog_warn("Process table full! Cannot create process '%s'", name ? name : "unnamed");
        return NULL;
    }

    process_t *p = &g_process_table[slot];

    /* Clean up previous stack memory if reused killed slot */
    if (p->kernel_stack_base) {
        kfree(p->kernel_stack_base);
        p->kernel_stack_base = NULL;
    }
    if (p->user_stack_base) {
        kfree(p->user_stack_base);
        p->user_stack_base = NULL;
    }

    memset(p, 0, sizeof(process_t));

    p->pid = g_next_pid++;
    p->ppid = g_current_process ? g_current_process->pid : 0;
    strncpy(p->name, name ? name : "process", sizeof(p->name) - 1);
    p->privilege_level = priv;
    p->entry = entry;
    p->cr3 = (uint64_t)(uintptr_t)vmm_get_kernel_pml4();
    p->time_slice = DEFAULT_TIME_SLICE;
    p->total_ticks = 0;
    strncpy(p->cwd, g_current_process ? g_current_process->cwd : "/", sizeof(p->cwd) - 1);

    /* Allocate dedicated 16KB kernel stack */
    p->kernel_stack_base = kcalloc(1, PROCESS_STACK_SIZE);
    if (!p->kernel_stack_base) {
        klog_err("Failed to allocate kernel stack for process %d", p->pid);
        p->state = PROCESS_STATE_UNUSED;
        return NULL;
    }
    /* Align stack top to 16 bytes */
    p->kernel_stack = ((uint64_t)(uintptr_t)p->kernel_stack_base + PROCESS_STACK_SIZE) & ~0xFULL;

    /* Context setup for iretq trampoline */
    p->context.rip = (uint64_t)(uintptr_t)process_trampoline;
    p->context.rflags = 0x202; /* IF enabled */

    p->context.cs = 0x08; /* Kernel Code Segment (RPL 0) */
    p->context.ss = 0x10; /* Kernel Data Segment (RPL 0) */
    p->context.rsp = p->kernel_stack;

    p->state = PROCESS_STATE_READY;
    klog_info("Created process PID %u '%s' (priv=%s)", p->pid, p->name, priv == PRIV_KERNEL ? "KERNEL" : "USER");
    return p;
}

int process_kill(uint32_t pid) {
    if (pid == 0) {
        console_puts("kill: cannot kill kernel idle process (PID 0)\n");
        return -1;
    }
    if (pid == 1) {
        kernel_panic("Attempted to kill init! (PID 1)", NULL);
    }

    process_t *p = process_get_by_pid(pid);
    if (!p) {
        console_printf("kill: process %u not found\n", pid);
        return -1;
    }

    if (p->pid == 1) {
        kernel_panic("Attempted to kill init! (PID 1)", NULL);
    }

    p->state = PROCESS_STATE_KILLED;
    p->exit_code = -9;
    klog_info("Killed process PID %u '%s'", pid, p->name);

    if (p == g_current_process) {
        scheduler_yield();
    }
    return 0;
}

void process_exit(int exit_code) {
    process_t *p = process_get_current();
    if (!p || p->pid == 0) {
        /* Idle process cannot exit */
        while (1) {
            __asm__ volatile ("hlt");
        }
    }

    if (p->pid == 1) {
        char panic_msg[64];
        snprintf(panic_msg, sizeof(panic_msg), "init (PID 1) exited with code %d", exit_code);
        kernel_panic(panic_msg, NULL);
    }

    p->exit_code = exit_code;
    p->state = PROCESS_STATE_KILLED;
    klog_info("Process PID %u '%s' exited with code %d", p->pid, p->name, exit_code);
    scheduler_yield();

    /* Should not be reached */
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void process_dump_list(void) {
    console_puts("PID    PPID   STATE       PRIVILEGE   TICKS      NAME\n");
    console_puts("-------------------------------------------------------------\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &g_process_table[i];
        if (p->state == PROCESS_STATE_UNUSED) continue;

        console_printf("%u      %u      ", p->pid, p->ppid);
        switch (p->state) {
            case PROCESS_STATE_READY:   console_puts("READY       "); break;
            case PROCESS_STATE_RUNNING: console_puts("RUNNING     "); break;
            case PROCESS_STATE_BLOCKED: console_puts("BLOCKED     "); break;
            case PROCESS_STATE_KILLED:  console_puts("KILLED      "); break;
            default:                    console_puts("UNKNOWN     "); break;
        }
        if (p->privilege_level == PRIV_KERNEL) {
            console_puts("KERNEL      ");
        } else {
            console_puts("USER        ");
        }
        console_printf("%u          %s\n", (uint32_t)p->total_ticks, p->name);
    }
}
