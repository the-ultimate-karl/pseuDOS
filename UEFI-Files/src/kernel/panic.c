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

static inline uint64_t read_rflags(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f));
    return f;
}

static void dump_registers(const panic_context_t *ctx) {
    char s_rax[32], s_rbx[32], s_rcx[32], s_rdx[32];
    char s_rsi[32], s_rdi[32], s_rbp[32], s_rsp[32];
    char s_r8[32], s_r9[32], s_r10[32], s_r11[32];
    char s_r12[32], s_r13[32], s_r14[32], s_r15[32];
    char s_rip[32], s_rflags[32];
    char s_cr0[32], s_cr2[32], s_cr3[32], s_cr4[32];

    uint64_t cr0 = 0, cr2 = 0, cr3 = 0, cr4 = 0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if (ctx && ctx->cr2 != 0) cr2 = ctx->cr2;
    if (ctx && ctx->cr3 != 0) cr3 = ctx->cr3;

    uint64_t rax = ctx ? ctx->rax : 0;
    uint64_t rbx = ctx ? ctx->rbx : 0;
    uint64_t rcx = ctx ? ctx->rcx : 0;
    uint64_t rdx = ctx ? ctx->rdx : 0;
    uint64_t rsi = ctx ? ctx->rsi : 0;
    uint64_t rdi = ctx ? ctx->rdi : 0;
    uint64_t rbp = ctx ? ctx->rbp : 0;
    uint64_t rsp = ctx ? ctx->rsp : 0;
    uint64_t r8  = ctx ? ctx->r8 : 0;
    uint64_t r9  = ctx ? ctx->r9 : 0;
    uint64_t r10 = ctx ? ctx->r10 : 0;
    uint64_t r11 = ctx ? ctx->r11 : 0;
    uint64_t r12 = ctx ? ctx->r12 : 0;
    uint64_t r13 = ctx ? ctx->r13 : 0;
    uint64_t r14 = ctx ? ctx->r14 : 0;
    uint64_t r15 = ctx ? ctx->r15 : 0;
    uint64_t rip = ctx ? ctx->rip : 0;
    uint64_t rflags = ctx ? ctx->rflags : 0;
    uint64_t cs = ctx ? ctx->cs : 0;
    uint64_t ss = ctx ? ctx->ss : 0;

    if (rsp == 0) {
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    }
    if (rip == 0) {
        rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
    }
    if (rflags == 0) {
        rflags = read_rflags();
    }
    if (cs == 0) {
        uint16_t seg;
        __asm__ volatile ("mov %%cs, %0" : "=r"(seg));
        cs = seg;
    }
    if (ss == 0) {
        uint16_t seg;
        __asm__ volatile ("mov %%ss, %0" : "=r"(seg));
        ss = seg;
    }

    format_addr_dashed(rax, s_rax, sizeof(s_rax));
    format_addr_dashed(rbx, s_rbx, sizeof(s_rbx));
    format_addr_dashed(rcx, s_rcx, sizeof(s_rcx));
    format_addr_dashed(rdx, s_rdx, sizeof(s_rdx));
    format_addr_dashed(rsi, s_rsi, sizeof(s_rsi));
    format_addr_dashed(rdi, s_rdi, sizeof(s_rdi));
    format_addr_dashed(rbp, s_rbp, sizeof(s_rbp));
    format_addr_dashed(rsp, s_rsp, sizeof(s_rsp));
    format_addr_dashed(r8,  s_r8,  sizeof(s_r8));
    format_addr_dashed(r9,  s_r9,  sizeof(s_r9));
    format_addr_dashed(r10, s_r10, sizeof(s_r10));
    format_addr_dashed(r11, s_r11, sizeof(s_r11));
    format_addr_dashed(r12, s_r12, sizeof(s_r12));
    format_addr_dashed(r13, s_r13, sizeof(s_r13));
    format_addr_dashed(r14, s_r14, sizeof(s_r14));
    format_addr_dashed(r15, s_r15, sizeof(s_r15));
    format_addr_dashed(rip, s_rip, sizeof(s_rip));
    format_addr_dashed(rflags, s_rflags, sizeof(s_rflags));
    format_addr_dashed(cr0, s_cr0, sizeof(s_cr0));
    format_addr_dashed(cr2, s_cr2, sizeof(s_cr2));
    format_addr_dashed(cr3, s_cr3, sizeof(s_cr3));
    format_addr_dashed(cr4, s_cr4, sizeof(s_cr4));

    console_puts("\n--- register dump ---\n");
    console_printf("rax: %s  rbx: %s\n", s_rax, s_rbx);
    console_printf("rcx: %s  rdx: %s\n", s_rcx, s_rdx);
    console_printf("rsi: %s  rdi: %s\n", s_rsi, s_rdi);
    console_printf("rbp: %s  rsp: %s\n", s_rbp, s_rsp);
    console_printf("r8:  %s  r9:  %s\n", s_r8, s_r9);
    console_printf("r10: %s  r11: %s\n", s_r10, s_r11);
    console_printf("r12: %s  r13: %s\n", s_r12, s_r13);
    console_printf("r14: %s  r15: %s\n", s_r14, s_r15);
    console_printf("rip: %s  rflags: %s\n", s_rip, s_rflags);
    console_printf("cs:  0x%04X                ss:  0x%04X\n", (uint32_t)cs, (uint32_t)ss);
    console_printf("cr0: %s  cr2: %s\n", s_cr0, s_cr2);
    console_printf("cr3: %s  cr4: %s\n", s_cr3, s_cr4);
    console_puts("---------------------\n");
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
    if (rsp == 0) {
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    }
    if (rip == 0) {
        rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
    }

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
    console_puts("action: press (r) to restart, (d) to dump register states, or (s) to shutdown\n");

    /* 9. Interactive recovery loop */
    while (1) {
        char c = keyboard_getchar();
        if (c == 'r' || c == 'R') {
            console_puts("\nrestarting system...\n");
            acpi_reboot();
            while (1) {
                __asm__ volatile ("cli; hlt");
            }
        } else if (c == 's' || c == 'S') {
            console_puts("\nshutting down system...\n");
            acpi_shutdown();
            while (1) {
                __asm__ volatile ("cli; hlt");
            }
        } else if (c == 'd' || c == 'D') {
            dump_registers(ctx);
            console_puts("action: press (r) to restart, (d) to dump register states, or (s) to shutdown\n");
        }
    }
}
