#include "drivers.h"
#include "io.h"
#include "lib.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1U << 31)
                                | ((uint32_t)bus << 16)
                                | ((uint32_t)slot << 11)
                                | ((uint32_t)func << 8)
                                | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config_16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config_32(bus, slot, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read_config_8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config_32(bus, slot, func, offset);
    return (uint8_t)((val >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((1U << 31)
                                | ((uint32_t)bus << 16)
                                | ((uint32_t)slot << 11)
                                | ((uint32_t)func << 8)
                                | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_config_16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t cur = pci_read_config_32(bus, slot, func, offset);
    int shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFFU << shift)) | ((uint32_t)val << shift);
    pci_write_config_32(bus, slot, func, offset, cur);
}

static const char *pci_class_to_str(uint8_t base_class, uint8_t sub_class) {
    switch (base_class) {
        case 0x00: return "Unclassified Device";
        case 0x01:
            switch (sub_class) {
                case 0x01: return "IDE Controller";
                case 0x06: return "SATA Controller (AHCI)";
                case 0x08: return "NVMe Controller";
                default:   return "Mass Storage Controller";
            }
        case 0x02: return "Network Controller (Ethernet)";
        case 0x03: return "VGA / Display Controller";
        case 0x04: return "Multimedia Audio Device";
        case 0x05: return "Memory Controller";
        case 0x06:
            switch (sub_class) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                default:   return "Bridge Device";
            }
        case 0x07: return "Communication Controller (UART)";
        case 0x0C:
            switch (sub_class) {
                case 0x03: return "USB Controller (xHCI/EHCI)";
                default:   return "Serial Bus Controller";
            }
        default:
            return "Generic PCI Device";
    }
}

void pci_scan_bus(void) {
    console_printf("PCI / PCIe Bus Scan (I/O Ports 0xCF8 / 0xCFC):\n");
    console_printf("  Bus:Dev:Fn   Vendor:Device   Class Code   Description\n");
    console_printf("  ----------   -------------   ----------   --------------------------------\n");

    int count = 0;
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor_id = pci_read_config_16((uint8_t)bus, slot, 0, 0);
            if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
                continue;
            }

            uint16_t device_id = pci_read_config_16((uint8_t)bus, slot, 0, 2);
            uint32_t class_reg = pci_read_config_32((uint8_t)bus, slot, 0, 8);
            uint8_t base_class = (uint8_t)(class_reg >> 24);
            uint8_t sub_class = (uint8_t)(class_reg >> 16);
            uint8_t prog_if = (uint8_t)(class_reg >> 8);

            console_printf("  %02u:%02u.0      0x%04x:0x%04x   0x%02x%02x%02x     %s\n",
                bus, slot, vendor_id, device_id,
                base_class, sub_class, prog_if,
                pci_class_to_str(base_class, sub_class));
            count++;
        }
    }

    console_printf("  ----------   -------------   ----------   --------------------------------\n");
    console_printf("  Total PCI / PCIe devices detected: %d\n", count);
}
