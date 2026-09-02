#include "storage.h"
#include "drivers.h"
#include "io.h"
#include "lib.h"

/* PCI Configuration I/O Ports */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* AHCI HBA Memory Register Definitions */
#define HBA_GHC_AE   (1 << 31)  /* AHCI Enable */
#define HBA_GHC_HR   (1 << 0)   /* HBA Reset */

#define PORT_CMD_ST  (1 << 0)   /* Start */
#define PORT_CMD_FRE (1 << 4)   /* FIS Receive Enable */
#define PORT_CMD_FR  (1 << 14)  /* FIS Receive Running */
#define PORT_CMD_CR  (1 << 15)  /* Command List Running */

#define SATA_SIG_ATA   0x00000101  /* SATA Drive */
#define SATA_SIG_ATAPI 0xEB140101  /* SATAPI Optical Drive */

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} HbaPort;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    HbaPort  ports[32];
} HbaMem;

typedef struct {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} HbaCmdHeader;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} HbaPrdtEntry;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    HbaPrdtEntry prdt_entry[1];
} HbaCmdTable;

typedef struct {
    HbaMem *hba;
    HbaPort *port;
    uint8_t port_num;
    HbaCmdHeader *cmd_headers;
    HbaCmdTable *cmd_tables;
    void *fib_buf;
} AhciPortDriver;

int storage_register_device(const StorageDevice *dev);

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t cur = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFF << shift)) | ((uint32_t)val << shift);
    outl(PCI_CONFIG_DATA, cur);
}

static void port_stop_cmd(HbaPort *port) {
    port->cmd &= ~PORT_CMD_ST;
    port->cmd &= ~PORT_CMD_FRE;
    while (port->cmd & (PORT_CMD_FR | PORT_CMD_CR));
}

static void port_start_cmd(HbaPort *port) {
    while (port->cmd & PORT_CMD_CR);
    port->cmd |= PORT_CMD_FRE;
    port->cmd |= PORT_CMD_ST;
}

static int ahci_read_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->driver_priv || !buf || count == 0) return -1;
    AhciPortDriver *driver = (AhciPortDriver *)dev->driver_priv;
    HbaPort *port = driver->port;

    port->is = (uint32_t)-1;
    HbaCmdHeader *cmd_header = &driver->cmd_headers[0];
    cmd_header->cfl = sizeof(uint32_t) * 5 / 4;
    cmd_header->w = 0;
    cmd_header->prdtl = 1;

    HbaCmdTable *cmd_table = driver->cmd_tables;
    memset(cmd_table, 0, sizeof(HbaCmdTable));

    cmd_table->prdt_entry[0].dba = (uint32_t)(uintptr_t)buf;
    cmd_table->prdt_entry[0].dbau = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
    cmd_table->prdt_entry[0].dbc = (count * 512) - 1;
    cmd_table->prdt_entry[0].i = 1;

    uint8_t *fis = cmd_table->cfis;
    fis[0] = 0x27;       /* Host to Device Register FIS */
    fis[1] = 1 << 7;     /* Command */
    fis[2] = ATA_CMD_READ_DMA_EX;
    fis[3] = 0;

    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 1 << 6;     /* LBA mode */

    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[11] = 0;

    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
    fis[14] = 0;
    fis[15] = 0;

    port->ci = 1;
    while (port->ci & 1) {
        if (port->is & (1 << 30)) { /* Task File Error */
            return -1;
        }
    }

    if (port->is & (1 << 30)) return -1;
    return 0;
}

static int ahci_write_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->driver_priv || !buf || count == 0) return -1;
    AhciPortDriver *driver = (AhciPortDriver *)dev->driver_priv;
    HbaPort *port = driver->port;

    port->is = (uint32_t)-1;
    HbaCmdHeader *cmd_header = &driver->cmd_headers[0];
    cmd_header->cfl = sizeof(uint32_t) * 5 / 4;
    cmd_header->w = 1; /* Write */
    cmd_header->prdtl = 1;

    HbaCmdTable *cmd_table = driver->cmd_tables;
    memset(cmd_table, 0, sizeof(HbaCmdTable));

    cmd_table->prdt_entry[0].dba = (uint32_t)(uintptr_t)buf;
    cmd_table->prdt_entry[0].dbau = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
    cmd_table->prdt_entry[0].dbc = (count * 512) - 1;
    cmd_table->prdt_entry[0].i = 1;

    uint8_t *fis = cmd_table->cfis;
    fis[0] = 0x27;       /* Host to Device Register FIS */
    fis[1] = 1 << 7;     /* Command */
    fis[2] = ATA_CMD_WRITE_DMA_EX;
    fis[3] = 0;

    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 1 << 6;     /* LBA mode */

    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[11] = 0;

    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
    fis[14] = 0;
    fis[15] = 0;

    port->ci = 1;
    while (port->ci & 1) {
        if (port->is & (1 << 30)) {
            return -1;
        }
    }

    if (port->is & (1 << 30)) return -1;
    return 0;
}

