#include "process.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "lib.h"
#include "drivers.h"
#include "klog.h"
#include "gdt.h"
#include "syscall.h"
#include "panic.h"
#include "ipc.h"
#include "shm.h"
#include "pit.h"

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

process_t *process_get_by_slot(int slot) {
    if (slot < 0 || slot >= MAX_PROCESSES) return NULL;
    return &g_process_table[slot];
}

int process_get_slot(const process_t *proc) {
    if (!proc) return -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (&g_process_table[i] == proc) return i;
    }
    return -1;
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

void process_free_resources(process_t *p) {
    if (!p) return;
    if (p->pid == 0 || p->pid == 1) return;

    if (p->kernel_stack_base) {
        kfree(p->kernel_stack_base);
        p->kernel_stack_base = NULL;
        p->kernel_stack = 0;
    }
    if (p->user_stack_base) {
        kfree(p->user_stack_base);
        p->user_stack_base = NULL;
        p->user_stack = 0;
    }
    if (p->image_base) {
        kfree(p->image_base);
        p->image_base = NULL;
        p->image_size = 0;
    }

    ipc_close_process_sockets(p->pid);
    shm_cleanup_process(p->pid);

    if (p->cr3 && p->cr3 != (uint64_t)(uintptr_t)vmm_get_kernel_pml4()) {
        vmm_destroy_user_address_space((uint64_t *)(uintptr_t)p->cr3);
        p->cr3 = 0;
    }

    p->state = PROCESS_STATE_UNUSED;
}

int process_has_active_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &g_process_table[i];
        if (p->state != PROCESS_STATE_UNUSED && p->state != PROCESS_STATE_KILLED) {
            if (p->pid == 1) return 1;
            if (p->name[0] && strstr(p->name, "autoinit") != NULL) return 1;
        }
    }
    return 0;
}

