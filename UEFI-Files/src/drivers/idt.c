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
extern void isr_exception_handler(uint64_t vector, uint64_t err_code);

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

    /* Mask all IRQs except IRQ 1 (Keyboard) and IRQ 2 (Cascade to slave) */
    outb(PIC1_DATA, 0b11111001); /* Enable IRQ 1 and IRQ 2 */
    outb(PIC2_DATA, 0b11111111); /* Mask all slave IRQs */
}

/* Default generic exception stub handler */
__attribute__((interrupt)) void generic_exception_stub(interrupt_frame_t *frame) {
    (void)frame;
    console_puts("\n[cpu exception] unhandled cpu fault or interrupt!\n");
    __asm__ volatile ("cli; hlt");
}

/* Keyboard IRQ 1 ISR handler with attribute interrupt */
__attribute__((interrupt)) void keyboard_irq_stub(interrupt_frame_t *frame) {
    (void)frame;
    keyboard_isr_handler();
    pic_send_eoi(1);
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));

    /* Initialize all 256 entries to generic exception stub */
    for (int i = 0; i < 256; i++) {
        idt_set_descriptor((uint8_t)i, (void *)generic_exception_stub, 0x8E);
    }

    /* Register IRQ 1 (Vector 32 + 1 = 33) for PS/2 Keyboard */
    idt_set_descriptor(33, (void *)keyboard_irq_stub, 0x8E);

    /* Load IDTR */
    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (uint64_t)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idtr));

    /* Remap 8259 PIC and enable interrupts */
    pic_remap();
    __asm__ volatile ("sti");
}
