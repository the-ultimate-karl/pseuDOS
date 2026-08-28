#include "drivers.h"
#include "lib.h"

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(0));
}

void cpu_print_info(void) {
    uint32_t eax, ebx, ecx, edx;

    /* 1. CPU Vendor */
    char vendor[13];
    cpuid(0, &eax, (uint32_t *)&vendor[0], (uint32_t *)&vendor[8], (uint32_t *)&vendor[4]);
    vendor[12] = '\0';

    /* 2. CPU Brand / Model Name */
    char brand[49];
    memset(brand, 0, sizeof(brand));
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        cpuid(0x80000002, (uint32_t *)&brand[0], (uint32_t *)&brand[4], (uint32_t *)&brand[8], (uint32_t *)&brand[12]);
        cpuid(0x80000003, (uint32_t *)&brand[16], (uint32_t *)&brand[20], (uint32_t *)&brand[24], (uint32_t *)&brand[28]);
        cpuid(0x80000004, (uint32_t *)&brand[32], (uint32_t *)&brand[36], (uint32_t *)&brand[40], (uint32_t *)&brand[44]);
        brand[48] = '\0';
    } else {
        strcpy(brand, "generic x86_64 processor");
    }

    /* 3. Family, Model, Stepping */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t stepping = eax & 0xF;
    uint32_t model = (eax >> 4) & 0xF;
    uint32_t family = (eax >> 8) & 0xF;
    uint32_t type = (eax >> 12) & 0x3;
    if (family == 6 || family == 15) {
        model += ((eax >> 16) & 0xF) << 4;
    }
    if (family == 15) {
        family += ((eax >> 20) & 0xFF);
    }

    uint32_t max_cores = (ebx >> 16) & 0xFF;

    console_printf("cpu information:\n");
    console_printf("  vendor       : %s\n", vendor);
    console_printf("  model / name : %s\n", trim(brand));
    console_printf("  family/model : family %u, model %u, stepping %u (type %u)\n", family, model, stepping, type);
    console_printf("  max logical  : %u core(s)/thread(s)\n", max_cores > 0 ? max_cores : 1);

    /* 4. Feature flags */
    console_printf("  features     : ");
    if (edx & (1 << 0))  console_printf("fpu ");
    if (edx & (1 << 4))  console_printf("tsc ");
    if (edx & (1 << 6))  console_printf("pae ");
    if (edx & (1 << 9))  console_printf("apic ");
    if (edx & (1 << 23)) console_printf("mmx ");
    if (edx & (1 << 25)) console_printf("sse ");
    if (edx & (1 << 26)) console_printf("sse2 ");
    if (ecx & (1 << 0))  console_printf("sse3 ");
    if (ecx & (1 << 9))  console_printf("ssse3 ");
    if (ecx & (1 << 19)) console_printf("sse4.1 ");
    if (ecx & (1 << 20)) console_printf("sse4.2 ");
    if (ecx & (1 << 28)) console_printf("avx ");
    if (ecx & (1 << 30)) console_printf("rdrand ");

    /* Extended Long Mode flag */
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (edx & (1 << 29)) console_printf("lm(64-bit) ");
    if (edx & (1 << 20)) console_printf("nx/xd ");
    console_printf("\n");
}
