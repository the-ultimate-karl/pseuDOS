#include "storage.h"
#include "drivers.h"
#include "io.h"
#include "lib.h"

int storage_register_device(const StorageDevice *dev);

/* PCI Configuration Access */
#define pci_read32  pci_read_config_32
#define pci_read16  pci_read_config_16
#define pci_read8   pci_read_config_8
#define pci_write16 pci_write_config_16

/* xHCI Capability Registers */
#define XHCI_CAP_CAPLENGTH    0x00
#define XHCI_CAP_HCIVERSION   0x02
#define XHCI_CAP_HCSPARAMS1   0x04
#define XHCI_CAP_HCSPARAMS2   0x08
#define XHCI_CAP_HCCPARAMS1   0x10
#define XHCI_CAP_DBOFF        0x14
#define XHCI_CAP_RTSOFF       0x18

/* xHCI Operational Registers */
#define XHCI_OP_USBCMD        0x00
#define XHCI_OP_USBSTS        0x04
#define XHCI_OP_PAGESIZE      0x08
#define XHCI_OP_CRCR          0x18
#define XHCI_OP_DCBAAP        0x30
#define XHCI_OP_CONFIG        0x38
#define XHCI_OP_PORTSC_BASE   0x400

#define USBCMD_RS             (1U << 0)
#define USBCMD_HCRST          (1U << 1)
#define USBSTS_HCH            (1U << 0)

#define PORTSC_CCS            (1U << 0)
#define PORTSC_PED            (1U << 1)
#define PORTSC_PR             (1U << 4)

/* TRB Types */
#define TRB_NORMAL            1
#define TRB_SETUP_STAGE       2
#define TRB_DATA_STAGE        3
#define TRB_STATUS_STAGE      4
#define TRB_LINK              6
#define TRB_ENABLE_SLOT_CMD   9
#define TRB_ADDRESS_DEV_CMD   11
#define TRB_CONFIGURE_EP_CMD  12
#define TRB_TRANSFER_EVENT    32
#define TRB_CMD_COMPLETION    33

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) XhciTrb;

typedef struct {
    uint64_t ring_address;
    uint32_t ring_size;
    uint32_t rsvd;
} __attribute__((packed)) XhciErst;

typedef struct {
    uint32_t signature;          /* "USBC" = 0x43425355 */
    uint32_t tag;
    uint32_t data_transfer_len;
    uint8_t  flags;              /* 0x80 = IN (read), 0x00 = OUT (write) */
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cb[16];
} __attribute__((packed)) UsbCbw;

typedef struct {
    uint32_t signature;          /* "USBS" = 0x53425355 */
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;             /* 0 = Passed, 1 = Failed, 2 = Phase Error */
} __attribute__((packed)) UsbCsw;

/* xHCI Controller & Driver State */
typedef struct {
    uintptr_t mmio_base;
    uintptr_t op_base;
    uintptr_t rt_base;
    uintptr_t db_base;
    uint8_t   context_size;      /* 32 or 64 bytes */
    uint8_t   slot_id;
    uint8_t   bulk_in_dci;
    uint8_t   bulk_out_dci;
    uint32_t  bulk_max_packet;
    uint32_t  cmd_enqueue;
    uint32_t  cmd_cycle;
    uint32_t  event_dequeue;
    uint32_t  event_cycle;
    uint32_t  ep0_enqueue;
    uint32_t  ep0_cycle;
    uint32_t  in_enqueue;
    uint32_t  in_cycle;
    uint32_t  out_enqueue;
    uint32_t  out_cycle;
    uint32_t  cbw_tag;
    XhciTrb   *cmd_ring;
    XhciTrb   *event_ring;
    XhciTrb   *ep0_ring;
    XhciTrb   *bulk_in_ring;
    XhciTrb   *bulk_out_ring;
} XhciDriver;

/* Alignments required by xHCI specification */
static uint64_t g_dcbaa[256] __attribute__((aligned(64)));
static XhciTrb  g_cmd_ring[64] __attribute__((aligned(64)));
static XhciTrb  g_event_ring[64] __attribute__((aligned(64)));
static XhciErst g_erst[1] __attribute__((aligned(64)));

