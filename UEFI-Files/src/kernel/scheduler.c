#include "scheduler.h"
#include "process.h"
#include "gdt.h"
#include "idt.h"
#include "pit.h"
#include "klog.h"
#include "drivers.h"

extern uint64_t g_current_kernel_rsp;

static volatile int g_scheduler_enabled = 0;

int scheduler_is_enabled(void) {
    return g_scheduler_enabled;
}

void scheduler_init(void) {
    g_scheduler_enabled = 0;
    klog_info("Preemptive scheduler initialized (Quantum=%d ticks)", DEFAULT_TIME_SLICE);
}

void scheduler_start(void) {
    process_t *curr = process_get_current();
    if (curr) {
        g_current_kernel_rsp = curr->kernel_stack;
        tss_set_rsp0(curr->kernel_stack);
    }
    g_scheduler_enabled = 1;
    pic_unmask_irq(0); /* Unmask PIT Timer IRQ 0 on 8259 PIC */
    klog_info("Scheduler started with PIT IRQ 0 unmasked (~100 Hz)");
}

void scheduler_stop(void) {
    g_scheduler_enabled = 0;
    pic_mask_irq(0);
}

void scheduler_tick(interrupt_frame_t *frame, registers_t *regs) {
    pit_tick();

    extern void syscall_check_scheduled_shutdown(void);
    syscall_check_scheduled_shutdown();

    if (!g_scheduler_enabled) {
        return;
    }

    process_t *curr = process_get_current();
    if (!curr) {
        return;
    }

    curr->total_ticks++;

    if (curr->time_slice > 0) {
        curr->time_slice--;
    }

    if (curr->time_slice == 0) {
        curr->time_slice = DEFAULT_TIME_SLICE;
        scheduler_schedule(frame, regs);
    }
}

void scheduler_schedule(interrupt_frame_t *frame, registers_t *regs) {
    if (!g_scheduler_enabled) {
        return;
    }

    process_t *prev = process_get_current();

    /* 1. Save current context if process was actively running */
    if (prev && prev->state == PROCESS_STATE_RUNNING) {
        prev->context.rax = regs->rax;
        prev->context.rbx = regs->rbx;
        prev->context.rcx = regs->rcx;
        prev->context.rdx = regs->rdx;
        prev->context.rsi = regs->rsi;
        prev->context.rdi = regs->rdi;
        prev->context.rbp = regs->rbp;
        prev->context.r8  = regs->r8;
        prev->context.r9  = regs->r9;
        prev->context.r10 = regs->r10;
        prev->context.r11 = regs->r11;
        prev->context.r12 = regs->r12;
        prev->context.r13 = regs->r13;
        prev->context.r14 = regs->r14;
        prev->context.r15 = regs->r15;

        prev->context.rip    = frame->ip;
        prev->context.cs     = frame->cs;
        prev->context.rflags = frame->flags;
        prev->context.rsp    = frame->sp;
        prev->context.ss     = frame->ss;

        prev->state = PROCESS_STATE_READY;
    }

    /* 2. Find next READY process in round-robin order */
    process_t *next = NULL;
    uint32_t current_pid = prev ? prev->pid : 0;

    for (uint32_t i = 1; i <= MAX_PROCESSES; i++) {
        uint32_t check_pid = (current_pid + i) % MAX_PROCESSES;
        process_t *p = process_get_by_pid(check_pid);
        if (p && p->state == PROCESS_STATE_READY) {
            next = p;
            break;
        }
    }

    /* If no other ready process, fallback to prev if still ready, or PID 0 */
    if (!next) {
        if (prev && prev->state == PROCESS_STATE_READY) {
            next = prev;
        } else {
            next = process_get_by_pid(0);
        }
    }

    if (!next) {
        return;
    }

    if (prev == next) {
        next->state = PROCESS_STATE_RUNNING;
        return;
    }

    next->state = PROCESS_STATE_RUNNING;
    process_set_current(next);

    /* Update kernel stack in TSS and syscall handler */
    if (next->kernel_stack) {
        g_current_kernel_rsp = next->kernel_stack;
        tss_set_rsp0(next->kernel_stack);
    }

    /* Switch address space (CR3) if changed */
    if (next->cr3) {
        uint64_t current_cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3));
        if (current_cr3 != next->cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
        }
    }

    /* 3. Restore next process context into registers and interrupt frame */
    regs->rax = next->context.rax;
    regs->rbx = next->context.rbx;
    regs->rcx = next->context.rcx;
    regs->rdx = next->context.rdx;
    regs->rsi = next->context.rsi;
    regs->rdi = next->context.rdi;
    regs->rbp = next->context.rbp;
    regs->r8  = next->context.r8;
    regs->r9  = next->context.r9;
    regs->r10 = next->context.r10;
    regs->r11 = next->context.r11;
    regs->r12 = next->context.r12;
    regs->r13 = next->context.r13;
    regs->r14 = next->context.r14;
    regs->r15 = next->context.r15;

    frame->ip    = next->context.rip;
    frame->cs    = next->context.cs;
    frame->flags = next->context.rflags;
    frame->sp    = next->context.rsp;
    frame->ss    = next->context.ss;
}

void scheduler_yield(void) {
    if (!g_scheduler_enabled) return;
    __asm__ volatile ("int $48");
}
