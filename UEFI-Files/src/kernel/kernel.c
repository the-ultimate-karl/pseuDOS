#include "kernel.h"
#include "bootinfo.h"
#include "gdt.h"
#include "pmm.h"
#include "vmm.h"
#include "idt.h"
#include "rtc.h"
#include "klog.h"
#include "drivers.h"
#include "storage.h"
#include "fs.h"
#include "lib.h"
#include "io.h"
#include "pit.h"
#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "pe_loader.h"

const BootInfo *g_boot_info_global = NULL;

void kernel_main(BootInfo *boot_info) {
    if (!boot_info || boot_info->magic != BOOTINFO_MAGIC) {
        /* Hang safely if boot info is invalid */
        __asm__ volatile ("cli; hlt");
        return;
    }

    g_boot_info_global = boot_info;

    /* 1. Initialize Dynamic Memory Heap (16MB pre-allocated) */
    heap_init(boot_info->mem.heap_physical_start, boot_info->mem.heap_size_bytes);

    /* 2. Initialize Framebuffer & Text Console */
    fb_init(&boot_info->fb);
    console_init();

    /* 3. Initialize Kernel Logging Subsystem (klog ring buffer) */
    klog_init();
    boot_log("OK", "initializing pseuDOS kernel v0.6.0-scheduling");

    /* 4. Initialize Custom 64-bit GDT & Task State Segment (TSS) */
    gdt_init();

    /* 5. Initialize 64-bit IDT & Remap 8259 PIC */
    idt_init();
    boot_log("OK", "64-bit GDT, TSS, and IDT armed");

    /* 6. Initialize Physical Memory Manager (Bitmap Page Allocator) */
    pmm_init(&boot_info->mem, boot_info);

    /* 7. Initialize Virtual Memory Manager & 4-Level Higher-Half Paging */
    vmm_init(boot_info);
    boot_log("OK", "PMM and 4-level higher-half paging active");

    /* 8. Initialize CMOS Real-Time Clock (RTC) */
    rtc_init();
    boot_log("OK", "CMOS RTC hardware clock synchronized");

    /* 9. Initialize PS/2 Keyboard Driver */
    keyboard_init();

    /* 10. Initialize Boot Location & Devpath */
    fs_init_devpath(boot_info);

    /* 11. Initialize Storage Subsystem (AHCI SATA, NVMe PCIe, USB Mass Storage) */
    storage_init();

    /* 12. Mount Persistent Boot Disk FAT32 Filesystem (or Ramfs on CD-ROM) */
    vfs_mount_boot_media(boot_info);
    boot_log("OK", "storage subsystem and root filesystem mounted");

    /* 13. Initialize PIT Timer & Syscall MSRs */
    pit_init(PIT_FREQUENCY_HZ);
    syscall_init();
    boot_log("OK", "PIT timer and x86_64 syscall interface active");

    /* 14. Initialize Process Manager & Preemptive Scheduler */
    process_init();
    scheduler_init();
    scheduler_start();
    boot_log("OK", "preemptive round-robin scheduler armed (~100 Hz)");

    /* 15. Launch Userland Init Subsystem (autoinit.bin as PID 1) */
    const char *autoinit_path = (boot_info && boot_info->autoinit_path[0] != '\0')
                                ? boot_info->autoinit_path
                                : "/protected/krnl/autoinit.bin";

    boot_log("OK", "launching init subsystem from %s", autoinit_path);
    process_t *init_proc = pe_spawn_process("autoinit", autoinit_path, PRIV_KERNEL);
    if (!init_proc) {
        boot_log("WARN", "failed to launch %s, falling back to emergency kernel shell", autoinit_path);
        shell_init(boot_info);
        shell_run(boot_info);
    } else {
        /* PID 0 transitions into kernel idle loop */
        while (1) {
            __asm__ volatile ("hlt");
        }
    }
}