static uint8_t  g_input_ctx[2048] __attribute__((aligned(64)));
static uint8_t  g_output_ctx[2048] __attribute__((aligned(64)));
static XhciTrb  g_ep0_ring[64] __attribute__((aligned(64)));
static XhciTrb  g_bulk_in_ring[64] __attribute__((aligned(64)));
static XhciTrb  g_bulk_out_ring[64] __attribute__((aligned(64)));
static UsbCbw   g_bot_cbw __attribute__((aligned(64)));
static UsbCsw   g_bot_csw __attribute__((aligned(64)));
static uint8_t  g_usb_dma_buf[4096] __attribute__((aligned(4096)));

static XhciDriver g_xhci;
static int g_xhci_active = 0;

/* Send a command on the xHCI Command Ring */
static int xhci_send_command(XhciDriver *d, uint64_t param, uint32_t cmd_type, uint32_t slot_id, XhciTrb *out_evt) {
    uint32_t idx = d->cmd_enqueue;
    d->cmd_ring[idx].param = param;
    d->cmd_ring[idx].status = 0;
    d->cmd_ring[idx].control = (cmd_type << 10) | (slot_id << 24) | d->cmd_cycle;

    d->cmd_enqueue = (idx + 1) % 64;
    if (d->cmd_enqueue == 0) {
        d->cmd_cycle ^= 1;
    }

    /* Ring Doorbell 0 */
    volatile uint32_t *db = (volatile uint32_t *)d->db_base;
    db[0] = 0;

    /* Wait for Command Completion Event */
    int timeout = 1000000;
    while (--timeout > 0) {
        uint32_t e_idx = d->event_dequeue;
        uint32_t ctrl = d->event_ring[e_idx].control;
        if ((ctrl & 1) == d->event_cycle) {
            uint32_t type = (ctrl >> 10) & 0x3F;
            if (type == TRB_CMD_COMPLETION) {
                if (out_evt) *out_evt = d->event_ring[e_idx];

                d->event_dequeue = (e_idx + 1) % 64;
                if (d->event_dequeue == 0) {
                    d->event_cycle ^= 1;
                }

                /* Update Event Ring Dequeue Pointer (ERDP) */
                volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
                volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
                uint64_t erdp_val = (uint64_t)(uintptr_t)&d->event_ring[d->event_dequeue] | (1U << 3);
                *erdp_low = (uint32_t)erdp_val;
                *erdp_high = (uint32_t)(erdp_val >> 32);

                uint32_t code = (d->event_ring[e_idx].status >> 24) & 0xFF;
                return (code == 1) ? 0 : -1;
            }
            /* Advance other events */
            d->event_dequeue = (e_idx + 1) % 64;
            if (d->event_dequeue == 0) d->event_cycle ^= 1;
        }
    }
    return -1;
}

/* Wait for Transfer Event on an Endpoint Transfer Ring */
static int xhci_wait_transfer_event(XhciDriver *d) {
    int timeout = 2000000;
    while (--timeout > 0) {
        uint32_t e_idx = d->event_dequeue;
        uint32_t ctrl = d->event_ring[e_idx].control;
        if ((ctrl & 1) == d->event_cycle) {
            uint32_t type = (ctrl >> 10) & 0x3F;
            if (type == TRB_TRANSFER_EVENT) {
                uint32_t code = (d->event_ring[e_idx].status >> 24) & 0xFF;

                d->event_dequeue = (e_idx + 1) % 64;
                if (d->event_dequeue == 0) d->event_cycle ^= 1;

                volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
                volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
                uint64_t erdp_val = (uint64_t)(uintptr_t)&d->event_ring[d->event_dequeue] | (1U << 3);
                *erdp_low = (uint32_t)erdp_val;
                *erdp_high = (uint32_t)(erdp_val >> 32);

                return (code == 1 || code == 13) ? 0 : -1;
            }
            d->event_dequeue = (e_idx + 1) % 64;
            if (d->event_dequeue == 0) d->event_cycle ^= 1;
        }
    }
    return -1;
}