process_t *process_create(const char *name, void (*entry)(void), process_privilege_t priv) {
    if (!entry) return NULL;

    /* Detect unexpected second instance of init process (autoinit) */
    if (name && strstr(name, "autoinit") != NULL) {
        if (process_has_active_init()) {
            klog_err("CRITICAL: Detected second instance of init process '%s'!", name);
            kernel_panic("unexpected second instance of init process detected!", NULL);
        }
    }

    /* 1. Prefer truly unused slots */
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (g_process_table[i].state == PROCESS_STATE_UNUSED) {
            slot = i;
            break;
        }
    }

    /* 2. If table is full, reclaim oldest un-reaped KILLED zombie (except PID 0 and 1) */
    if (slot < 0) {
        for (int i = 1; i < MAX_PROCESSES; i++) {
            if (g_process_table[i].state == PROCESS_STATE_KILLED &&
                g_process_table[i].pid != 0 && g_process_table[i].pid != 1) {
                klog_info("Process table full: reclaiming zombie PID %u '%s' in slot %d",
                          g_process_table[i].pid, g_process_table[i].name, i);
                process_free_resources(&g_process_table[i]);
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) {
        klog_warn("Process table full! Cannot create process '%s'", name ? name : "unnamed");
        return NULL;
    }

    process_t *p = &g_process_table[slot];

    /* Clean up any existing resources if slot was previously used */
    process_free_resources(p);

    memset(p, 0, sizeof(process_t));

    p->pid = g_next_pid++;
    p->ppid = g_current_process ? g_current_process->pid : 0;
    strncpy(p->name, name ? name : "process", sizeof(p->name) - 1);
    p->privilege_level = priv;
    p->entry = entry;
    uint64_t *user_pml4 = vmm_create_user_address_space();
    p->cr3 = user_pml4 ? (uint64_t)(uintptr_t)user_pml4 : (uint64_t)(uintptr_t)vmm_get_kernel_pml4();
    p->time_slice = DEFAULT_TIME_SLICE;
    p->total_ticks = 0;
    p->tty = (g_current_process && g_current_process->tty != 0) ? g_current_process->tty : 1;
    strncpy(p->cwd, g_current_process ? g_current_process->cwd : "/", sizeof(p->cwd) - 1);
    p->cmdline[0] = '\0';

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
    if (pid < 3) {
        kernel_panic("Attempted to kill critical processes!", NULL);
    }

    process_t *p = process_get_by_pid(pid);
    if (!p) {
        console_printf("kill: process %u not found\n", pid);
        return -1;
    }

    if (p->pid < 3) {
        kernel_panic("Attempted to kill critical processes!", NULL);
    }

    p->state = PROCESS_STATE_KILLED;
    p->exit_code = -9;
    klog_info("Killed process PID %u '%s'", pid, p->name);
    ipc_close_process_sockets(p->pid);
    shm_cleanup_process(p->pid);

    if (p == g_current_process) {
        scheduler_yield();
    }
    return 0;
}

void process_exit(int exit_code) {
    process_t *p = process_get_current();
    if (!p || p->pid == 0) {
        kernel_panic("Attempted to kill critical processes!", NULL);
    }

    if (p->pid < 3) {
        kernel_panic("Attempted to kill critical processes!", NULL);
    }

    p->exit_code = exit_code;
    p->state = PROCESS_STATE_KILLED;
    klog_info("Process PID %u '%s' exited with code %d", p->pid, p->name, exit_code);
    ipc_close_process_sockets(p->pid);
    shm_cleanup_process(p->pid);
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

void system_shutdown_sequence(int is_reboot) {
    klog_info("System shutdown sequence initiated (is_reboot=%d)", is_reboot);

    /* 1. Stop multitasking preemption so no userland process can interrupt or write to fb */
    scheduler_disable();

    /* 2. Determine screen dimensions and clear to classic Windows teal */
    uint32_t width = fb_get_width();
    uint32_t height = fb_get_height();
    if (width == 0) width = 1280;
    if (height == 0) height = 720;

    uint32_t color_bg = 0x00008080; /* Classic Desktop Teal */
    uint32_t color_fg = 0x00FFFFFF; /* White */

    fb_clear(color_bg);

    /* 3. Draw centered Line 1: "shutting down system..." or "restarting system..." */
    const char *line1 = is_reboot ? "restarting system..." : "shutting down system...";
    int len1 = (int)strlen(line1);
    int x1 = ((int)width - (len1 * 16)) / 2;
    int y1 = ((int)height / 2) - 28;
    fb_draw_string_scaled((uint32_t)x1, (uint32_t)y1, line1, color_fg, color_bg, 2);

    int y2 = ((int)height / 2) + 16;

    /* 4. Kill user applications and active services in reverse order of creation */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = MAX_PROCESSES - 1; i >= 1; i--) {
            process_t *p = &g_process_table[i];
            if (p->state == PROCESS_STATE_UNUSED || p->state == PROCESS_STATE_KILLED) {
                continue;
            }
            if (pass == 0 && p->pid <= 5) continue;
            if (pass == 1 && p->pid > 5) continue;

            /* Draw centered Line 2: "Killing process: <p->name>" */
            char line2[64];
            snprintf(line2, sizeof(line2), "Killing process: %s", p->name);
            int len2 = (int)strlen(line2);
            int x2 = ((int)width - (len2 * 16)) / 2;

            /* Clear line 2 area and draw updated message */
            fb_fill_rect(0, (uint32_t)(y2 - 6), width, 44, color_bg);
            fb_draw_string_scaled((uint32_t)x2, (uint32_t)y2, line2, color_fg, color_bg, 2);

            klog_info("Shutdown: terminating PID %u '%s'", p->pid, p->name);

            /* Perform process termination */
            if (p->pid < 3 || p == g_current_process) {
                p->state = PROCESS_STATE_KILLED;
                p->exit_code = 0;
                ipc_close_process_sockets(p->pid);
                shm_cleanup_process(p->pid);
            } else {
                process_kill(p->pid);
            }

            /* Verbose delay so each process closing is visible */
            pit_sleep_ms(250);
        }
    }

    /* 5. Final status */
    fb_fill_rect(0, (uint32_t)(y2 - 6), width, 44, color_bg);
    const char *fin_msg = is_reboot ? "Restarting..." : "Power off.";
    int fin_len = (int)strlen(fin_msg);
    int fin_x = ((int)width - (fin_len * 16)) / 2;
    fb_draw_string_scaled((uint32_t)fin_x, (uint32_t)y2, fin_msg, color_fg, color_bg, 2);
    pit_sleep_ms(400);

    /* 6. Hardware poweroff / reset */
    if (is_reboot) {
        acpi_reboot();
    } else {
        acpi_shutdown();
    }
}