static void probe_sata_port(HbaMem *hba, uint8_t port_num, uint8_t bus, uint8_t slot, uint8_t func) {
    HbaPort *port = &hba->ports[port_num];
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3 || ipm != 1) return; /* Device not present and active */
    if (port->sig != SATA_SIG_ATA) return; /* Only SATA hard drives and SSDs */

    port_stop_cmd(port);

    /* Allocate memory buffers for port command headers, command tables, and FIS */
    HbaCmdHeader *cmd_headers = (HbaCmdHeader *)kmalloc(sizeof(HbaCmdHeader) * 32);
    memset(cmd_headers, 0, sizeof(HbaCmdHeader) * 32);

    HbaCmdTable *cmd_tables = (HbaCmdTable *)kmalloc(sizeof(HbaCmdTable));
    memset(cmd_tables, 0, sizeof(HbaCmdTable));

    void *fib_buf = kmalloc(256);
    memset(fib_buf, 0, 256);

    port->clb = (uint32_t)(uintptr_t)cmd_headers;
    port->clbu = (uint32_t)(((uint64_t)(uintptr_t)cmd_headers) >> 32);
    port->fb = (uint32_t)(uintptr_t)fib_buf;
    port->fbu = (uint32_t)(((uint64_t)(uintptr_t)fib_buf) >> 32);

    cmd_headers[0].ctba = (uint32_t)(uintptr_t)cmd_tables;
    cmd_headers[0].ctbau = (uint32_t)(((uint64_t)(uintptr_t)cmd_tables) >> 32);

    port_start_cmd(port);

    /* Allocate 512-byte buffer for IDENTIFY DEVICE */
    uint16_t *ident_buf = (uint16_t *)kmalloc(512);
    memset(ident_buf, 0, 512);

    port->is = (uint32_t)-1;
    cmd_headers[0].cfl = sizeof(uint32_t) * 5 / 4;
    cmd_headers[0].w = 0;
    cmd_headers[0].prdtl = 1;

    cmd_tables->prdt_entry[0].dba = (uint32_t)(uintptr_t)ident_buf;
    cmd_tables->prdt_entry[0].dbau = (uint32_t)(((uint64_t)(uintptr_t)ident_buf) >> 32);
    cmd_tables->prdt_entry[0].dbc = 512 - 1;
    cmd_tables->prdt_entry[0].i = 1;

    uint8_t *fis = cmd_tables->cfis;
    fis[0] = 0x27;
    fis[1] = 1 << 7;
    fis[2] = ATA_CMD_IDENTIFY;

    port->ci = 1;
    int timeout = 100000;
    while ((port->ci & 1) && --timeout > 0);

    char model[41];
    memset(model, 0, sizeof(model));

    uint64_t total_sectors = 0;

    if (timeout > 0 && !(port->is & (1 << 30))) {
        /* Extract ATA Model String (Words 27-46, bytes swapped) */
        for (int i = 0; i < 20; i++) {
            uint16_t word = ident_buf[27 + i];
            model[i * 2] = (char)(word >> 8);
            model[i * 2 + 1] = (char)(word & 0xFF);
        }
        model[40] = '\0';
        trim(model);

        /* Extract 48-bit LBA sector count */
        total_sectors = ((uint64_t)ident_buf[103] << 48) |
                        ((uint64_t)ident_buf[102] << 32) |
                        ((uint64_t)ident_buf[101] << 16) |
                        (uint64_t)ident_buf[100];
        if (total_sectors == 0) {
            total_sectors = ((uint32_t)ident_buf[61] << 16) | (uint32_t)ident_buf[60];
        }
    } else {
        strcpy(model, "SATA Hard Disk");
        total_sectors = 20971520; /* 10 GB default */
    }

    if (model[0] == '\0') {
        strcpy(model, "SATA Drive");
    }

    kfree(ident_buf);

    AhciPortDriver *driver = (AhciPortDriver *)kmalloc(sizeof(AhciPortDriver));
    driver->hba = hba;
    driver->port = port;
    driver->port_num = port_num;
    driver->cmd_headers = cmd_headers;
    driver->cmd_tables = cmd_tables;
    driver->fib_buf = fib_buf;

    StorageDevice dev;
    memset(&dev, 0, sizeof(StorageDevice));
    strncpy(dev.name, model, sizeof(dev.name) - 1);
    strcpy(dev.type_str, "SATA");
    strcpy(dev.bus_speed, "SATA 6.0 Gbps");
    dev.type = STORAGE_TYPE_INTERNAL_SATA;
    dev.total_sectors = total_sectors > 0 ? total_sectors : 20971520;
    dev.sector_size = 512;
    dev.pci_bus = bus;
    dev.pci_slot = slot;
    dev.pci_func = func;
    dev.driver_priv = driver;
    dev.read_sectors = ahci_read_sectors_impl;
    dev.write_sectors = ahci_write_sectors_impl;

    storage_format_size(dev.total_sectors * 512, dev.size_str, sizeof(dev.size_str));
    snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/Sata(0x%X,0x0,0x0)", slot, func, port_num);

    storage_register_device(&dev);
}

void ahci_init(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t val0 = pci_read32((uint8_t)bus, slot, func, 0x00);
                if ((val0 & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_reg = pci_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                /* Mass Storage Controller (0x01), SATA (0x06), AHCI (0x01) */
                if (base_class == 0x01 && sub_class == 0x06 && prog_if == 0x01) {
                    /* Enable Bus Master (bit 2) and Memory Space (bit 1) */
                    uint32_t cmd_reg = pci_read32((uint8_t)bus, slot, func, 0x04);
                    pci_write16((uint8_t)bus, slot, func, 0x04, (uint16_t)(cmd_reg | 0x06));

                    /* Read BAR5 (ABAR) */
                    uint32_t abar_phys = pci_read32((uint8_t)bus, slot, func, 0x24) & 0xFFFFFFF0;
                    if (abar_phys == 0) continue;

                    HbaMem *hba = (HbaMem *)(uintptr_t)abar_phys;
                    hba->ghc |= HBA_GHC_AE; /* Enable AHCI Mode */

                    uint32_t pi = hba->pi;
                    for (uint8_t p = 0; p < 32; p++) {
                        if (pi & (1 << p)) {
                            probe_sata_port(hba, p, (uint8_t)bus, slot, func);
                        }
                    }
                }
            }
        }
    }
}
