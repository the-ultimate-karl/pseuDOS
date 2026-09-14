#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include <stddef.h>

/* GDT Segment Selectors */
#define GDT_KERNEL_CS  0x08
#define GDT_KERNEL_DS  0x10
#define GDT_USER_DS    0x18
#define GDT_USER_CS    0x20
#define GDT_TSS        0x28

/* 64-bit Task State Segment (TSS) Structure */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss_entry_t;

/* 64-bit GDT Descriptor (Entry) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

/* 64-bit System Segment (TSS) Descriptor (16 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_entry_t;

/* GDTR Pointer Structure */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

/* GDT API */
void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif /* GDT_H */
