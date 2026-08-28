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
        strcpy(brand, "Generic x86_64 Processor");
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

    console_printf("CPU Information:\n");
    console_printf("  Vendor       : %s\n", vendor);
    console_printf("  Model / Name : %s\n", trim(brand));
    console_printf("  Family/Model : Family %u, Model %u, Stepping %u (Type %u)\n", family, model, stepping, type);
    console_printf("  Max Logical  : %u core(s)/thread(s)\n", max_cores > 0 ? max_cores : 1);

    /* 4. Feature flags */
    console_printf("  Features     : ");
    if (edx & (1 << 0))  console_printf("FPU ");
    if (edx & (1 << 4))  console_printf("TSC ");
    if (edx & (1 << 6))  console_printf("PAE ");
    if (edx & (1 << 9))  console_printf("APIC ");
    if (edx & (1 << 23)) console_printf("MMX ");
    if (edx & (1 << 25)) console_printf("SSE ");
    if (edx & (1 << 26)) console_printf("SSE2 ");
    if (ecx & (1 << 0))  console_printf("SSE3 ");
    if (ecx & (1 << 9))  console_printf("SSSE3 ");
    if (ecx & (1 << 19)) console_printf("SSE4.1 ");
    if (ecx & (1 << 20)) console_printf("SSE4.2 ");
    if (ecx & (1 << 28)) console_printf("AVX ");
    if (ecx & (1 << 30)) console_printf("RDRAND ");

    /* Extended Long Mode flag */
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (edx & (1 << 29)) console_printf("LM(64-bit) ");
    if (edx & (1 << 20)) console_printf("NX/XD ");
    console_printf("\n");
}
