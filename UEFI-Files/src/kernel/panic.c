#include "panic.h"
#include "drivers.h"
#include "pmm.h"
#include "rtc.h"
#include "process.h"
#include "scheduler.h"
#include "pit.h"
#include "lib.h"

static void get_cpu_brand(char *brand_out, size_t brand_sz) {
    uint32_t eax, ebx, ecx, edx;
    memset(brand_out, 0, brand_sz);

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000), "c"(0));
    if (eax >= 0x80000004 && brand_sz >= 49) {
        __asm__ volatile ("cpuid" : "=a"(*(uint32_t *)&brand_out[0]),  "=b"(*(uint32_t *)&brand_out[4]),  "=c"(*(uint32_t *)&brand_out[8]),  "=d"(*(uint32_t *)&brand_out[12]) : "a"(0x80000002), "c"(0));
        __asm__ volatile ("cpuid" : "=a"(*(uint32_t *)&brand_out[16]), "=b"(*(uint32_t *)&brand_out[20]), "=c"(*(uint32_t *)&brand_out[24]), "=d"(*(uint32_t *)&brand_out[28]) : "a"(0x80000003), "c"(0));
        __asm__ volatile ("cpuid" : "=a"(*(uint32_t *)&brand_out[32]), "=b"(*(uint32_t *)&brand_out[36]), "=c"(*(uint32_t *)&brand_out[40]), "=d"(*(uint32_t *)&brand_out[44]) : "a"(0x80000004), "c"(0));
        brand_out[48] = '\0';
    } else {
        char vendor[13];
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
        *(uint32_t *)&vendor[0] = ebx;
        *(uint32_t *)&vendor[4] = edx;
        *(uint32_t *)&vendor[8] = ecx;
        vendor[12] = '\0';
        strncpy(brand_out, vendor, brand_sz - 1);
    }
}

static void format_addr_dashed(uint64_t addr, char *out, size_t out_sz) {
    uint16_t w3 = (uint16_t)((addr >> 48) & 0xFFFF);
    uint16_t w2 = (uint16_t)((addr >> 32) & 0xFFFF);
    uint16_t w1 = (uint16_t)((addr >> 16) & 0xFFFF);
    uint16_t w0 = (uint16_t)(addr & 0xFFFF);
    snprintf(out, out_sz, "0x%04X-%04X-%04X-%04X", w3, w2, w1, w0);
}

void kernel_panic(const char *reason, const panic_context_t *ctx) {
    /* 1. Disable hardware interrupts and stop scheduler */
    __asm__ volatile ("cli");
    scheduler_stop();

    /* 2. Switch console to Classic BSOD color scheme (white on dark blue) */
    console_set_colors(0xFFFFFF, 0x000084);
    console_clear();

    /* 3. Query RTC timestamp */
    rtc_datetime_t dt;
    char date_buf[32] = "2026-09-13";
    char time_buf[32] = "00:00:00";
    if (rtc_get_datetime(&dt) == 0) {
        snprintf(date_buf, sizeof(date_buf), "%04u-%02u-%02u", dt.year, dt.month, dt.day);
        snprintf(time_buf, sizeof(time_buf), "%02u:%02u:%02u", dt.hours, dt.minutes, dt.seconds);
    }

    /* 4. Query Hardware CPU info */
    char cpu_brand[64];
    get_cpu_brand(cpu_brand, sizeof(cpu_brand));
    char *clean_cpu = trim(cpu_brand);

    /* 5. Query Memory stats */
    size_t free_pages = pmm_get_free_pages();
    size_t total_pages = pmm_get_total_pages();
    size_t used_pages = (total_pages > free_pages) ? (total_pages - free_pages) : 0;
    uint64_t used_mb = (used_pages * 4096ULL) / (1024ULL * 1024ULL);
    uint64_t total_mb = (total_pages * 4096ULL) / (1024ULL * 1024ULL);

    /* 6. Extract Registers */
    uint64_t cr2 = 0;
    uint64_t cr3 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    if (ctx && ctx->cr2 != 0) cr2 = ctx->cr2;
    if (ctx && ctx->cr3 != 0) cr3 = ctx->cr3;

    uint64_t rip = ctx ? ctx->rip : 0;
    uint64_t rsp = ctx ? ctx->rsp : 0;

    char addr_cr2[32], addr_rip[32], addr_rsp[32], addr_cr3[32];
    format_addr_dashed(cr2, addr_cr2, sizeof(addr_cr2));
    format_addr_dashed(rip, addr_rip, sizeof(addr_rip));
    format_addr_dashed(rsp, addr_rsp, sizeof(addr_rsp));
    format_addr_dashed(cr3, addr_cr3, sizeof(addr_cr3));

    /* 7. Query Process context & Uptime */
    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;
    const char *pname = (curr && curr->name[0]) ? curr->name : "kernel";
    uint64_t ticks = pit_get_ticks();
    uint64_t sec = ticks / 100;
    uint64_t sub_sec = (ticks % 100) * 10;

    /* 8. Render Kernel Panic BSOD */
    console_puts("\n");
    console_puts("fatal exception: kernel panic!\n");
    console_printf("reason: %s\n", reason ? reason : "unspecified system fault");
    console_printf("date: %s\n", date_buf);
    console_printf("time: %s\n", time_buf);
    console_puts("------------------------------------\n");
    console_puts("hardware information:\n");
    console_printf("cpu: %s\n", clean_cpu && clean_cpu[0] ? clean_cpu : "x86_64 processor");
    console_printf("memory: %lu MB used / %lu MB total\n", used_mb, total_mb);
    console_printf("last accessed memory: %s\n", addr_cr2);
    console_puts("------------------------------------\n");
    console_puts("system & register context:\n");
    console_printf("active process: %s (PID %u)\n", pname, pid);
    console_printf("rip: %s\n", addr_rip);
    console_printf("rsp: %s\n", addr_rsp);
    console_printf("cr3: %s\n", addr_cr3);
    console_printf("uptime: %lu ticks (%lu.%03lu s)\n", ticks, sec, sub_sec);
    console_puts("------------------------------------\n");
    console_puts("end of fatal exception\n");
    console_puts("system halted!\n");

    /* 9. Indefinite CPU halt */
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}
