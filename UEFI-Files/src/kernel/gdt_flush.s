.section .text
.code64

.global gdt_flush
# void gdt_flush(const gdtr_t *gdtr, uint64_t cs, uint64_t ds, uint64_t tss_sel);
# MinGW x86_64 ABI:
#   rcx = gdtr pointer
#   rdx = kernel CS (0x08)
#   r8  = kernel DS (0x10)
#   r9  = TSS selector (0x28)
gdt_flush:
    lgdt (%rcx)

    # Reload data segment registers
    movw %r8w, %ds
    movw %r8w, %es
    movw %r8w, %ss
    movw %r8w, %fs
    movw %r8w, %gs

    # Far return to reload CS
    pushq %rdx
    leaq .reload_cs(%rip), %rax
    pushq %rax
    lretq

.reload_cs:
    # Load Task State Segment selector
    ltr %r9w
    ret