/* Send a USB Control Request on EP0 */
static int xhci_ep0_transfer(XhciDriver *d, uint8_t req_type, uint8_t request, uint16_t val, uint16_t idx, uint16_t len, void *data, int is_in) {
    /* 1. Setup Stage TRB */
    uint32_t s_idx = d->ep0_enqueue;
    d->ep0_ring[s_idx].param = ((uint64_t)request << 8) | req_type | ((uint64_t)val << 16) | ((uint64_t)idx << 32) | ((uint64_t)len << 48);
    d->ep0_ring[s_idx].status = 8; /* Length */
    uint32_t trt = (len == 0) ? 0 : (is_in ? 3 : 2);
    d->ep0_ring[s_idx].control = (TRB_SETUP_STAGE << 10) | (trt << 16) | (1 << 6) /* IDT */ | d->ep0_cycle;
    d->ep0_enqueue = (s_idx + 1) % 64;
    if (d->ep0_enqueue == 0) d->ep0_cycle ^= 1;

    /* 2. Data Stage TRB (if len > 0) */
    if (len > 0) {
        uint32_t d_idx = d->ep0_enqueue;
        d->ep0_ring[d_idx].param = (uint64_t)(uintptr_t)data;
        d->ep0_ring[d_idx].status = len;
        d->ep0_ring[d_idx].control = (TRB_DATA_STAGE << 10) | (is_in ? (1 << 16) : 0) | (1 << 2) /* ENT */ | d->ep0_cycle;
        d->ep0_enqueue = (d_idx + 1) % 64;
        if (d->ep0_enqueue == 0) d->ep0_cycle ^= 1;
    }

    /* 3. Status Stage TRB */
    uint32_t st_idx = d->ep0_enqueue;
    d->ep0_ring[st_idx].param = 0;
    d->ep0_ring[st_idx].status = 0;
    d->ep0_ring[st_idx].control = (TRB_STATUS_STAGE << 10) | (is_in ? 0 : (1 << 16)) | (1 << 5) /* IOC */ | d->ep0_cycle;
    d->ep0_enqueue = (st_idx + 1) % 64;
    if (d->ep0_enqueue == 0) d->ep0_cycle ^= 1;

    /* Ring Doorbell for Slot ID, Target = 1 (EP0) */
    volatile uint32_t *db = (volatile uint32_t *)(d->db_base + d->slot_id * 4);
    *db = 1;

    return xhci_wait_transfer_event(d);
}

/* Execute SCSI Bulk-Only Transport (BOT) Command */
static int xhci_bot_exec(XhciDriver *d, const void *cdb, uint8_t cdb_len, void *buf, uint32_t buf_len, int is_read) {
    if (!d || !g_xhci_active) return -1;

    d->cbw_tag++;

    /* 1. Build Command Block Wrapper (CBW) */
    memset(&g_bot_cbw, 0, sizeof(UsbCbw));
    g_bot_cbw.signature = 0x43425355;
    g_bot_cbw.tag = d->cbw_tag;
    g_bot_cbw.data_transfer_len = buf_len;
    g_bot_cbw.flags = is_read ? 0x80 : 0x00;
    g_bot_cbw.lun = 0;
    g_bot_cbw.cb_len = cdb_len > 16 ? 16 : cdb_len;
    memcpy(g_bot_cbw.cb, cdb, g_bot_cbw.cb_len);

    /* Enqueue CBW on Bulk OUT ring */
    uint32_t out_idx = d->out_enqueue;
    d->bulk_out_ring[out_idx].param = (uint64_t)(uintptr_t)&g_bot_cbw;
    d->bulk_out_ring[out_idx].status = sizeof(UsbCbw);
    d->bulk_out_ring[out_idx].control = (TRB_NORMAL << 10) | (1 << 5) /* IOC */ | d->out_cycle;
    d->out_enqueue = (out_idx + 1) % 64;
    if (d->out_enqueue == 0) d->out_cycle ^= 1;

    volatile uint32_t *db_out = (volatile uint32_t *)(d->db_base + d->slot_id * 4);
    *db_out = d->bulk_out_dci;

    if (xhci_wait_transfer_event(d) != 0) {
        return -1;
    }

    /* 2. Transfer Data Stage (if any) */
    if (buf && buf_len > 0) {
        if (is_read) {
            uint32_t in_idx = d->in_enqueue;
            d->bulk_in_ring[in_idx].param = (uint64_t)(uintptr_t)buf;
            d->bulk_in_ring[in_idx].status = buf_len;
            d->bulk_in_ring[in_idx].control = (TRB_NORMAL << 10) | (1 << 5) /* IOC */ | d->in_cycle;
            d->in_enqueue = (in_idx + 1) % 64;
            if (d->in_enqueue == 0) d->in_cycle ^= 1;

            volatile uint32_t *db_in = (volatile uint32_t *)(d->db_base + d->slot_id * 4);
            *db_in = d->bulk_in_dci;

            if (xhci_wait_transfer_event(d) != 0) {
                return -1;
            }
        } else {
            uint32_t o_idx = d->out_enqueue;
            d->bulk_out_ring[o_idx].param = (uint64_t)(uintptr_t)buf;
            d->bulk_out_ring[o_idx].status = buf_len;
            d->bulk_out_ring[o_idx].control = (TRB_NORMAL << 10) | (1 << 5) /* IOC */ | d->out_cycle;
            d->out_enqueue = (o_idx + 1) % 64;
            if (d->out_enqueue == 0) d->out_cycle ^= 1;

            *db_out = d->bulk_out_dci;

            if (xhci_wait_transfer_event(d) != 0) {
                return -1;
            }
        }
    }

    /* 3. Read Command Status Wrapper (CSW) on Bulk IN */
    memset(&g_bot_csw, 0, sizeof(UsbCsw));

    uint32_t csw_idx = d->in_enqueue;
    d->bulk_in_ring[csw_idx].param = (uint64_t)(uintptr_t)&g_bot_csw;
    d->bulk_in_ring[csw_idx].status = sizeof(UsbCsw);
    d->bulk_in_ring[csw_idx].control = (TRB_NORMAL << 10) | (1 << 5) /* IOC */ | d->in_cycle;
    d->in_enqueue = (csw_idx + 1) % 64;
    if (d->in_enqueue == 0) d->in_cycle ^= 1;

    volatile uint32_t *db_in = (volatile uint32_t *)(d->db_base + d->slot_id * 4);
    *db_in = d->bulk_in_dci;

    if (xhci_wait_transfer_event(d) != 0) {
        return -1;
    }

    if (g_bot_csw.signature == 0x53425355 && g_bot_csw.tag == d->cbw_tag && g_bot_csw.status == 0) {
        return 0; /* Passed */
    }

    return -1;
}

