#include "storage.h"
#include "drivers.h"
#include "io.h"
#include "lib.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

int storage_register_device(const StorageDevice *dev);

static uint32_t usb_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void usb_pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t old = inl(PCI_CONFIG_DATA);
    if (offset & 2) {
        old = (old & 0x0000FFFF) | ((uint32_t)val << 16);
    } else {
        old = (old & 0xFFFF0000) | (uint32_t)val;
    }
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, old);
}

static int usb_read_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, void *buf) {
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return 0;
}

static int usb_write_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, const void *buf) {
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return 0;
}

static void probe_usb_controller(uint8_t bus, uint8_t slot, uint8_t func, uint8_t prog_if) {
    /* Enable Bus Master (bit 2) and Memory Space (bit 1) */
    uint32_t cmd_reg = usb_pci_read32(bus, slot, func, 0x04);
    usb_pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd_reg | 0x06));

    uint32_t bar0 = usb_pci_read32(bus, slot, func, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFF) return;

    uint64_t mmio_base = bar0 & 0xFFFFFFF0;
    if ((bar0 & 0x06) == 0x04) {
        /* 64-bit BAR */
        uint32_t bar1 = usb_pci_read32(bus, slot, func, 0x14);
        mmio_base |= ((uint64_t)bar1 << 32);
    }
    if (mmio_base == 0) return;

    /* 1. xHCI USB 3.x Host Controller (prog_if = 0x30) */
    if (prog_if == 0x30) {
        volatile uint8_t *cap_base = (volatile uint8_t *)(uintptr_t)mmio_base;
        uint8_t caplength = cap_base[0];
        if (caplength == 0 || caplength > 0x80) caplength = 0x20;

        volatile uint32_t *hcsparams1 = (volatile uint32_t *)(uintptr_t)(mmio_base + 0x04);
        uint8_t max_ports = (*hcsparams1 >> 24) & 0xFF;
        if (max_ports == 0 || max_ports > 32) max_ports = 4;

        uintptr_t op_base = (uintptr_t)(mmio_base + caplength);
        uintptr_t port_reg_base = op_base + 0x400;

        for (uint8_t p = 0; p < max_ports; p++) {
            volatile uint32_t *portsc = (volatile uint32_t *)(port_reg_base + p * 0x10);
            uint32_t port_val = *portsc;

            /* Check Current Connect Status (CCS - bit 0) */
            if (port_val & (1 << 0)) {
                /* Connected device found! Extract port speed from bits 13:10 */
                uint8_t speed_code = (port_val >> 10) & 0x0F;
                uint32_t usb_ver = 0x0310;
                const char *speed_str = "USB 3.1 Gen 1";
                const char *drive_name = "USB 3.1 Flash Drive";

                if (speed_code == 4) {
                    usb_ver = 0x0310;
                    speed_str = "USB 3.1 Gen 1";
                    drive_name = "USB 3.1 SuperSpeed Drive";
                } else if (speed_code == 5) {
                    usb_ver = 0x0320;
                    speed_str = "USB 3.1 Gen 2";
                    drive_name = "USB 3.1 SuperSpeedPlus Drive";
                } else if (speed_code == 3) {
                    usb_ver = 0x0200;
                    speed_str = "USB 2.0 High-Speed";
                    drive_name = "USB 2.0 Flash Drive";
                } else if (speed_code == 1 || speed_code == 2) {
                    usb_ver = 0x0110;
                    speed_str = "USB 1.1 Full-Speed";
                    drive_name = "USB 1.1 Legacy Drive";
                }

                StorageDevice dev;
                memset(&dev, 0, sizeof(StorageDevice));
                strncpy(dev.name, drive_name, sizeof(dev.name) - 1);
                strcpy(dev.type_str, "USB");
                strncpy(dev.bus_speed, speed_str, sizeof(dev.bus_speed) - 1);
                dev.type = STORAGE_TYPE_EXTERNAL_USB;
                dev.usb_version = usb_ver;
                dev.total_sectors = 1024000;
                dev.sector_size = 512;
                dev.pci_bus = bus;
                dev.pci_slot = slot;
                dev.pci_func = func;
                dev.read_sectors = usb_read_sectors_impl;
                dev.write_sectors = usb_write_sectors_impl;

                storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
                snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/USB(0x%X,0x0)", slot, func, p);

                storage_register_device(&dev);
            }
        }
    }
    /* 2. EHCI USB 2.0 Host Controller (prog_if = 0x20) */
    else if (prog_if == 0x20) {
        volatile uint8_t *cap_base = (volatile uint8_t *)(uintptr_t)mmio_base;
        uint8_t caplength = cap_base[0];
        if (caplength == 0 || caplength > 0x80) caplength = 0x20;

        volatile uint32_t *hcsparams = (volatile uint32_t *)(uintptr_t)(mmio_base + 0x04);
        uint8_t n_ports = *hcsparams & 0x0F;
        if (n_ports == 0 || n_ports > 16) n_ports = 4;

        uintptr_t op_base = (uintptr_t)(mmio_base + caplength);
        uintptr_t port_reg_base = op_base + 0x44;

        for (uint8_t p = 0; p < n_ports; p++) {
            volatile uint32_t *portsc = (volatile uint32_t *)(port_reg_base + p * 0x04);
            uint32_t port_val = *portsc;

            /* Check Current Connect Status (CCS - bit 0) */
            if (port_val & (1 << 0)) {
                StorageDevice dev;
                memset(&dev, 0, sizeof(StorageDevice));
                strncpy(dev.name, "USB 2.0 Flash Drive", sizeof(dev.name) - 1);
                strcpy(dev.type_str, "USB");
                strcpy(dev.bus_speed, "USB 2.0 High-Speed");
                dev.type = STORAGE_TYPE_EXTERNAL_USB;
                dev.usb_version = 0x0200;
                dev.total_sectors = 1024000;
                dev.sector_size = 512;
                dev.pci_bus = bus;
                dev.pci_slot = slot;
                dev.pci_func = func;
                dev.read_sectors = usb_read_sectors_impl;
                dev.write_sectors = usb_write_sectors_impl;

                storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
                snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/USB(0x%X,0x0)", slot, func, p);

                storage_register_device(&dev);
            }
        }
    }
}

void usb_storage_init(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t val0 = usb_pci_read32((uint8_t)bus, slot, func, 0x00);
                if ((val0 & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_reg = usb_pci_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                /* Serial Bus Controller (0x0C), USB Controller (0x03) */
                if (base_class == 0x0C && sub_class == 0x03) {
                    probe_usb_controller((uint8_t)bus, slot, func, prog_if);
                }
            }
        }
    }
}
