.section .text
.code64

.global syscall_entry
.extern syscall_dispatch
.extern g_current_kernel_rsp
.extern g_syscall_user_rsp
.extern g_current_process_privilege

syscall_entry:
    # 1. Save calling RSP
    movq %rsp, g_syscall_user_rsp(%rip)

    # 2. Push interrupt frame for iretq
    pushq $0x10                          # SS: Kernel Data (0x10)
    pushq g_syscall_user_rsp(%rip)       # RSP: Caller stack pointer
    pushq %r11                           # RFLAGS: Flags saved by syscall
    pushq $0x08                          # CS: Kernel Code (0x08)
    pushq %rcx                           # RIP: Instruction pointer saved by syscall

.Lsyscall_frame_done:
    # 3. Push dummy error code and vector (0x80)
    pushq $0
    pushq $0x80

    # 4. Push general-purpose registers (matching registers_t layout)
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    cld

    # 5. Marshal parameters into Microsoft x64 C calling convention:
    #    RCX = num (RAX)
    #    RDX = a1  (RDI)
    #    R8  = a2  (RSI)
    #    R9  = a3  (RDX)
    #    [RSP+32] = a4 (R10)
    #    [RSP+40] = a5 (R8)
    subq $48, %rsp
    movq %r10, 32(%rsp)      # a4 -> 5th argument
    movq %r8,  40(%rsp)      # a5 -> 6th argument

    movq %rdx, %r9           # a3 -> R9 (4th argument)
    movq %rsi, %r8           # a2 -> R8 (3rd argument)
    movq %rdi, %rdx          # a1 -> RDX (2nd argument)
    movq %rax, %rcx          # num -> RCX (1st argument)

    call syscall_dispatch

    addq $48, %rsp

    # Store return value from RAX into saved RAX slot at (%rsp)
    movq %rax, (%rsp)

    # 6. Restore general-purpose registers
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    # 7. Skip dummy vector and error code
    addq $16, %rsp

    # 8. Atomically resume userspace execution with updated registers
    iretq