/* SCSI INQUIRY Command (0x12) */
static int scsi_inquiry(XhciDriver *d, char *out_name, size_t max_len) {
    uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, 36, 0x00 };
    memset(g_usb_dma_buf, 0, 64);

    if (xhci_bot_exec(d, cdb, 6, g_usb_dma_buf, 36, 1) != 0) {
        return -1;
    }

    char vendor[9];
    char product[17];
    memcpy(vendor, &g_usb_dma_buf[8], 8);
    vendor[8] = '\0';
    trim(vendor);

    memcpy(product, &g_usb_dma_buf[16], 16);
    product[16] = '\0';
    trim(product);

    if (vendor[0] != '\0' && product[0] != '\0') {
        snprintf(out_name, max_len, "%s %s", vendor, product);
    } else if (product[0] != '\0') {
        strncpy(out_name, product, max_len - 1);
    } else {
        strncpy(out_name, "USB Mass Storage Drive", max_len - 1);
    }
    out_name[max_len - 1] = '\0';
    return 0;
}

/* SCSI READ CAPACITY (10) Command (0x25) */
static int scsi_read_capacity(XhciDriver *d, uint64_t *out_sectors, uint32_t *out_sec_size) {
    uint8_t cdb[10] = { 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    memset(g_usb_dma_buf, 0, 16);

    if (xhci_bot_exec(d, cdb, 10, g_usb_dma_buf, 8, 1) != 0) {
        return -1;
    }

    uint32_t last_lba = ((uint32_t)g_usb_dma_buf[0] << 24) | ((uint32_t)g_usb_dma_buf[1] << 16) |
                        ((uint32_t)g_usb_dma_buf[2] << 8)  | (uint32_t)g_usb_dma_buf[3];
    uint32_t blk_size = ((uint32_t)g_usb_dma_buf[4] << 24) | ((uint32_t)g_usb_dma_buf[5] << 16) |
                        ((uint32_t)g_usb_dma_buf[6] << 8)  | (uint32_t)g_usb_dma_buf[7];

    if (out_sectors) *out_sectors = (uint64_t)last_lba + 1;
    if (out_sec_size) *out_sec_size = blk_size > 0 ? blk_size : 512;
    return 0;
}

/* SCSI READ (10) Command (0x28) */
static int usb_read_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, void *buf) {
    XhciDriver *d = (XhciDriver *)dev->driver_priv;
    if (!d || !buf || count == 0) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        uint16_t chunk = remaining > 8 ? 8 : (uint16_t)remaining;
        uint8_t cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x28; /* READ (10) */
        cdb[2] = (uint8_t)(cur_lba >> 24);
        cdb[3] = (uint8_t)(cur_lba >> 16);
        cdb[4] = (uint8_t)(cur_lba >> 8);
        cdb[5] = (uint8_t)cur_lba;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        uint32_t byte_len = chunk * dev->sector_size;
        if (xhci_bot_exec(d, cdb, 10, g_usb_dma_buf, byte_len, 1) != 0) {
            return -1;
        }

        memcpy(dst, g_usb_dma_buf, byte_len);
        dst += byte_len;
        cur_lba += chunk;
        remaining -= chunk;
    }

    return 0;
}

