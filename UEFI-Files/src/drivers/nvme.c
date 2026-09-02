#include "storage.h"
#include "drivers.h"
#include "io.h"
#include "lib.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* NVMe Controller Register Offsets */
#define NVME_REG_CAP   0x00
#define NVME_REG_VS    0x08
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30

#define NVME_ADMIN_IDENTIFY  0x06
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05

#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ  0x02

typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsv0;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) NvmeCmd;

typedef struct {
    uint32_t dw0;
    uint32_t rsv0;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) NvmeCqe;

typedef struct {
    uintptr_t bar0;
    uint32_t dstrd;
    uint32_t nsid;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  acq_phase;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint8_t  iocq_phase;
    uint16_t cmd_id;
    NvmeCmd *asq;
    NvmeCqe *acq;
    NvmeCmd *iosq;
    NvmeCqe *iocq;
} NvmeDriver;

static NvmeCmd g_asq[64] __attribute__((aligned(4096)));
static NvmeCqe g_acq[64] __attribute__((aligned(4096)));
static NvmeCmd g_iosq[64] __attribute__((aligned(4096)));
static NvmeCqe g_iocq[64] __attribute__((aligned(4096)));
static uint8_t g_ident_buf[4096] __attribute__((aligned(4096)));
static uint8_t g_nvme_io_buf[4096] __attribute__((aligned(4096)));

int storage_register_device(const StorageDevice *dev);

static uint32_t nvme_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void nvme_pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t cur = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFF << shift)) | ((uint32_t)val << shift);
    outl(PCI_CONFIG_DATA, cur);
}

static inline volatile uint32_t *nvme_reg(NvmeDriver *d, uint32_t offset) {
    return (volatile uint32_t *)(d->bar0 + offset);
}

static inline volatile uint32_t *nvme_doorbell(NvmeDriver *d, uint32_t qid, int is_cq) {
    uint32_t offset = 0x1000 + (2 * qid + (is_cq ? 1 : 0)) * (4 << d->dstrd);
    return (volatile uint32_t *)(d->bar0 + offset);
}

static int nvme_submit_admin_cmd(NvmeDriver *d, NvmeCmd *cmd) {
    cmd->cid = d->cmd_id++;
    memcpy(&d->asq[d->sq_tail], cmd, sizeof(NvmeCmd));

    d->sq_tail = (d->sq_tail + 1) % 64;
    *nvme_doorbell(d, 0, 0) = d->sq_tail;

    int timeout = 500000;
    while (((d->acq[d->cq_head].status & 1) != d->acq_phase) && --timeout > 0);

    if (timeout <= 0) return -1;

    uint16_t cqe_status = d->acq[d->cq_head].status;
    d->cq_head++;
    if (d->cq_head == 64) {
        d->cq_head = 0;
        d->acq_phase ^= 1;
    }
    *nvme_doorbell(d, 0, 1) = d->cq_head;

    if ((cqe_status >> 1) != 0) {
        return -1;
    }
    return 0;
}

static int nvme_submit_io_cmd(NvmeDriver *d, NvmeCmd *cmd) {
    cmd->cid = d->cmd_id++;
    memcpy(&d->iosq[d->io_sq_tail], cmd, sizeof(NvmeCmd));

    d->io_sq_tail = (d->io_sq_tail + 1) % 64;
    *nvme_doorbell(d, 1, 0) = d->io_sq_tail;

    int timeout = 500000;
    while (((d->iocq[d->io_cq_head].status & 1) != d->iocq_phase) && --timeout > 0);

    if (timeout <= 0) return -1;

    uint16_t cqe_status = d->iocq[d->io_cq_head].status;
    d->io_cq_head++;
    if (d->io_cq_head == 64) {
        d->io_cq_head = 0;
        d->iocq_phase ^= 1;
    }
    *nvme_doorbell(d, 1, 1) = d->io_cq_head;

    if ((cqe_status >> 1) != 0) {
        return -1;
    }
    return 0;
}

static int nvme_read_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->driver_priv || !buf || count == 0) return -1;
    NvmeDriver *d = (NvmeDriver *)dev->driver_priv;

    uint32_t sectors_left = count;
    uint64_t cur_lba = lba;
    uint8_t *dst = (uint8_t *)buf;

    while (sectors_left > 0) {
        uint32_t chunk = sectors_left > 8 ? 8 : sectors_left;

        NvmeCmd cmd;
        memset(&cmd, 0, sizeof(NvmeCmd));
        cmd.opcode = NVME_CMD_READ;
        cmd.nsid = d->nsid > 0 ? d->nsid : 1;
        cmd.prp1 = (uint64_t)(uintptr_t)g_nvme_io_buf;
        cmd.cdw10 = (uint32_t)cur_lba;
        cmd.cdw11 = (uint32_t)(cur_lba >> 32);
        cmd.cdw12 = (chunk - 1) & 0xFFFF;

        if (nvme_submit_io_cmd(d, &cmd) != 0) {
            return -1;
        }

        memcpy(dst, g_nvme_io_buf, chunk * 512);

        sectors_left -= chunk;
        cur_lba += chunk;
        dst += chunk * 512;
    }

    return 0;
}

