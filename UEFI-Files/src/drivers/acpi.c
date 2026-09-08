#include "drivers.h"
#include "storage.h"
#include "io.h"
#include "bootinfo.h"
#include "lib.h"

extern const BootInfo *g_boot_info_global;

typedef struct {
    char     signature[8];    /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;        /* 0 for ACPI 1.0, 2 for ACPI 2.0+ */
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) AcpiRsdp;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) AcpiHeader;

typedef struct {
    AcpiHeader header;
    uint32_t   firmware_ctrl;
    uint32_t   dsdt;
    uint8_t    reserved;
    uint8_t    preferred_pm_profile;
    uint16_t   sci_int;
    uint32_t   smi_cmd;
    uint8_t    acpi_enable;
    uint8_t    acpi_disable;
    uint8_t    s4bios_req;
    uint8_t    pstate_cnt;
    uint32_t   pm1a_evt_blk;
    uint32_t   pm1b_evt_blk;
    uint32_t   pm1a_cnt_blk;
    uint32_t   pm1b_cnt_blk;
    uint32_t   pm2_cnt_blk;
    uint32_t   pm_tmr_blk;
    uint32_t   gpe0_blk;
    uint32_t   gpe1_blk;
    uint8_t    pm1_evt_len;
    uint8_t    pm1_cnt_len;
    uint8_t    pm2_cnt_len;
    uint8_t    pm_tmr_len;
    uint8_t    gpe0_blk_len;
    uint8_t    gpe1_blk_len;
    uint8_t    gpe1_base;
    uint8_t    cst_cnt;
    uint16_t   c2_latency;
    uint16_t   c3_latency;
    uint16_t   flush_size;
    uint16_t   flush_stride;
    uint8_t    duty_offset;
    uint8_t    duty_width;
    uint8_t    day_alarm;
    uint8_t    month_alarm;
    uint8_t    century;
    uint16_t   boot_arch_flags;
    uint8_t    reserved2;
    uint32_t   flags;
    /* Reset Register GAS */
    uint8_t    reset_reg[12];
    uint8_t    reset_value;
    uint8_t    reserved3[3];
    uint64_t   x_firmware_ctrl;
    uint64_t   x_dsdt;
    uint8_t    x_pm1a_evt_blk[12];
    uint8_t    x_pm1b_evt_blk[12];
    uint8_t    x_pm1a_cnt_blk[12];
    uint8_t    x_pm1b_cnt_blk[12];
} __attribute__((packed)) AcpiFadt;

static int parse_s5_from_aml(const uint8_t *dsdt_aml, uint32_t length, uint16_t *out_slp_typa, uint16_t *out_slp_typb) {
    if (!dsdt_aml || length < 8) return -1;

    for (uint32_t i = 0; i < length - 8; i++) {
        /* Look for "_S5_" signature in DSDT AML */
        if (dsdt_aml[i] == '_' && dsdt_aml[i + 1] == 'S' && dsdt_aml[i + 2] == '5' && dsdt_aml[i + 3] == '_') {
            uint32_t p = i + 4;
            if (p < length && dsdt_aml[p] == 0x12) {
                p++; /* Skip PackageOp */
            }
            /* Skip package length encoding */
            if (p < length) {
                uint8_t pkg_lead = dsdt_aml[p];
                uint8_t byte_count = (pkg_lead >> 6) & 3;
                p += (byte_count + 1);
            }
            if (p < length) {
                p++; /* Skip NumElements */
            }
            /* Element 0: SLP_TYPa */
            if (p < length) {
                if (dsdt_aml[p] == 0x0A) p++; /* BytePrefix */
                *out_slp_typa = dsdt_aml[p++];
            }
            /* Element 1: SLP_TYPb */
            if (p < length) {
                if (dsdt_aml[p] == 0x0A) p++; /* BytePrefix */
                *out_slp_typb = dsdt_aml[p++];
            }
            return 0;
        }
    }
    return -1;
}

static AcpiFadt *find_fadt(void) {
    if (!g_boot_info_global || g_boot_info_global->acpi_rsdp_address == 0) return NULL;

    const AcpiRsdp *rsdp = (const AcpiRsdp *)(uintptr_t)g_boot_info_global->acpi_rsdp_address;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) return NULL;

    /* 1. Try XSDT (64-bit pointers, ACPI 2.0+) */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        const AcpiHeader *xsdt = (const AcpiHeader *)(uintptr_t)rsdp->xsdt_address;
        if (memcmp(xsdt->signature, "XSDT", 4) == 0 && xsdt->length >= sizeof(AcpiHeader)) {
            uint32_t entries = (xsdt->length - sizeof(AcpiHeader)) / sizeof(uint64_t);
            const uint64_t *table_ptrs = (const uint64_t *)((const uint8_t *)xsdt + sizeof(AcpiHeader));
            for (uint32_t i = 0; i < entries; i++) {
                const AcpiHeader *hdr = (const AcpiHeader *)(uintptr_t)table_ptrs[i];
                if (hdr && memcmp(hdr->signature, "FACP", 4) == 0) {
                    return (AcpiFadt *)hdr;
                }
            }
        }
    }

    /* 2. Try RSDT (32-bit pointers, ACPI 1.0+) */
    if (rsdp->rsdt_address != 0) {
        const AcpiHeader *rsdt = (const AcpiHeader *)(uintptr_t)rsdp->rsdt_address;
        if (memcmp(rsdt->signature, "RSDT", 4) == 0 && rsdt->length >= sizeof(AcpiHeader)) {
            uint32_t entries = (rsdt->length - sizeof(AcpiHeader)) / sizeof(uint32_t);
            const uint32_t *table_ptrs = (const uint32_t *)((const uint8_t *)rsdt + sizeof(AcpiHeader));
            for (uint32_t i = 0; i < entries; i++) {
                const AcpiHeader *hdr = (const AcpiHeader *)(uintptr_t)table_ptrs[i];
                if (hdr && memcmp(hdr->signature, "FACP", 4) == 0) {
                    return (AcpiFadt *)hdr;
                }
            }
        }
    }

    return NULL;
}

