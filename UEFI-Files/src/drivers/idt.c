#include "idt.h"
#include "io.h"
#include "drivers.h"
#include "lib.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI      0x20

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

static idt_entry_t g_idt[256] __attribute__((aligned(0x10)));
static idtr_t g_idtr;

extern void keyboard_isr_handler(void);
extern void *g_isr_stub_table[256];

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64

static uint16_t get_cs(void) {
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return cs;
}

void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags) {
    uint64_t addr = (uint64_t)isr;
    g_idt[vector].isr_low = (uint16_t)(addr & 0xFFFF);
    g_idt[vector].kernel_cs = get_cs();
    g_idt[vector].ist = 0;
    g_idt[vector].attributes = flags;
    g_idt[vector].isr_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vector].isr_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved = 0;
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = (uint8_t)(inb(port) & ~(1 << irq));
    outb(port, value);
}

void pic_remap(void) {
    /* Save masks */
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);
    (void)a1; (void)a2;

    /* Start initialization sequence in cascade mode */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* Master PIC vector offset 32 (0x20) */
    outb(PIC1_DATA, 0x20);
    io_wait();
    /* Slave PIC vector offset 40 (0x28) */
    outb(PIC2_DATA, 0x28);
    io_wait();

    /* Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100) */
    outb(PIC1_DATA, 0x04);
    io_wait();
    /* Tell Slave PIC its cascade identity (0000 0010) */
    outb(PIC2_DATA, 0x02);
    io_wait();

    /* Set 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask all IRQs except IRQ 1 (Keyboard).
       Bit 0: IRQ 0 Timer (masked = 1)
       Bit 1: IRQ 1 Keyboard (UNMASKED = 0)
       Bit 2: IRQ 2 Cascade (masked = 1)
       Bits 3..7: masked = 1
    */
    outb(PIC1_DATA, 0xFD); /* 0b11111101: only IRQ 1 unmasked */
    outb(PIC2_DATA, 0xFF); /* 0b11111111: all slave IRQs masked */
}

void isr_dispatch(uint64_t vector, uint64_t error_code, interrupt_frame_t *frame, registers_t *regs) {
    (void)regs;

    /* 1. Spurious IRQ 7 on Master PIC (Vector 39 = 32 + 7) */
    if (vector == 39) {
        outb(PIC1_COMMAND, 0x0B); /* Read In-Service Register (ISR) */
        uint8_t isr = inb(PIC1_COMMAND);
        if (!(isr & 0x80)) {
            /* Spurious IRQ: per 8259A spec, do NOT send EOI to PIC */
            return;
        }
        pic_send_eoi(7);
        return;
    }

    /* 2. Spurious IRQ 15 on Slave PIC (Vector 47 = 40 + 7) */
    if (vector == 47) {
        outb(PIC2_COMMAND, 0x0B); /* Read In-Service Register (ISR) */
        uint8_t isr = inb(PIC2_COMMAND);
        if (!(isr & 0x80)) {
            /* Spurious IRQ 15: send EOI only to Master PIC */
            outb(PIC1_COMMAND, PIC_EOI);
            return;
        }
        pic_send_eoi(15);
        return;
    }

    /* 3. Keyboard IRQ 1 (Vector 33) */
    if (vector == 33) {
        keyboard_isr_handler();
        pic_send_eoi(1);
        return;
    }

    /* 4. Timer IRQ 0 (Vector 32) */
    if (vector == 32) {
        pic_send_eoi(0);
        return;
    }

    /* 5. PS/2 Mouse IRQ 12 (Vector 44 = 40 + 4) */
    if (vector == 44) {
        /* Drain byte from data port to clear pending mouse packet */
        if (inb(PS2_STATUS_PORT) & 0x01) {
            (void)inb(PS2_DATA_PORT);
        }
        pic_send_eoi(12);
        return;
    }

    /* 6. Any other PIC hardware IRQ (Vectors 34..46) */
    if (vector >= 32 && vector <= 47) {
        pic_send_eoi((uint8_t)(vector - 32));
        return;
    }

    /* 7. Local APIC Spurious Interrupt (Vector 255) */
    if (vector == 255) {
        return;
    }

    /* 8. CPU Exception (Vectors 0..31) or Fatal Unhandled Interrupt */
    const char *name = "UNKNOWN_EXCEPTION";
    switch (vector) {
        case 0:  name = "#DE (Divide Error)"; break;
        case 1:  name = "#DB (Debug Exception)"; break;
        case 2:  name = "#NMI (Non-Maskable Interrupt)"; break;
        case 3:  name = "#BP (Breakpoint)"; break;
        case 4:  name = "#OF (Overflow)"; break;
        case 5:  name = "#BR (BOUND Range Exceeded)"; break;
        case 6:  name = "#UD (Invalid Opcode)"; break;
        case 7:  name = "#NM (Device Not Available)"; break;
        case 8:  name = "#DF (Double Fault)"; break;
        case 9:  name = "Coprocessor Segment Overrun"; break;
        case 10: name = "#TS (Invalid TSS)"; break;
        case 11: name = "#NP (Segment Not Present)"; break;
        case 12: name = "#SS (Stack-Segment Fault)"; break;
        case 13: name = "#GP (General Protection Fault)"; break;
        case 14: name = "#PF (Page Fault)"; break;
        case 16: name = "#MF (x87 FPU Floating-Point Error)"; break;
        case 17: name = "#AC (Alignment Check)"; break;
        case 18: name = "#MC (Machine Check)"; break;
        case 19: name = "#XM (SIMD Floating-Point Exception)"; break;
        case 20: name = "#VE (Virtualization Exception)"; break;
        case 21: name = "#CP (Control Protection Exception)"; break;
        default: break;
    }

    uint64_t cr2 = 0;
    if (vector == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    }

    console_printf("\n============================================================\n");
    console_printf("[CPU EXCEPTION] Unhandled CPU Fault!\n");
    console_printf("  Vector:     0x%02lX (%s)\n", vector, name);
    console_printf("  Error Code: 0x%016lX\n", error_code);
    if (frame) {
        console_printf("  RIP:        0x%016lX\n", frame->ip);
        console_printf("  CS:         0x%04lX\n", frame->cs);
        console_printf("  RFLAGS:     0x%016lX\n", frame->flags);
        console_printf("  RSP:        0x%016lX\n", frame->sp);
        console_printf("  SS:         0x%04lX\n", frame->ss);
    }
    if (vector == 14) {
        console_printf("  Fault Addr: 0x%016lX (CR2)\n", cr2);
    }
    console_printf("============================================================\n");

    __asm__ volatile ("cli; hlt");
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));

    /* Initialize all 256 entries to their corresponding dedicated ISR stubs */
    for (int i = 0; i < 256; i++) {
        idt_set_descriptor((uint8_t)i, g_isr_stub_table[i], 0x8E);
    }

    /* Load IDTR */
    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (uint64_t)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idtr));

    /* Remap 8259 PIC and enable interrupts */
    pic_remap();
    __asm__ volatile ("sti");
}
