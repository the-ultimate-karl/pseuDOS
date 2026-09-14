#include "gdt.h"
#include "lib.h"

#define GDT_ENTRY_COUNT 7

static gdt_entry_t g_gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static gdtr_t g_gdtr;
static tss_entry_t g_tss __attribute__((aligned(16)));

/* Dedicated 16KB interrupt stack for Ring 3 -> Ring 0 transitions */
static uint8_t g_kernel_interrupt_stack[16384] __attribute__((aligned(16)));

extern void gdt_flush(const gdtr_t *gdtr, uint64_t cs, uint64_t ds, uint64_t tss_sel);

static void set_gdt_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    g_gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    g_gdt[index].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    g_gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    g_gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    g_gdt[index].granularity |= (gran & 0xF0);
    g_gdt[index].access      = access;
}

static void set_gdt_tss(int index, uint64_t base, uint32_t limit) {
    gdt_tss_entry_t *tss_desc = (gdt_tss_entry_t *)&g_gdt[index];

    tss_desc->limit_low   = (uint16_t)(limit & 0xFFFF);
    tss_desc->base_low    = (uint16_t)(base & 0xFFFF);
    tss_desc->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    tss_desc->access      = 0x89; /* Present, DPL 0, 64-bit TSS (Available) */
    tss_desc->granularity = (uint8_t)(((limit >> 16) & 0x0F));
    tss_desc->base_high   = (uint8_t)((base >> 24) & 0xFF);
    tss_desc->base_upper  = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    tss_desc->reserved    = 0;
}

void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

void gdt_init(void) {
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    /* 0x00: Null Descriptor */
    set_gdt_entry(0, 0, 0, 0, 0);

    /* 0x08: Kernel Code (Ring 0, 64-bit long mode, execute/read) */
    set_gdt_entry(1, 0, 0, 0x9A, 0x20);

    /* 0x10: Kernel Data (Ring 0, read/write) */
    set_gdt_entry(2, 0, 0, 0x92, 0x00);

    /* 0x18: User Data (Ring 3, read/write) */
    set_gdt_entry(3, 0, 0, 0xF2, 0x00);

    /* 0x20: User Code (Ring 3, 64-bit long mode, execute/read) */
    set_gdt_entry(4, 0, 0, 0xFA, 0x20);

    /* Setup TSS */
    g_tss.rsp0 = (uint64_t)(uintptr_t)&g_kernel_interrupt_stack[sizeof(g_kernel_interrupt_stack)];
    g_tss.iopb_offset = (uint16_t)sizeof(tss_entry_t);

    /* 0x28: TSS Descriptor (16 bytes, occupies index 5 and 6) */
    set_gdt_tss(5, (uint64_t)(uintptr_t)&g_tss, sizeof(tss_entry_t) - 1);

    /* Configure GDTR */
    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base  = (uint64_t)(uintptr_t)&g_gdt;

    /* Reload GDT, segment registers, and load TSS */
    gdt_flush(&g_gdtr, GDT_KERNEL_CS, GDT_KERNEL_DS, GDT_TSS);
}