void acpi_reboot(void) {
    console_puts("rebooting system...\n");
    storage_flush_all();

    /* 1. Try FADT ACPI Reset Register if supported */
    AcpiFadt *fadt = find_fadt();
    if (fadt && (fadt->flags & (1 << 10))) { /* RESET_REG_SUP bit 10 */
        uint8_t addr_space = fadt->reset_reg[0];
        uint64_t addr = *(uint64_t *)&fadt->reset_reg[4];
        if (addr_space == 1 && addr != 0) { /* System I/O space */
            outb((uint16_t)addr, fadt->reset_value);
            io_wait();
        }
    }

    /* 2. 8042 PS/2 Keyboard Controller Pulse Reset */
    uint8_t temp;
    int timeout = 10000;
    do {
        temp = inb(0x64);
        if (temp & 1) inb(0x60);
    } while ((temp & 2) && --timeout > 0);
    outb(0x64, 0xFE);
    io_wait();

    /* 3. PCI Reset Register (Port 0xCF9) */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();

    /* 4. Triple Fault Force Reboot */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } null_idtr = { 0, 0 };

    __asm__ volatile ("lidt %0; int3" : : "m"(null_idtr));

    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}

void acpi_shutdown(void) {
    console_puts("shutting down system via ACPI...\n");
    storage_shutdown_all();

    /* 1. Parse hardware ACPI Tables (RSDP -> FADT -> DSDT -> _S5) */
    AcpiFadt *fadt = find_fadt();
    if (fadt) {
        uint32_t pm1a_cnt = fadt->pm1a_cnt_blk;
        uint32_t pm1b_cnt = fadt->pm1b_cnt_blk;

        /* Check 64-bit GAS addresses if available */
        if (fadt->header.length >= 172) {
            uint8_t space_a = fadt->x_pm1a_cnt_blk[0];
            uint64_t addr_a = *(uint64_t *)&fadt->x_pm1a_cnt_blk[4];
            if (space_a == 1 && addr_a != 0) pm1a_cnt = (uint32_t)addr_a;

            uint8_t space_b = fadt->x_pm1b_cnt_blk[0];
            uint64_t addr_b = *(uint64_t *)&fadt->x_pm1b_cnt_blk[4];
            if (space_b == 1 && addr_b != 0) pm1b_cnt = (uint32_t)addr_b;
        }

        /* Check if ACPI mode needs enabling */
        if (fadt->smi_cmd != 0 && fadt->acpi_enable != 0) {
            if (pm1a_cnt != 0 && (inw((uint16_t)pm1a_cnt) & 1) == 0) {
                outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
                for (volatile int d = 0; d < 100000; d++);
            }
        }

        /* Locate DSDT Table */
        uintptr_t dsdt_phys = (uintptr_t)fadt->dsdt;
        if (fadt->header.length >= 148 && fadt->x_dsdt != 0) {
            dsdt_phys = (uintptr_t)fadt->x_dsdt;
        }

        uint16_t slp_typa = 7; /* Standard fallback sleep type */
        uint16_t slp_typb = 7;

        if (dsdt_phys != 0) {
            const AcpiHeader *dsdt_hdr = (const AcpiHeader *)dsdt_phys;
            if (memcmp(dsdt_hdr->signature, "DSDT", 4) == 0 && dsdt_hdr->length > sizeof(AcpiHeader)) {
                parse_s5_from_aml((const uint8_t *)dsdt_hdr + sizeof(AcpiHeader),
                                  dsdt_hdr->length - sizeof(AcpiHeader),
                                  &slp_typa, &slp_typb);
            }
        }

        /* Execute ACPI S5 Sleep State Transition */
        if (pm1a_cnt != 0) {
            uint16_t pm1a_val = (uint16_t)((slp_typa << 10) | (1 << 13)); /* SLP_EN = bit 13 */
            outw((uint16_t)pm1a_cnt, pm1a_val);
            io_wait();
        }
        if (pm1b_cnt != 0) {
            uint16_t pm1b_val = (uint16_t)((slp_typb << 10) | (1 << 13));
            outw((uint16_t)pm1b_cnt, pm1b_val);
            io_wait();
        }
    }

    /* 2. VMware Backdoor Poweroff */
    /* Port 0x5658 ("VX") command 10 (GETVERSION) / BDOOR_CMD */
    __asm__ volatile (
        "mov $0x564D5868, %%eax\n" /* Magic 'VMXh' */
        "mov $0, %%ebx\n"
        "mov $10, %%ecx\n"          /* BDOOR_CMD_GETVERSION */
        "mov $0x5658, %%dx\n"
        "inl (%%dx), %%eax\n"
        : : : "eax", "ebx", "ecx", "edx"
    );

    /* 3. Comprehensive Hypervisor Port Poweroff Fallbacks */
    /* QEMU / KVM Q35 & i440fx modern ACPI PM1a_CNT */
    outw(0x0604, 0x2000);
    io_wait();
    outw(0x0604, 0x3400);
    io_wait();

    /* VirtualBox ACPI PM1a_CNT */
    outw(0x4004, 0x3400);
    io_wait();

    /* QEMU older i440fx / Bochs */
    outw(0xB004, 0x2000);
    io_wait();

    /* Cloud Hypervisor / Firecracker */
    outw(0x0600, 0x0034);
    io_wait();

    /* Loop and halt CPU permanently */
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}