static int nvme_write_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->driver_priv || !buf || count == 0) return -1;
    NvmeDriver *d = (NvmeDriver *)dev->driver_priv;

    uint32_t sectors_left = count;
    uint64_t cur_lba = lba;
    const uint8_t *src = (const uint8_t *)buf;

    while (sectors_left > 0) {
        uint32_t chunk = sectors_left > 8 ? 8 : sectors_left;
        memcpy(g_nvme_io_buf, src, chunk * 512);

        NvmeCmd cmd;
        memset(&cmd, 0, sizeof(NvmeCmd));
        cmd.opcode = NVME_CMD_WRITE;
        cmd.nsid = d->nsid > 0 ? d->nsid : 1;
        cmd.prp1 = (uint64_t)(uintptr_t)g_nvme_io_buf;
        cmd.cdw10 = (uint32_t)cur_lba;
        cmd.cdw11 = (uint32_t)(cur_lba >> 32);
        cmd.cdw12 = (chunk - 1) & 0xFFFF;

        if (nvme_submit_io_cmd(d, &cmd) != 0) {
            return -1;
        }

        sectors_left -= chunk;
        cur_lba += chunk;
        src += chunk * 512;
    }

    return 0;
}

static void probe_nvme_controller(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar0_low = nvme_pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0;
    uint32_t bar0_high = nvme_pci_read32(bus, slot, func, 0x14);
    uint64_t bar0_phys = ((uint64_t)bar0_high << 32) | bar0_low;

    if (bar0_phys == 0) return;

    /* Enable Bus Master (bit 2) and Memory Space (bit 1) */
    uint32_t cmd_reg = nvme_pci_read32(bus, slot, func, 0x04);
    nvme_pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd_reg | 0x06));

    NvmeDriver *driver = (NvmeDriver *)kmalloc(sizeof(NvmeDriver));
    memset(driver, 0, sizeof(NvmeDriver));
    driver->bar0 = (uintptr_t)bar0_phys;
    driver->acq_phase = 1;
    driver->iocq_phase = 1;

    /* Read CAP register */
    uint32_t cap_high = *nvme_reg(driver, NVME_REG_CAP + 4);
    driver->dstrd = (cap_high >> 0) & 0x0F;

    /* Disable controller before config */
    *nvme_reg(driver, NVME_REG_CC) = 0;
    int timeout = 50000;
    while ((*nvme_reg(driver, NVME_REG_CSTS) & 1) && --timeout > 0);

    /* Use 4096-byte aligned static queues */
    driver->asq = g_asq;
    memset(driver->asq, 0, sizeof(g_asq));

    driver->acq = g_acq;
    memset(driver->acq, 0, sizeof(g_acq));

    /* Set AQA (64 entries each: size - 1 = 63) */
    *nvme_reg(driver, NVME_REG_AQA) = (63 << 16) | 63;
    *nvme_reg(driver, NVME_REG_ASQ) = (uint32_t)(uintptr_t)driver->asq;
    *nvme_reg(driver, NVME_REG_ASQ + 4) = (uint32_t)(((uint64_t)(uintptr_t)driver->asq) >> 32);
    *nvme_reg(driver, NVME_REG_ACQ) = (uint32_t)(uintptr_t)driver->acq;
    *nvme_reg(driver, NVME_REG_ACQ + 4) = (uint32_t)(((uint64_t)(uintptr_t)driver->acq) >> 32);

    /* Enable controller: CC.EN = 1, IOSQES = 6 (64B), IOCQES = 4 (16B) */
    *nvme_reg(driver, NVME_REG_CC) = (1 << 0) | (6 << 16) | (4 << 20);

    timeout = 50000;
    while (!(*nvme_reg(driver, NVME_REG_CSTS) & 1) && --timeout > 0);

    /* Query Identify Controller */
    memset(g_ident_buf, 0, sizeof(g_ident_buf));

    NvmeCmd id_cmd;
    memset(&id_cmd, 0, sizeof(NvmeCmd));
    id_cmd.opcode = NVME_ADMIN_IDENTIFY;
    id_cmd.prp1 = (uint64_t)(uintptr_t)g_ident_buf;
    id_cmd.cdw10 = 1; /* CNS = 1: Identify Controller */

    char model[41];
    memset(model, 0, sizeof(model));

    if (nvme_submit_admin_cmd(driver, &id_cmd) == 0) {
        /* Model Number bytes 24..63 (40 chars) */
        memcpy(model, &g_ident_buf[24], 40);
        model[40] = '\0';
        trim(model);
    } else {
        strcpy(model, "NVMe SSD");
    }

    if (model[0] == '\0') {
        strcpy(model, "NVMe PCIe SSD");
    }

    /* Query Identify Namespace 1 */
    memset(g_ident_buf, 0, sizeof(g_ident_buf));

    memset(&id_cmd, 0, sizeof(NvmeCmd));
    id_cmd.opcode = NVME_ADMIN_IDENTIFY;
    id_cmd.nsid = 1;
    id_cmd.prp1 = (uint64_t)(uintptr_t)g_ident_buf;
    id_cmd.cdw10 = 0; /* CNS = 0: Identify Namespace */

    uint64_t total_sectors = 0;
    uint32_t sector_size = 512;

    if (nvme_submit_admin_cmd(driver, &id_cmd) == 0) {
        total_sectors = *(uint64_t *)&g_ident_buf[0]; /* NSZE: Namespace Size in blocks */
        uint8_t flbas = g_ident_buf[26];
        uint8_t lbaf = flbas & 0x0F;
        uint8_t lbads = g_ident_buf[128 + lbaf * 4 + 2]; /* LBA Data Size 2^lbads */
        if (lbads >= 9) {
            sector_size = 1 << lbads;
        }
    } else {
        total_sectors = 488397168; /* ~250 GB default */
    }

    if (total_sectors == 0) total_sectors = 488397168;

    /* Use 4096-byte aligned static I/O queues */
    driver->iosq = g_iosq;
    memset(driver->iosq, 0, sizeof(g_iosq));
    driver->iocq = g_iocq;
    memset(driver->iocq, 0, sizeof(g_iocq));

    /* Admin command: Create I/O CQ (QID = 1) */
    memset(&id_cmd, 0, sizeof(NvmeCmd));
    id_cmd.opcode = NVME_ADMIN_CREATE_CQ;
    id_cmd.prp1 = (uint64_t)(uintptr_t)driver->iocq;
    id_cmd.cdw10 = (63 << 16) | 1; /* Size 64, QID 1 */
    id_cmd.cdw11 = (1 << 0);       /* Physically contiguous */
    nvme_submit_admin_cmd(driver, &id_cmd);

    /* Admin command: Create I/O SQ (QID = 1) */
    memset(&id_cmd, 0, sizeof(NvmeCmd));
    id_cmd.opcode = NVME_ADMIN_CREATE_SQ;
    id_cmd.prp1 = (uint64_t)(uintptr_t)driver->iosq;
    id_cmd.cdw10 = (63 << 16) | 1; /* Size 64, QID 1 */
    id_cmd.cdw11 = (1 << 16) | (1 << 0); /* CQID 1, Physically contiguous */
    nvme_submit_admin_cmd(driver, &id_cmd);

    driver->nsid = 1;

    StorageDevice dev;
    memset(&dev, 0, sizeof(StorageDevice));
    strncpy(dev.name, model, sizeof(dev.name) - 1);
    strcpy(dev.type_str, "NVMe");
    strcpy(dev.bus_speed, "PCIe 3.0 x4");
    dev.type = STORAGE_TYPE_INTERNAL_NVME;
    dev.total_sectors = total_sectors;
    dev.sector_size = sector_size;
    dev.pci_bus = bus;
    dev.pci_slot = slot;
    dev.pci_func = func;
    dev.driver_priv = driver;
    dev.read_sectors = nvme_read_sectors_impl;
    dev.write_sectors = nvme_write_sectors_impl;

    storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
    snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/NVMe(0x1,00-00-00-00-00-00-00-01)", slot, func);

    storage_register_device(&dev);
}

void nvme_init(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t val0 = nvme_pci_read32((uint8_t)bus, slot, func, 0x00);
                if ((val0 & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_reg = nvme_pci_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                /* Mass Storage (0x01), Non-Volatile Memory (0x08), NVM Express (0x02) */
                if (base_class == 0x01 && sub_class == 0x08 && prog_if == 0x02) {
                    probe_nvme_controller((uint8_t)bus, slot, func);
                }
            }
        }
    }
}
