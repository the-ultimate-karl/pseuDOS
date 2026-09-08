#include "kernel.h"
#include "bootinfo.h"
#include "idt.h"
#include "drivers.h"
#include "storage.h"
#include "fs.h"
#include "lib.h"

#include "io.h"

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

    /* 3. Initialize 64-bit IDT & Remap 8259 PIC */
    idt_init();

    /* 4. Initialize PS/2 Keyboard Driver */
    keyboard_init();

    /* 5. Initialize Boot Location & Devpath */
    fs_init_devpath(boot_info);

    /* 6. Initialize Storage Subsystem (AHCI SATA, NVMe PCIe, USB Mass Storage) */
    storage_init();

    /* 7. Mount Persistent Boot Disk FAT32 Filesystem (or Ramfs on CD-ROM) */
    vfs_mount_boot_media(boot_info);

    /* 8. Launch Interactive Bare-Metal Kernel Shell */
    shell_init(boot_info);
    shell_run(boot_info);

    /* System halted fallback */
    while (1) {
        __asm__ volatile ("hlt");
    }
}
