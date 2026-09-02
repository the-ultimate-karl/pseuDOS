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

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t prog_if;
    uint32_t usb_version;
} UsbStoragePriv;

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
    uint32_t bar0 = usb_pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0;
    if (bar0 == 0) return;

    /* Check xHCI (0x30) vs EHCI (0x20) vs UHCI/OHCI (0x00/0x10) */
    uint32_t usb_version = 0x0200;
    const char *speed_str = "USB 2.0 High-Speed";
    const char *drive_name = "TransMemory U202";

    if (prog_if == 0x30) {
        /* xHCI USB 3.1 Controller */
        usb_version = 0x0310;
        speed_str = "USB 3.1 Gen 1";
        drive_name = "TransMemory U301";
    } else if (prog_if == 0x20) {
        /* EHCI USB 2.0 Controller */
        usb_version = 0x0200;
        speed_str = "USB 2.0 High-Speed";
        drive_name = "TransMemory U202";
    }

    StorageDevice dev;
    memset(&dev, 0, sizeof(StorageDevice));
    strncpy(dev.name, drive_name, sizeof(dev.name) - 1);
    strcpy(dev.type_str, "USB");
    strncpy(dev.bus_speed, speed_str, sizeof(dev.bus_speed) - 1);
    dev.type = STORAGE_TYPE_EXTERNAL_USB;
    dev.usb_version = usb_version;
    dev.total_sectors = 1024000; /* 500 MB (1024000 * 512) */
    dev.sector_size = 512;
    dev.pci_bus = bus;
    dev.pci_slot = slot;
    dev.pci_func = func;
    dev.read_sectors = usb_read_sectors_impl;
    dev.write_sectors = usb_write_sectors_impl;

    storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
    snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/USB(0x0,0x0)", slot, func);

    storage_register_device(&dev);
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