/* SCSI WRITE (10) Command (0x2A) */
static int usb_write_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, const void *buf) {
    XhciDriver *d = (XhciDriver *)dev->driver_priv;
    if (!d || !buf || count == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        uint16_t chunk = remaining > 8 ? 8 : (uint16_t)remaining;
        uint8_t cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x2A; /* WRITE (10) */
        cdb[2] = (uint8_t)(cur_lba >> 24);
        cdb[3] = (uint8_t)(cur_lba >> 16);
        cdb[4] = (uint8_t)(cur_lba >> 8);
        cdb[5] = (uint8_t)cur_lba;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        uint32_t byte_len = chunk * dev->sector_size;
        memcpy(g_usb_dma_buf, src, byte_len);

        if (xhci_bot_exec(d, cdb, 10, g_usb_dma_buf, byte_len, 0) != 0) {
            return -1;
        }

        src += byte_len;
        cur_lba += chunk;
        remaining -= chunk;
    }

    return 0;
}

static void probe_usb_controller(uint8_t bus, uint8_t slot, uint8_t func, uint8_t prog_if) {
    /* Enable Bus Master (bit 2) and Memory Space (bit 1) */
    uint32_t cmd_reg = pci_read32(bus, slot, func, 0x04);
    pci_write16(bus, slot, func, 0x04, (uint16_t)(cmd_reg | 0x06));

    uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFF) return;

    uint64_t mmio_base = bar0 & 0xFFFFFFF0;
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1 = pci_read32(bus, slot, func, 0x14);
        mmio_base |= ((uint64_t)bar1 << 32);
    }
    if (mmio_base == 0) return;

    /* Only xHCI (prog_if == 0x30) has USB 3.0 / Mass Storage BOT driver */
    if (prog_if != 0x30) return;

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)mmio_base;
    uint8_t caplength = cap[0];
    if (caplength == 0 || caplength > 0x80) caplength = 0x20;

    uint32_t hccparams1 = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_DBOFF) & ~0x3;
    uint32_t rtsoff = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_RTSOFF) & ~0x1F;

    XhciDriver *d = &g_xhci;
    memset(d, 0, sizeof(XhciDriver));
    d->cmd_ring = g_cmd_ring;
    d->event_ring = g_event_ring;
    d->ep0_ring = g_ep0_ring;
    d->bulk_in_ring = g_bulk_in_ring;
    d->bulk_out_ring = g_bulk_out_ring;
    d->mmio_base = mmio_base;
    d->op_base = mmio_base + caplength;
    d->rt_base = mmio_base + rtsoff;
    d->db_base = mmio_base + dboff;
    d->context_size = (hccparams1 & (1U << 2)) ? 64 : 32;

    /* Halt and Reset Controller */
    volatile uint32_t *usbcmd = (volatile uint32_t *)(d->op_base + XHCI_OP_USBCMD);
    volatile uint32_t *usbsts = (volatile uint32_t *)(d->op_base + XHCI_OP_USBSTS);

    *usbcmd &= ~USBCMD_RS;
    int timeout = 50000;
    while (!(*usbsts & USBSTS_HCH) && --timeout > 0);

    *usbcmd |= USBCMD_HCRST;
    timeout = 50000;
    while ((*usbcmd & USBCMD_HCRST) && --timeout > 0);

    /* Initialize Ring Pointers & Cycle States */
    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    memset(g_cmd_ring, 0, sizeof(g_cmd_ring));
    memset(g_event_ring, 0, sizeof(g_event_ring));

    d->cmd_enqueue = 0;
    d->cmd_cycle = 1;
    d->event_dequeue = 0;
    d->event_cycle = 1;

    /* Program MaxSlotsEn */
    volatile uint32_t *config = (volatile uint32_t *)(d->op_base + XHCI_OP_CONFIG);
    *config = 1; /* Enable Slot 1 */

    /* Program DCBAAP */
    volatile uint32_t *dcbaap_low = (volatile uint32_t *)(d->op_base + XHCI_OP_DCBAAP);
    volatile uint32_t *dcbaap_high = (volatile uint32_t *)(d->op_base + XHCI_OP_DCBAAP + 4);
    uint64_t dcbaa_phys = (uint64_t)(uintptr_t)g_dcbaa;
    *dcbaap_low = (uint32_t)dcbaa_phys;
    *dcbaap_high = (uint32_t)(dcbaa_phys >> 32);

    /* Program Command Ring Control Register (CRCR) */
    volatile uint32_t *crcr_low = (volatile uint32_t *)(d->op_base + XHCI_OP_CRCR);
    volatile uint32_t *crcr_high = (volatile uint32_t *)(d->op_base + XHCI_OP_CRCR + 4);
    uint64_t cr_phys = (uint64_t)(uintptr_t)g_cmd_ring | 1U /* RCS */;
    *crcr_low = (uint32_t)cr_phys;
    *crcr_high = (uint32_t)(cr_phys >> 32);

    /* Program Event Ring Segment Table (ERST) */
    g_erst[0].ring_address = (uint64_t)(uintptr_t)g_event_ring;
    g_erst[0].ring_size = 64;
    g_erst[0].rsvd = 0;

    volatile uint32_t *erstsz = (volatile uint32_t *)(d->rt_base + 0x20 + 0x08);
    *erstsz = 1;

    volatile uint32_t *erstba_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x10);
    volatile uint32_t *erstba_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x14);
    uint64_t erst_phys = (uint64_t)(uintptr_t)g_erst;
    *erstba_low = (uint32_t)erst_phys;
    *erstba_high = (uint32_t)(erst_phys >> 32);

    volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
    volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
    uint64_t erdp_phys = (uint64_t)(uintptr_t)g_event_ring;
    *erdp_low = (uint32_t)erdp_phys;
    *erdp_high = (uint32_t)(erdp_phys >> 32);

    volatile uint32_t *iman = (volatile uint32_t *)(d->rt_base + 0x20 + 0x00);
    *iman &= ~(1U << 1); /* Polling mode: keep hardware interrupt line disabled */

    /* Run Controller */
    *usbcmd |= USBCMD_RS;
    timeout = 50000;
    while ((*usbsts & USBSTS_HCH) && --timeout > 0);

    /* Check Root Hub Ports for Connected USB Drives */
    uint32_t hcsparams1 = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_HCSPARAMS1);
    uint8_t max_ports = (hcsparams1 >> 24) & 0xFF;
    if (max_ports == 0 || max_ports > 16) max_ports = 4;

    uintptr_t port_reg_base = d->op_base + XHCI_OP_PORTSC_BASE;

    for (uint8_t p = 0; p < max_ports; p++) {
        volatile uint32_t *portsc = (volatile uint32_t *)(port_reg_base + p * 0x10);
        uint32_t port_val = *portsc;

        if (port_val & PORTSC_CCS) {
            /* Reset Port */
            *portsc = (*portsc & ~0x00FE1FEE) | PORTSC_PR;
            timeout = 50000;
            while ((*portsc & PORTSC_PR) && --timeout > 0);

            /* Wait for Port Enabled */
            timeout = 50000;
            while (!(*portsc & PORTSC_PED) && --timeout > 0);
            if (!(*portsc & PORTSC_PED)) continue;

            uint8_t speed = (*portsc >> 10) & 0x0F;
            uint32_t ep0_max_packet = (speed == 4) ? 512 : 64;

            /* 1. ENABLE_SLOT Command */
            XhciTrb comp_evt;
            if (xhci_send_command(d, 0, TRB_ENABLE_SLOT_CMD, 0, &comp_evt) != 0) {
                continue;
            }

            d->slot_id = (comp_evt.control >> 24) & 0xFF;
            if (d->slot_id == 0) continue;

            /* 2. ADDRESS_DEVICE Command */
            memset(g_input_ctx, 0, sizeof(g_input_ctx));
            memset(g_output_ctx, 0, sizeof(g_output_ctx));
            memset(g_ep0_ring, 0, sizeof(g_ep0_ring));

            d->ep0_enqueue = 0;
            d->ep0_cycle = 1;

            uint32_t csz = d->context_size;

            /* Input Control Context: Add Slot (bit 0) and EP0 (bit 1) */
            uint32_t *icc = (uint32_t *)g_input_ctx;
            icc[1] = (1U << 0) | (1U << 1);

            /* Slot Context at offset csz */
            uint32_t *slot_ctx = (uint32_t *)(g_input_ctx + csz);
            slot_ctx[0] = (speed << 20) | (1U << 27); /* 1 Context entry */
            slot_ctx[1] = ((uint32_t)(p + 1) << 16);

            /* EP0 Context at offset 2 * csz */
            uint32_t *ep0_ctx = (uint32_t *)(g_input_ctx + 2 * csz);
            ep0_ctx[1] = (4U << 3) | (ep0_max_packet << 16); /* Control EP */
            uint64_t ep0_phys = (uint64_t)(uintptr_t)g_ep0_ring | 1U;
            ep0_ctx[2] = (uint32_t)ep0_phys;
            ep0_ctx[3] = (uint32_t)(ep0_phys >> 32);
            ep0_ctx[4] = 8; /* Avg TRB length */

            g_dcbaa[d->slot_id] = (uint64_t)(uintptr_t)g_output_ctx;

            if (xhci_send_command(d, (uint64_t)(uintptr_t)g_input_ctx, TRB_ADDRESS_DEV_CMD, d->slot_id, NULL) != 0) {
                continue;
            }

            /* 3. GET_DESCRIPTOR: Query Configuration Descriptor */
            memset(g_usb_dma_buf, 0, 256);
            if (xhci_ep0_transfer(d, 0x80, 0x06, 0x0200, 0, 255, g_usb_dma_buf, 1) != 0) {
                continue;
            }

            /* Parse Configuration Descriptor to locate Bulk IN and Bulk OUT endpoints */
            uint8_t *desc = g_usb_dma_buf;
            uint16_t total_desc_len = *(uint16_t *)&desc[2];
            if (total_desc_len > 255) total_desc_len = 255;

            uint8_t bulk_in_ep = 0;
            uint8_t bulk_out_ep = 0;
            uint16_t bulk_max_packet = (speed == 4) ? 1024 : 512;
            int is_mass_storage = 0;

            uint16_t off = desc[0];
            while (off + 2 <= total_desc_len) {
                uint8_t len = desc[off];
                uint8_t type = desc[off + 1];
                if (len == 0) break;

                if (type == 0x04 && len >= 9) { /* Interface Descriptor */
                    uint8_t if_class = desc[off + 5];
                    if (if_class == 0x08) { /* Mass Storage Class */
                        is_mass_storage = 1;
                    }
                }

                if (type == 0x05 && len >= 7) { /* Endpoint Descriptor */
                    uint8_t ep_addr = desc[off + 2];
                    uint8_t ep_attr = desc[off + 3];
                    if ((ep_attr & 0x03) == 0x02) { /* Bulk Endpoint */
                        if (ep_addr & 0x80) {
                            bulk_in_ep = ep_addr & 0x0F;
                        } else {
                            bulk_out_ep = ep_addr & 0x0F;
                        }
                    }
                }
                off += len;
            }

            /* Only configure and register if this device is genuinely a Mass Storage drive */
            if (!is_mass_storage || bulk_in_ep == 0 || bulk_out_ep == 0) {
                continue;
            }

            d->bulk_in_dci = bulk_in_ep * 2 + 1;
            d->bulk_out_dci = bulk_out_ep * 2;
            d->bulk_max_packet = bulk_max_packet;

            /* 4. CONFIGURE_ENDPOINT Command: Add Bulk IN & Bulk OUT */
            memset(g_input_ctx, 0, sizeof(g_input_ctx));
            memset(g_bulk_in_ring, 0, sizeof(g_bulk_in_ring));
            memset(g_bulk_out_ring, 0, sizeof(g_bulk_out_ring));

            d->in_enqueue = 0;  d->in_cycle = 1;
            d->out_enqueue = 0; d->out_cycle = 1;

            icc = (uint32_t *)g_input_ctx;
            icc[1] = (1U << 0) | (1U << d->bulk_in_dci) | (1U << d->bulk_out_dci);

            slot_ctx = (uint32_t *)(g_input_ctx + csz);
            uint8_t max_dci = d->bulk_in_dci > d->bulk_out_dci ? d->bulk_in_dci : d->bulk_out_dci;
            slot_ctx[0] = (speed << 20) | ((uint32_t)max_dci << 27);
            slot_ctx[1] = ((uint32_t)(p + 1) << 16);

            /* Bulk OUT Context (DCI = bulk_out_dci) */
            uint32_t *out_ctx = (uint32_t *)(g_input_ctx + (d->bulk_out_dci + 1) * csz);
            out_ctx[1] = (2U << 3) | ((uint32_t)bulk_max_packet << 16); /* Bulk OUT */
            uint64_t out_phys = (uint64_t)(uintptr_t)g_bulk_out_ring | 1U;
            out_ctx[2] = (uint32_t)out_phys;
            out_ctx[3] = (uint32_t)(out_phys >> 32);
            out_ctx[4] = 512;

            /* Bulk IN Context (DCI = bulk_in_dci) */
            uint32_t *in_ctx = (uint32_t *)(g_input_ctx + (d->bulk_in_dci + 1) * csz);
            in_ctx[1] = (6U << 3) | ((uint32_t)bulk_max_packet << 16); /* Bulk IN */
            uint64_t in_phys = (uint64_t)(uintptr_t)g_bulk_in_ring | 1U;
            in_ctx[2] = (uint32_t)in_phys;
            in_ctx[3] = (uint32_t)(in_phys >> 32);
            in_ctx[4] = 512;

            if (xhci_send_command(d, (uint64_t)(uintptr_t)g_input_ctx, TRB_CONFIGURE_EP_CMD, d->slot_id, NULL) != 0) {
                continue;
            }

            /* 5. SET_CONFIGURATION (1) */
            if (xhci_ep0_transfer(d, 0x00, 0x09, 1, 0, 0, NULL, 0) != 0) {
                continue;
            }

            g_xhci_active = 1;

            /* 6. Execute SCSI INQUIRY (0x12) */
            char drive_name[64];
            if (scsi_inquiry(d, drive_name, sizeof(drive_name)) != 0) {
                strcpy(drive_name, "USB Mass Storage Drive");
            }

            /* 7. Execute SCSI READ CAPACITY (10) (0x25) */
            uint64_t total_sectors = 0;
            uint32_t sector_size = 512;
            if (scsi_read_capacity(d, &total_sectors, &sector_size) != 0 || total_sectors == 0) {
                total_sectors = 1024000;
                sector_size = 512;
            }

            /* 8. Register Real USB Storage Device */
            StorageDevice dev;
            memset(&dev, 0, sizeof(StorageDevice));
            strncpy(dev.name, drive_name, sizeof(dev.name) - 1);
            strcpy(dev.type_str, "USB");

            const char *speed_str = "USB 3.0 SuperSpeed (5 Gbps)";
            uint32_t usb_ver = 0x0300;
            if (speed == 4) {
                speed_str = "USB 3.0 SuperSpeed (5 Gbps)";
                usb_ver = 0x0300;
            } else if (speed == 5) {
                speed_str = "USB 3.1 SuperSpeedPlus (10 Gbps)";
                usb_ver = 0x0310;
            } else if (speed == 3) {
                speed_str = "USB 2.0 High-Speed (480 Mbps)";
                usb_ver = 0x0200;
            } else {
                speed_str = "USB 1.1 Full-Speed (12 Mbps)";
                usb_ver = 0x0110;
            }

            strncpy(dev.bus_speed, speed_str, sizeof(dev.bus_speed) - 1);
            dev.type = STORAGE_TYPE_EXTERNAL_USB;
            dev.usb_version = usb_ver;
            dev.total_sectors = total_sectors;
            dev.sector_size = sector_size;
            dev.pci_bus = bus;
            dev.pci_slot = slot;
            dev.pci_func = func;
            dev.driver_priv = d;
            dev.read_sectors = usb_read_sectors_impl;
            dev.write_sectors = usb_write_sectors_impl;

            storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
            snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/USB(0x%X,0x0)", slot, func, p);

            storage_register_device(&dev);
            return;
        }
    }
}

void usb_storage_init(void) {
    g_xhci_active = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t val0 = pci_read32((uint8_t)bus, slot, func, 0x00);
                if ((val0 & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_reg = pci_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                /* Serial Bus Controller (0x0C), USB Controller (0x03) */
                if (base_class == 0x0C && sub_class == 0x03) {
                    probe_usb_controller((uint8_t)bus, slot, func, prog_if);
                    if (g_xhci_active) return;
                }
            }
        }
    }
}
