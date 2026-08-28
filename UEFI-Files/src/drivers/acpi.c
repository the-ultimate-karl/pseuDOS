#include "drivers.h"
#include "io.h"

void acpi_reboot(EFI_SYSTEM_TABLE *SystemTable) {
    if (SystemTable && SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
        SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }

    /* PS/2 keyboard controller reset fallback */
    uint8_t temp;
    do {
        temp = inb(0x64);
        if (temp & 1) inb(0x60);
    } while (temp & 2);
    outb(0x64, 0xFE);

    /* Triple fault CPU fallback */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));

    while (1) {
        __asm__ volatile ("hlt");
    }
}

void acpi_shutdown(EFI_SYSTEM_TABLE *SystemTable) {
    if (SystemTable && SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
        SystemTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }

    /* Direct ACPI PM1a port shutdown fallback */
    outw(0x604, 0x2000);  /* QEMU / Bochs older ACPI shutdown */
    outw(0xB004, 0x2000); /* Bochs / older QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */

    console_puts("\nSystem power down failed. Please turn off machine manually.\n");
    console_puts("System halted!\n");

    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}
