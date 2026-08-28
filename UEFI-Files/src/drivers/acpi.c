#include "drivers.h"
#include "io.h"

void acpi_reboot(void) {
    console_puts("rebooting system...\n");

    /* 1. 8042 Keyboard Controller Reset */
    uint8_t temp;
    do {
        temp = inb(0x64);
        if (temp & 1) inb(0x60);
    } while (temp & 2);
    outb(0x64, 0xFE);
    io_wait();

    /* 2. PCI Reset Register (0xCF9) */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();

    /* 3. Triple Fault Fallback */
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

    /* 1. QEMU standard ACPI poweroff (Port 0x604) */
    outw(0x604, 0x2000);
    io_wait();

    /* 2. QEMU older i440fx / Bochs (Port 0xB004) */
    outw(0xB004, 0x2000);
    io_wait();

    /* 3. VirtualBox ACPI (Port 0x4004) */
    outw(0x4004, 0x3400);
    io_wait();

    /* 4. Cloud Hypervisor (Port 0x0600) */
    outw(0x0600, 0x0034);
    io_wait();

    console_puts("system is now safe to power off.\n");
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}
