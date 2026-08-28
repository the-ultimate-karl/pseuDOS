#include "drivers.h"
#include "io.h"
#include "lib.h"

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1U << 31) |
                                  ((uint32_t)bus << 16) |
                                  ((uint32_t)slot << 11) |
                                  ((uint32_t)func << 8) |
                                  (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, slot, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, slot, func, offset);
    return (uint8_t)((val >> ((offset & 3) * 8)) & 0xFF);
}

static const char *pci_class_name(uint8_t base_class, uint8_t sub_class, uint8_t prog_if) {
    (void)prog_if;
    switch (base_class) {
        case 0x01:
            switch (sub_class) {
                case 0x01: return "IDE Controller";
                case 0x06: return "SATA Controller (AHCI)";
                case 0x08: return "NVMe Controller";
                default:   return "Mass Storage Controller";
            }
        case 0x02:
            switch (sub_class) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default:   return "Network Device";
            }
        case 0x03:
            switch (sub_class) {
                case 0x00: return "VGA Compatible Controller";
                default:   return "Display Controller";
            }
        case 0x04: return "Multimedia / Audio Device";
        case 0x05: return "Memory Controller";
        case 0x06:
            switch (sub_class) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                default:   return "Bridge Device";
            }
        case 0x07: return "Communication Controller (UART/Modem)";
        case 0x08: return "Generic System Peripheral";
        case 0x0C:
            switch (sub_class) {
                case 0x03:
                    if (prog_if == 0x30) return "USB 3.0 Host Controller (xHCI)";
                    if (prog_if == 0x20) return "USB 2.0 Host Controller (EHCI)";
                    return "USB Host Controller (UHCI/OHCI)";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        default:
            return "Generic PCI Device";
    }
}

void pci_scan_bus(void) {
    console_printf("PCI Bus Scan (I/O Ports 0xCF8 / 0xCFC):\n");
    console_printf("  Bus:Dev:Fn   Vendor:Device   Class Code   Description\n");
    console_printf("  ----------   -------------   ----------   --------------------------------\n");

    int found = 0;

    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor_id = pci_read16((uint8_t)bus, slot, 0, 0x00);
            if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
                continue;
            }

            uint8_t header_type = pci_read8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint16_t vid = pci_read16((uint8_t)bus, slot, func, 0x00);
                if (vid == 0xFFFF || vid == 0x0000) continue;

                uint16_t did = pci_read16((uint8_t)bus, slot, func, 0x02);
                uint8_t base_class = pci_read8((uint8_t)bus, slot, func, 0x0B);
                uint8_t sub_class = pci_read8((uint8_t)bus, slot, func, 0x0A);
                uint8_t prog_if = pci_read8((uint8_t)bus, slot, func, 0x09);

                const char *desc = pci_class_name(base_class, sub_class, prog_if);

                console_printf("  %02x:%02x.%01x      0x%04x:0x%04x   0x%02x%02x%02x     %s\n",
                               bus, slot, func, vid, did, base_class, sub_class, prog_if, desc);
                found++;
            }
        }
    }

    console_printf("  ----------   -------------   ----------   --------------------------------\n");
    console_printf("  Total PCI devices detected: %d\n", found);
}
