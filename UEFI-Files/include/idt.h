#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* 64-Bit IDT Gate Descriptor (16 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t isr_low;      /* Bits 0..15 of ISR address */
    uint16_t kernel_cs;    /* Code segment selector */
    uint8_t  ist;          /* Interrupt Stack Table index */
    uint8_t  attributes;   /* Type and attributes (0x8E = Present, 64-bit Interrupt Gate) */
    uint16_t isr_mid;      /* Bits 16..31 of ISR address */
    uint32_t isr_high;     /* Bits 32..63 of ISR address */
    uint32_t reserved;     /* Reserved (must be 0) */
} idt_entry_t;

/* IDTR Register Structure (10 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

/* Interrupt Frame passed by CPU/stub */
typedef struct {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
} interrupt_frame_t;

/* Core IDT API */
void idt_init(void);
void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags);
void pic_remap(void);
void pic_send_eoi(uint8_t irq);
void pic_unmask_irq(uint8_t irq);

#endif /* IDT_H */
