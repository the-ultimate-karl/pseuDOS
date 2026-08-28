#include "kernel.h"
#include "drivers.h"
#include "fs.h"

void kernel_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    /* Initialize subsystems */
    console_init(SystemTable);
    keyboard_init(SystemTable);
    fs_init_boot_location(ImageHandle, SystemTable);

    console_printf("pseuDOS Kernel v0.2.0-native (x86_64 UEFI)\n");
    console_printf("Type 'help' to view available commands.\n\n");

    /* Enter interactive kernel shell */
    shell_run(SystemTable);

    /* Should never reach here */
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}
