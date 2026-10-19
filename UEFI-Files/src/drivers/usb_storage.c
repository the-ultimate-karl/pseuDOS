#include "storage.h"
#include "drivers.h"
#include "io.h"
#include "lib.h"
#include "klog.h"
#include "mice.h"
#include "vmm.h"

int storage_register_device(const StorageDevice *dev);

/* PCI Configuration Access */
#define pci_read32  pci_read_config_32
#define pci_read16  pci_read_config_16
#define pci_read8   pci_read_config_8
#define pci_write16 pci_write_config_16
#define pci_write32 pci_write_config_32

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
#define TRB_EVALUATE_CTX_CMD  13
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

#define MAX_XHCI_SLOTS 16
#define MAX_XHCI_CTRLS 6

/* xHCI Controller & Driver State (self-contained per-controller DMA rings) */
typedef struct {
    uint64_t  dcbaa[256] __attribute__((aligned(64)));
    XhciTrb   cmd_ring_buf[64] __attribute__((aligned(64)));
    XhciTrb   event_ring_buf[64] __attribute__((aligned(64)));
    XhciErst  erst[1] __attribute__((aligned(64)));
    uint8_t   output_ctx[MAX_XHCI_SLOTS][4096] __attribute__((aligned(64)));
    XhciTrb   ep0_ring[MAX_XHCI_SLOTS][64] __attribute__((aligned(64)));
    uint32_t  ep0_enqueue[MAX_XHCI_SLOTS];
    uint32_t  ep0_cycle[MAX_XHCI_SLOTS];
    XhciTrb   bulk_in_ring_buf[64] __attribute__((aligned(64)));
    XhciTrb   bulk_out_ring_buf[64] __attribute__((aligned(64)));

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
    uint32_t  in_enqueue;
    uint32_t  in_cycle;
    uint32_t  out_enqueue;
    uint32_t  out_cycle;
    uint32_t  cbw_tag;
    XhciTrb   *cmd_ring;
    XhciTrb   *event_ring;
    uint8_t   storage_slot_id;
    XhciTrb   *bulk_in_ring;
    XhciTrb   *bulk_out_ring;
    int       active;
} __attribute__((aligned(64))) XhciDriver;

typedef struct {
    int        active;
    XhciDriver *ctrl;
    uint8_t    slot_id;
    uint8_t    dci;
    uint8_t    type;          /* 1 = keyboard, 2 = mouse/touchpad */
    uint8_t    has_report_id; /* 1 if byte 0 is a Report ID */
    uint8_t    xy_16bit;      /* 1 if X/Y deltas are 16-bit signed */
    uint16_t   max_packet;
    uint32_t   enqueue;
    uint32_t   cycle;
    XhciTrb    ring[64] __attribute__((aligned(64)));
    uint8_t    report_buf[64] __attribute__((aligned(64)));
    uint8_t    prev_modifiers;
    uint8_t    prev_keys[6];
} XhciHidDevice;

static XhciHidDevice g_hid_devices[16];
static int g_hid_dev_count = 0;

static uint8_t  g_input_ctx[4096] __attribute__((aligned(64)));
static UsbCbw   g_bot_cbw __attribute__((aligned(64)));
static UsbCsw   g_bot_csw __attribute__((aligned(64)));
static uint8_t  g_usb_dma_buf_pool[65536 + 65536];
static inline uint8_t *get_usb_dma_buf(void) {
    return (uint8_t *)(((uintptr_t)g_usb_dma_buf_pool + 65535) & ~65535ULL);
}
#define g_usb_dma_buf get_usb_dma_buf()

static XhciDriver g_xhci_ctrls[MAX_XHCI_CTRLS] __attribute__((aligned(64)));
static int g_xhci_ctrl_count = 0;
static int g_xhci_active = 0;

/* EHCI (USB 2.0) Data Structures & State */
typedef struct {
    volatile uint32_t next_qtd;      /* Physical pointer to next qTD (bit 0 = Terminate 'T') */
    volatile uint32_t alt_next_qtd;  /* Alternate next qTD pointer (bit 0 = Terminate 'T') */
    volatile uint32_t token;         /* Status, PID, Cerr, Page, IOC, Total Bytes, Data Toggle */
    volatile uint32_t buffer[5];     /* Buffer page pointers */
} __attribute__((aligned(32))) EhciQtd;

typedef struct {
    volatile uint32_t qh_link;           /* Horizontal link pointer (bit 0 = T, bit 1-2 = Type: 01 for QH) */
    volatile uint32_t ep_char;           /* Endpoint characteristics */
    volatile uint32_t ep_cap;            /* Endpoint capabilities */
    volatile uint32_t current_qtd;       /* Current qTD pointer */
    volatile uint32_t overlay_next_qtd;  /* Overlay Next qTD */
    volatile uint32_t overlay_alt_next_qtd;
    volatile uint32_t overlay_token;
    volatile uint32_t overlay_buffer[5];
} __attribute__((aligned(64))) EhciQh;

typedef struct {
    uintptr_t mmio_base;
    uintptr_t op_base;
    uint8_t   bus;
    uint8_t   slot;
    uint8_t   func;
    uint8_t   dev_addr;
    uint8_t   bulk_in_ep;
    uint8_t   bulk_out_ep;
    uint16_t  bulk_in_mps;
    uint16_t  bulk_out_mps;
    uint32_t  cbw_tag;
    uint64_t  total_sectors;
    uint32_t  sector_size;
    int       no_sync_cache;
} EhciDriver;

static EhciDriver g_ehci_driver;
static int        g_ehci_active = 0;

static EhciQh     g_ehci_qh_ep0 __attribute__((aligned(64)));
static EhciQh     g_ehci_qh_bulk_out __attribute__((aligned(64)));
static EhciQh     g_ehci_qh_bulk_in __attribute__((aligned(64)));

static EhciQtd    g_ehci_ctrl_qtds[6] __attribute__((aligned(64)));
static uint32_t   g_ehci_ctrl_set = 0;
static EhciQtd    g_ehci_bulk_qtds[8] __attribute__((aligned(64)));
static uint32_t   g_ehci_bulk_qtd_idx = 0;
static uint8_t    g_ehci_setup_pkt[8] __attribute__((aligned(64)));
static UsbCbw     g_ehci_cbw __attribute__((aligned(64)));
static UsbCsw     g_ehci_csw __attribute__((aligned(64)));
static uint8_t    g_ehci_dma_buf_pool[32768 + 4096];
static inline uint8_t *get_ehci_dma_buf(void) {
    return (uint8_t *)(((uintptr_t)g_ehci_dma_buf_pool + 4095) & ~4095ULL);
}
#define g_ehci_dma_buf get_ehci_dma_buf()
static uint8_t    g_ehci_desc_buf[512] __attribute__((aligned(64)));

static uint8_t hid_to_ps2_scancode(uint8_t hid) {
    switch (hid) {
        case 0x04: return 0x1E; /* A */
        case 0x05: return 0x30; /* B */
        case 0x06: return 0x2E; /* C */
        case 0x07: return 0x20; /* D */
        case 0x08: return 0x12; /* E */
        case 0x09: return 0x21; /* F */
        case 0x0A: return 0x22; /* G */
        case 0x0B: return 0x23; /* H */
        case 0x0C: return 0x17; /* I */
        case 0x0D: return 0x24; /* J */
        case 0x0E: return 0x25; /* K */
        case 0x0F: return 0x26; /* L */
        case 0x10: return 0x32; /* M */
        case 0x11: return 0x31; /* N */
        case 0x12: return 0x18; /* O */
        case 0x13: return 0x19; /* P */
        case 0x14: return 0x10; /* Q */
        case 0x15: return 0x13; /* R */
        case 0x16: return 0x1F; /* S */
        case 0x17: return 0x14; /* T */
        case 0x18: return 0x16; /* U */
        case 0x19: return 0x2F; /* V */
        case 0x1A: return 0x11; /* W */
        case 0x1B: return 0x2D; /* X */
        case 0x1C: return 0x15; /* Y */
        case 0x1D: return 0x2C; /* Z */
        case 0x1E: return 0x02; /* 1 */
        case 0x1F: return 0x03; /* 2 */
        case 0x20: return 0x04; /* 3 */
        case 0x21: return 0x05; /* 4 */
        case 0x22: return 0x06; /* 5 */
        case 0x23: return 0x07; /* 6 */
        case 0x24: return 0x08; /* 7 */
        case 0x25: return 0x09; /* 8 */
        case 0x26: return 0x0A; /* 9 */
        case 0x27: return 0x0B; /* 0 */
        case 0x28: return 0x1C; /* Enter */
        case 0x29: return 0x01; /* Escape */
        case 0x2A: return 0x0E; /* Backspace */
        case 0x2B: return 0x0F; /* Tab */
        case 0x2C: return 0x39; /* Space */
        case 0x2D: return 0x0C; /* - _ */
        case 0x2E: return 0x0D; /* = + */
        case 0x2F: return 0x1A; /* [ { */
        case 0x30: return 0x1B; /* ] } */
        case 0x31: return 0x2B; /* \ | */
        case 0x33: return 0x27; /* ; : */
        case 0x34: return 0x28; /* ' " */
        case 0x35: return 0x29; /* ` ~ */
        case 0x36: return 0x33; /* , < */
        case 0x37: return 0x34; /* . > */
        case 0x38: return 0x35; /* / ? */
        case 0x39: return 0x3A; /* Caps Lock */
        case 0x4F: return 0x4D; /* Right Arrow */
        case 0x50: return 0x4B; /* Left Arrow */
        case 0x51: return 0x50; /* Down Arrow */
        case 0x52: return 0x48; /* Up Arrow */
        default: return 0;
    }
}

static void handle_usb_keyboard_report(XhciHidDevice *hid, const uint8_t *report) {
    uint8_t modifiers = report[0];
    uint8_t prev_mod = hid->prev_modifiers;

    if ((modifiers & 0x01) && !(prev_mod & 0x01)) keyboard_post_scancode(0x1D);
    else if (!(modifiers & 0x01) && (prev_mod & 0x01)) keyboard_post_scancode(0x1D | 0x80);

    if ((modifiers & 0x02) && !(prev_mod & 0x02)) keyboard_post_scancode(0x2A);
    else if (!(modifiers & 0x02) && (prev_mod & 0x02)) keyboard_post_scancode(0x2A | 0x80);

    if ((modifiers & 0x04) && !(prev_mod & 0x04)) keyboard_post_scancode(0x38);
    else if (!(modifiers & 0x04) && (prev_mod & 0x04)) keyboard_post_scancode(0x38 | 0x80);

    if ((modifiers & 0x08) && !(prev_mod & 0x08)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x5B); }
    else if (!(modifiers & 0x08) && (prev_mod & 0x08)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x5B | 0x80); }

    if ((modifiers & 0x10) && !(prev_mod & 0x10)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x1D); }
    else if (!(modifiers & 0x10) && (prev_mod & 0x10)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x1D | 0x80); }

    if ((modifiers & 0x20) && !(prev_mod & 0x20)) keyboard_post_scancode(0x36);
    else if (!(modifiers & 0x20) && (prev_mod & 0x20)) keyboard_post_scancode(0x36 | 0x80);

    if ((modifiers & 0x40) && !(prev_mod & 0x40)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x38); }
    else if (!(modifiers & 0x40) && (prev_mod & 0x40)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x38 | 0x80); }

    if ((modifiers & 0x80) && !(prev_mod & 0x80)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x5C); }
    else if (!(modifiers & 0x80) && (prev_mod & 0x80)) { keyboard_post_scancode(0xE0); keyboard_post_scancode(0x5C | 0x80); }

    hid->prev_modifiers = modifiers;

    for (int i = 0; i < 6; i++) {
        uint8_t old_key = hid->prev_keys[i];
        if (old_key >= 0x04) {
            int still_pressed = 0;
            for (int j = 2; j < 8; j++) {
                if (report[j] == old_key) {
                    still_pressed = 1;
                    break;
                }
            }
            if (!still_pressed) {
                uint8_t sc = hid_to_ps2_scancode(old_key);
                if (sc != 0) {
                    if (old_key >= 0x4F && old_key <= 0x52) {
                        keyboard_post_scancode(0xE0);
                    }
                    keyboard_post_scancode(sc | 0x80);
                }
            }
        }
    }

    for (int j = 2; j < 8; j++) {
        uint8_t new_key = report[j];
        if (new_key >= 0x04) {
            int was_pressed = 0;
            for (int i = 0; i < 6; i++) {
                if (hid->prev_keys[i] == new_key) {
                    was_pressed = 1;
                    break;
                }
            }
            if (!was_pressed) {
                uint8_t sc = hid_to_ps2_scancode(new_key);
                if (sc != 0) {
                    if (new_key >= 0x4F && new_key <= 0x52) {
                        keyboard_post_scancode(0xE0);
                    }
                    keyboard_post_scancode(sc);
                }
            }
        }
    }

    for (int i = 0; i < 6; i++) {
        hid->prev_keys[i] = report[2 + i];
    }
}

static void handle_usb_mouse_report(XhciHidDevice *hid, const uint8_t *buf, uint32_t actual_len) {
    if (!hid || !buf || actual_len < 3) return;

    const uint8_t *p = buf;
    uint32_t len = actual_len;

    if (hid->has_report_id && len >= 4) {
        p++;
        len--;
    }

    uint8_t btns = p[0] & 0x07;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t dz = 0;

    if (hid->xy_16bit && len >= 5) {
        int16_t raw_x = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        int16_t raw_y = (int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
        dx = (int32_t)raw_x;
        dy = (int32_t)raw_y;
        if (len >= 6) {
            dz = (int32_t)(int8_t)p[5];
        }
    } else {
        dx = (int32_t)(int8_t)p[1];
        dy = (int32_t)(int8_t)p[2];
        if (len >= 4) {
            dz = (int32_t)(int8_t)p[3];
        }
    }

    mice_post_event(dx, dy, dz, btns);
}

static void hid_requeue_in_trb(XhciDriver *d, XhciHidDevice *hid) {
    uint32_t q = hid->enqueue;
    memset(hid->report_buf, 0, sizeof(hid->report_buf));
    hid->ring[q].param = (uint64_t)(uintptr_t)hid->report_buf;
    hid->ring[q].status = hid->max_packet;
    hid->ring[q].control = (TRB_NORMAL << 10) | (1U << 5) /* IOC */ | (1U << 2) /* ISP */ | hid->cycle;
    hid->enqueue++;
    if (hid->enqueue == 63) {
        hid->ring[63].param = (uint64_t)(uintptr_t)&hid->ring[0];
        hid->ring[63].status = 0;
        hid->ring[63].control = (TRB_LINK << 10) | (1U << 1) /* TC */ | hid->cycle;
        hid->enqueue = 0;
        hid->cycle ^= 1;
    }

    __asm__ volatile ("mfence" ::: "memory");
    volatile uint32_t *db = (volatile uint32_t *)(d->db_base + hid->slot_id * 4);
    *db = hid->dci;
}

/* Send a command on the xHCI Command Ring */
static int xhci_send_command(XhciDriver *d, uint64_t param, uint32_t cmd_type, uint32_t slot_id, XhciTrb *out_evt) {
    uint32_t idx = d->cmd_enqueue;
    d->cmd_ring[idx].param = param;
    d->cmd_ring[idx].status = 0;
    d->cmd_ring[idx].control = (cmd_type << 10) | (slot_id << 24) | d->cmd_cycle;

    d->cmd_enqueue++;
    if (d->cmd_enqueue == 63) {
        d->cmd_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */ | d->cmd_cycle;
        d->cmd_enqueue = 0;
        d->cmd_cycle ^= 1;
    }

    __asm__ volatile ("mfence" ::: "memory");

    /* Ring Doorbell 0 */
    volatile uint32_t *db = (volatile uint32_t *)d->db_base;
    db[0] = 0;

    /* Wait for Command Completion Event */
    int timeout = 5000000;
    while (--timeout > 0) {
        __asm__ volatile ("lfence" ::: "memory");
        uint32_t e_idx = d->event_dequeue;
        uint32_t ctrl = d->event_ring[e_idx].control;
        if ((ctrl & 1) == d->event_cycle) {
            uint32_t type = (ctrl >> 10) & 0x3F;
            uint32_t evt_status = d->event_ring[e_idx].status;

            d->event_dequeue = (e_idx + 1) % 64;
            if (d->event_dequeue == 0) {
                d->event_cycle ^= 1;
            }

            volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
            volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
            uint64_t erdp_val = (uint64_t)(uintptr_t)&d->event_ring[d->event_dequeue] | (1U << 3);
            *erdp_low = (uint32_t)erdp_val;
            *erdp_high = (uint32_t)(erdp_val >> 32);

            if (type == TRB_CMD_COMPLETION) {
                if (out_evt) *out_evt = d->event_ring[e_idx];
                uint32_t code = (evt_status >> 24) & 0xFF;
                return (code == 1) ? 0 : -1;
            } else if (type == TRB_TRANSFER_EVENT) {
                uint32_t code = (evt_status >> 24) & 0xFF;
                uint32_t residual = evt_status & 0xFFFFFF;
                uint8_t ev_slot = (ctrl >> 24) & 0xFF;
                uint8_t ep_id = (ctrl >> 16) & 0x1F;
                for (int h = 0; h < g_hid_dev_count; h++) {
                    XhciHidDevice *hid = &g_hid_devices[h];
                    if (hid->active && hid->ctrl == d && hid->slot_id == ev_slot && hid->dci == ep_id) {
                        if (code == 1 || code == 13) {
                            uint32_t actual_len = (hid->max_packet > residual) ? (hid->max_packet - residual) : hid->max_packet;
                            if (hid->type == 2) {
                                handle_usb_mouse_report(hid, hid->report_buf, actual_len);
                            } else if (hid->type == 1) {
                                handle_usb_keyboard_report(hid, hid->report_buf);
                            }
                        }
                        hid_requeue_in_trb(d, hid);
                        break;
                    }
                }
            }
        }
        io_wait();
    }
    return -1;
}

/* Wait for Transfer Event on an Endpoint Transfer Ring */
static int xhci_wait_transfer_event_ex(XhciDriver *d, uint64_t target_trb_phys) {
    int timeout = 5000000;
    while (--timeout > 0) {
        __asm__ volatile ("lfence" ::: "memory");
        uint32_t e_idx = d->event_dequeue;
        uint32_t ctrl = d->event_ring[e_idx].control;
        if ((ctrl & 1) == d->event_cycle) {
            uint32_t type = (ctrl >> 10) & 0x3F;
            uint64_t evt_param = d->event_ring[e_idx].param;
            uint32_t evt_status = d->event_ring[e_idx].status;

            d->event_dequeue = (e_idx + 1) % 64;
            if (d->event_dequeue == 0) d->event_cycle ^= 1;

            volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
            volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
            uint64_t erdp_val = (uint64_t)(uintptr_t)&d->event_ring[d->event_dequeue] | (1U << 3);
            *erdp_low = (uint32_t)erdp_val;
            *erdp_high = (uint32_t)(erdp_val >> 32);

            if (type == TRB_TRANSFER_EVENT) {
                uint32_t code = (evt_status >> 24) & 0xFF;
                uint32_t residual = evt_status & 0xFFFFFF;
                uint8_t slot = (ctrl >> 24) & 0xFF;
                uint8_t ep_id = (ctrl >> 16) & 0x1F;

                int is_hid = 0;
                for (int h = 0; h < g_hid_dev_count; h++) {
                    XhciHidDevice *hid = &g_hid_devices[h];
                    if (hid->active && hid->ctrl == d && hid->slot_id == slot && hid->dci == ep_id) {
                        is_hid = 1;
                        if (code == 1 || code == 13) {
                            uint32_t actual_len = (hid->max_packet > residual) ? (hid->max_packet - residual) : hid->max_packet;
                            if (hid->type == 2) {
                                handle_usb_mouse_report(hid, hid->report_buf, actual_len);
                            } else if (hid->type == 1) {
                                handle_usb_keyboard_report(hid, hid->report_buf);
                            }
                        }
                        hid_requeue_in_trb(d, hid);
                        break;
                    }
                }
                if (is_hid) continue;

                /* If waiting for a specific Status Stage TRB and a Data Stage Short Packet event arrived first, keep waiting */
                if (target_trb_phys != 0 && evt_param != target_trb_phys && (code == 1 || code == 13)) {
                    continue;
                }

                return (code == 1 || code == 13) ? 0 : -1;
            }
        }
        io_wait();
    }
    return -1;
}

static int xhci_wait_transfer_event(XhciDriver *d) {
    return xhci_wait_transfer_event_ex(d, 0);
}

static void ep0_advance_ring(XhciDriver *d, uint8_t slot, uint32_t *s_idx, uint32_t *cyc) {
    XhciTrb *ring = d->ep0_ring[slot];
    (*s_idx)++;
    if (*s_idx == 63) {
        ring[63].param = (uint64_t)(uintptr_t)&ring[0];
        ring[63].status = 0;
        ring[63].control = (TRB_LINK << 10) | (1U << 1) /* TC */ | (*cyc);
        *s_idx = 0;
        *cyc ^= 1;
    }
}

/* Send a USB Control Request on EP0 */
static int xhci_ep0_transfer(XhciDriver *d, uint8_t req_type, uint8_t request, uint16_t val, uint16_t idx, uint16_t len, void *data, int is_in) {
    uint8_t slot = d->slot_id;
    if (slot >= MAX_XHCI_SLOTS) return -1;

    uint32_t s_idx = d->ep0_enqueue[slot];
    XhciTrb *ring = d->ep0_ring[slot];
    uint32_t cyc = d->ep0_cycle[slot];

    /* 1. Setup Stage TRB */
    ring[s_idx].param = ((uint64_t)request << 8) | req_type | ((uint64_t)val << 16) | ((uint64_t)idx << 32) | ((uint64_t)len << 48);
    ring[s_idx].status = 8;
    uint32_t trt = (len == 0) ? 0 : (is_in ? 3 : 2);
    ring[s_idx].control = (TRB_SETUP_STAGE << 10) | (trt << 16) | (1U << 6) /* IDT */ | cyc;
    ep0_advance_ring(d, slot, &s_idx, &cyc);

    /* 2. Data Stage TRB (if len > 0) */
    if (len > 0) {
        ring[s_idx].param = (uint64_t)(uintptr_t)data;
        ring[s_idx].status = len;
        ring[s_idx].control = (TRB_DATA_STAGE << 10) | (is_in ? (1U << 16) : 0) | cyc;
        ep0_advance_ring(d, slot, &s_idx, &cyc);
    }

    /* 3. Status Stage TRB */
    uint64_t status_trb_phys = (uint64_t)(uintptr_t)&ring[s_idx];
    ring[s_idx].param = 0;
    ring[s_idx].status = 0;
    ring[s_idx].control = (TRB_STATUS_STAGE << 10) | ((len > 0 && is_in) ? 0 : (1U << 16)) | (1U << 5) /* IOC */ | cyc;
    ep0_advance_ring(d, slot, &s_idx, &cyc);

    d->ep0_enqueue[slot] = s_idx;
    d->ep0_cycle[slot] = cyc;

    __asm__ volatile ("mfence" ::: "memory");

    /* Ring Doorbell for Slot ID, Target = 1 (EP0) */
    volatile uint32_t *db = (volatile uint32_t *)(d->db_base + slot * 4);
    *db = 1;

    return xhci_wait_transfer_event_ex(d, status_trb_phys);
}

void usb_hid_poll(void) {
    if (!g_xhci_active || g_hid_dev_count == 0) return;
    static volatile int g_hid_polling = 0;
    if (g_hid_polling) return;
    g_hid_polling = 1;

    for (int c = 0; c < g_xhci_ctrl_count; c++) {
        XhciDriver *d = &g_xhci_ctrls[c];
        if (!d->active) continue;

        while (1) {
            __asm__ volatile ("lfence" ::: "memory");
            uint32_t e_idx = d->event_dequeue;
            uint32_t ctrl = d->event_ring[e_idx].control;
            if ((ctrl & 1) != d->event_cycle) break;

            uint32_t type = (ctrl >> 10) & 0x3F;
            uint32_t evt_status = d->event_ring[e_idx].status;
            uint32_t code = (evt_status >> 24) & 0xFF;
            uint32_t residual = evt_status & 0xFFFFFF;
            uint8_t slot = (ctrl >> 24) & 0xFF;
            uint8_t ep_id = (ctrl >> 16) & 0x1F;

            if (type == TRB_TRANSFER_EVENT) {
                int handled = 0;
                for (int h = 0; h < g_hid_dev_count; h++) {
                    XhciHidDevice *hid = &g_hid_devices[h];
                    if (hid->active && hid->ctrl == d && hid->slot_id == slot && hid->dci == ep_id) {
                        handled = 1;
                        if (code == 1 || code == 13) {
                            uint32_t actual_len = (hid->max_packet > residual) ? (hid->max_packet - residual) : hid->max_packet;
                            if (hid->type == 2) {
                                handle_usb_mouse_report(hid, hid->report_buf, actual_len);
                            } else if (hid->type == 1) {
                                handle_usb_keyboard_report(hid, hid->report_buf);
                            }
                        }
                        hid_requeue_in_trb(d, hid);
                        break;
                    }
                }
                if (!handled) {
                    /* Not for HID, don't consume here; leave for xhci_wait_transfer_event */
                    break;
                }
            } else if (type == 34 /* Port Status Change Event */) {
                /* Consume asynchronous port status change events so they never block HID transfer events */
            } else {
                /* Command completion event, leave on ring */
                break;
            }

            d->event_dequeue = (e_idx + 1) % 64;
            if (d->event_dequeue == 0) d->event_cycle ^= 1;

            volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
            volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
            uint64_t erdp_val = (uint64_t)(uintptr_t)&d->event_ring[d->event_dequeue] | (1U << 3);
            *erdp_low = (uint32_t)erdp_val;
            *erdp_high = (uint32_t)(erdp_val >> 32);
        }
    }

    g_hid_polling = 0;
}

/* Execute SCSI Bulk-Only Transport (BOT) Command */
static int xhci_bot_exec(XhciDriver *d, const void *cdb, uint8_t cdb_len, void *buf, uint32_t buf_len, int is_read) {
    if (!d || !d->active) return -1;

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
    d->out_enqueue++;
    if (d->out_enqueue == 63) {
        d->bulk_out_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */ | d->out_cycle;
        d->out_enqueue = 0;
        d->out_cycle ^= 1;
    }

    __asm__ volatile ("mfence" ::: "memory");
    volatile uint32_t *db_out = (volatile uint32_t *)(d->db_base + d->storage_slot_id * 4);
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
            d->in_enqueue++;
            if (d->in_enqueue == 63) {
                d->bulk_in_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */ | d->in_cycle;
                d->in_enqueue = 0;
                d->in_cycle ^= 1;
            }

            __asm__ volatile ("mfence" ::: "memory");
            volatile uint32_t *db_in = (volatile uint32_t *)(d->db_base + d->storage_slot_id * 4);
            *db_in = d->bulk_in_dci;

            if (xhci_wait_transfer_event(d) != 0) {
                return -1;
            }
        } else {
            uint32_t o_idx = d->out_enqueue;
            d->bulk_out_ring[o_idx].param = (uint64_t)(uintptr_t)buf;
            d->bulk_out_ring[o_idx].status = buf_len;
            d->bulk_out_ring[o_idx].control = (TRB_NORMAL << 10) | (1 << 5) /* IOC */ | d->out_cycle;
            d->out_enqueue++;
            if (d->out_enqueue == 63) {
                d->bulk_out_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */ | d->out_cycle;
                d->out_enqueue = 0;
                d->out_cycle ^= 1;
            }

            __asm__ volatile ("mfence" ::: "memory");
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
    d->in_enqueue++;
    if (d->in_enqueue == 63) {
        d->bulk_in_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */ | d->in_cycle;
        d->in_enqueue = 0;
        d->in_cycle ^= 1;
    }

    __asm__ volatile ("mfence" ::: "memory");
    volatile uint32_t *db_in = (volatile uint32_t *)(d->db_base + d->storage_slot_id * 4);
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
        uint16_t chunk = remaining > 128 ? 128 : (uint16_t)remaining;
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
        uint16_t chunk = remaining > 128 ? 128 : (uint16_t)remaining;
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

/* SCSI SYNCHRONIZE CACHE (10) Command (0x35) */
static int usb_flush_cache_impl(StorageDevice *dev) {
    if (!dev || !dev->driver_priv) return -1;
    XhciDriver *d = (XhciDriver *)dev->driver_priv;

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35; /* SYNCHRONIZE CACHE (10) */
    return xhci_bot_exec(d, cdb, 10, NULL, 0, 0);
}

/* SCSI START STOP UNIT Command (0x1B) */
static void usb_shutdown_impl(StorageDevice *dev) {
    if (!dev || !dev->driver_priv) return;
    XhciDriver *d = (XhciDriver *)dev->driver_priv;

    /* 1. Flush volatile caches */
    usb_flush_cache_impl(dev);

    /* 2. Issue START STOP UNIT: START=0 to spin down / park */
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1B; /* START STOP UNIT */
    cdb[1] = 0x00; /* Immediate */
    cdb[4] = 0x00; /* START=0 (stop unit) */
    xhci_bot_exec(d, cdb, 6, NULL, 0, 0);
}

static void probe_xhci_controller(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base) {
    if (g_xhci_ctrl_count >= MAX_XHCI_CTRLS) return;

    /* Map BAR0 physical pages into kernel virtual address space (1 MB to cover large rtsoff/dboff on AMD FCH) */
    uint64_t bar0_phys = mmio_base & ~0xFFFULL;
    uint64_t bar0_virt = KERNEL_VIRTUAL_BASE + bar0_phys;
    size_t num_pages = 256; /* 1 MB xHCI MMIO space */
    vmm_map_pages(vmm_get_kernel_pml4(), bar0_virt, bar0_phys, num_pages, PTE_WRITABLE | PTE_PCD | PTE_PWT);
    vmm_map_pages(vmm_get_kernel_pml4(), bar0_phys, bar0_phys, num_pages, PTE_WRITABLE | PTE_PCD | PTE_PWT);
    mmio_base = bar0_virt + (mmio_base & 0xFFFULL);

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)mmio_base;
    uint8_t caplength = cap[0];
    if (caplength == 0 || caplength > 0x80) caplength = 0x20;

    uint32_t hccparams1 = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_DBOFF) & ~0x3;
    uint32_t rtsoff = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_RTSOFF) & ~0x1F;

    /* xHCI BIOS/OS Handoff (Extended Capability ID 1 = USBLEGSUP) */
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;
    if (xecp != 0) {
        uintptr_t ext_off = (uintptr_t)xecp << 2;
        for (int cap_iter = 0; cap_iter < 32 && ext_off != 0 && ext_off < 0x10000; cap_iter++) {
            volatile uint32_t *ext_cap = (volatile uint32_t *)(mmio_base + ext_off);
            uint32_t cap_val = *ext_cap;
            if (cap_val == 0xFFFFFFFF) break;
            uint8_t cap_id = cap_val & 0xFF;
            uint8_t next_ptr = (cap_val >> 8) & 0xFF;
            if (cap_id == 1) { /* USB Legacy Support Capability */
                if (cap_val & (1U << 16)) { /* HC BIOS Owned Semaphore */
                    *ext_cap = cap_val | (1U << 24); /* Set HC OS Owned Semaphore */
                    int ho_wait = 200000;
                    while ((*ext_cap & (1U << 16)) && --ho_wait > 0) {
                        io_wait();
                    }
                }
                volatile uint32_t *legctlsts = (volatile uint32_t *)(mmio_base + ext_off + 4);
                *legctlsts &= ~0x0000FFFFU; /* Disable Legacy SMIs */
                *legctlsts |= 0xE0000000U;  /* Clear RWC status bits */
                break;
            }
            if (next_ptr == 0) break;
            ext_off += ((uintptr_t)next_ptr << 2);
        }
    }

    XhciDriver *d = &g_xhci_ctrls[g_xhci_ctrl_count];
    memset(d, 0, sizeof(XhciDriver));
    d->cmd_ring = d->cmd_ring_buf;
    d->event_ring = d->event_ring_buf;
    d->bulk_in_ring = d->bulk_in_ring_buf;
    d->bulk_out_ring = d->bulk_out_ring_buf;
    d->mmio_base = mmio_base;
    d->op_base = mmio_base + caplength;
    d->rt_base = mmio_base + rtsoff;
    d->db_base = mmio_base + dboff;
    d->context_size = (hccparams1 & (1U << 2)) ? 64 : 32;

    /* Halt and Reset Controller (with proper io_wait timing for real silicon) */
    volatile uint32_t *usbcmd = (volatile uint32_t *)(d->op_base + XHCI_OP_USBCMD);
    volatile uint32_t *usbsts = (volatile uint32_t *)(d->op_base + XHCI_OP_USBSTS);

    *usbcmd &= ~USBCMD_RS;
    int timeout = 200000;
    while (!(*usbsts & USBSTS_HCH) && --timeout > 0) io_wait();

    *usbcmd |= USBCMD_HCRST;
    timeout = 500000;
    while ((*usbcmd & USBCMD_HCRST) && --timeout > 0) io_wait();

    /* Wait for Controller Not Ready (CNR, bit 11) to clear per xHCI spec 4.2.1 */
    timeout = 500000;
    while ((*usbsts & (1U << 11)) && --timeout > 0) io_wait();

    /* Initialize Ring Pointers & Cycle States */
    d->cmd_ring[63].param = (uint64_t)(uintptr_t)&d->cmd_ring[0];
    d->cmd_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */;

    d->cmd_enqueue = 0;
    d->cmd_cycle = 1;
    d->event_dequeue = 0;
    d->event_cycle = 1;

    /* Program MaxSlotsEn */
    uint32_t hcsparams1 = *(volatile uint32_t *)(uintptr_t)(mmio_base + XHCI_CAP_HCSPARAMS1);
    uint8_t max_slots_cap = hcsparams1 & 0xFF;
    uint8_t max_slots_en = (max_slots_cap > MAX_XHCI_SLOTS - 1) ? (MAX_XHCI_SLOTS - 1) : max_slots_cap;
    if (max_slots_en == 0) max_slots_en = 1;

    volatile uint32_t *config = (volatile uint32_t *)(d->op_base + XHCI_OP_CONFIG);
    *config = max_slots_en;

    /* Program DCBAAP */
    volatile uint32_t *dcbaap_low = (volatile uint32_t *)(d->op_base + XHCI_OP_DCBAAP);
    volatile uint32_t *dcbaap_high = (volatile uint32_t *)(d->op_base + XHCI_OP_DCBAAP + 4);
    uint64_t dcbaa_phys = (uint64_t)(uintptr_t)d->dcbaa;
    *dcbaap_low = (uint32_t)dcbaa_phys;
    *dcbaap_high = (uint32_t)(dcbaa_phys >> 32);

    /* Program Command Ring Control Register (CRCR) */
    volatile uint32_t *crcr_low = (volatile uint32_t *)(d->op_base + XHCI_OP_CRCR);
    volatile uint32_t *crcr_high = (volatile uint32_t *)(d->op_base + XHCI_OP_CRCR + 4);
    uint64_t cr_phys = (uint64_t)(uintptr_t)d->cmd_ring | 1U /* RCS */;
    *crcr_low = (uint32_t)cr_phys;
    *crcr_high = (uint32_t)(cr_phys >> 32);

    /* Program Event Ring Segment Table (ERST) */
    d->erst[0].ring_address = (uint64_t)(uintptr_t)d->event_ring;
    d->erst[0].ring_size = 64;
    d->erst[0].rsvd = 0;

    volatile uint32_t *erstsz = (volatile uint32_t *)(d->rt_base + 0x20 + 0x08);
    *erstsz = 1;

    volatile uint32_t *erstba_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x10);
    volatile uint32_t *erstba_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x14);
    uint64_t erst_phys = (uint64_t)(uintptr_t)d->erst;
    *erstba_low = (uint32_t)erst_phys;
    *erstba_high = (uint32_t)(erst_phys >> 32);

    volatile uint32_t *erdp_low = (volatile uint32_t *)(d->rt_base + 0x20 + 0x18);
    volatile uint32_t *erdp_high = (volatile uint32_t *)(d->rt_base + 0x20 + 0x1C);
    uint64_t erdp_phys = (uint64_t)(uintptr_t)d->event_ring;
    *erdp_low = (uint32_t)erdp_phys;
    *erdp_high = (uint32_t)(erdp_phys >> 32);

    volatile uint32_t *iman = (volatile uint32_t *)(d->rt_base + 0x20 + 0x00);
    *iman &= ~(1U << 1); /* Polling mode: keep hardware interrupt line disabled */

    __asm__ volatile ("mfence" ::: "memory");

    /* Run Controller */
    *usbcmd |= USBCMD_RS;
    timeout = 200000;
    while ((*usbsts & USBSTS_HCH) && --timeout > 0) io_wait();

    /* Check Root Hub Ports for Connected USB Devices */
    uint8_t max_ports = (hcsparams1 >> 24) & 0xFF;
    if (max_ports == 0) max_ports = 4;
    else if (max_ports > 64) max_ports = 64;

    klog_info("xHCI controller initialized: mmio=0x%llx (virt=0x%llx), max_ports=%u",
              (unsigned long long)bar0_phys, (unsigned long long)mmio_base, max_ports);

    uintptr_t port_reg_base = d->op_base + XHCI_OP_PORTSC_BASE;

    /* Ensure Port Power (PP, bit 9) is enabled on all root ports */
    for (uint8_t p = 0; p < max_ports; p++) {
        volatile uint32_t *portsc = (volatile uint32_t *)(port_reg_base + p * 0x10);
        uint32_t pval = *portsc;
        if (!(pval & (1U << 9))) {
            pval &= ~((1U << 1) | (0x7FU << 17)); /* Clear RWC bits to prevent unintended clear */
            pval |= (1U << 9);                    /* Set Port Power (PP) */
            *portsc = pval;
        }
    }

    /* Wait ~250ms for port power stabilization and Low/Full-Speed USB 2.0 attach debounce (TATTDB) */
    for (int ms = 0; ms < 250; ms++) {
        for (int j = 0; j < 1000; j++) {
            io_wait();
        }
    }

    for (uint8_t p = 0; p < max_ports; p++) {
        volatile uint32_t *portsc = (volatile uint32_t *)(port_reg_base + p * 0x10);
        uint32_t port_val = *portsc;

        if ((port_val & PORTSC_CCS) || p < 8) {
            klog_info("xHCI port %u: PORTSC=0x%08x (CCS=%u, PED=%u, PP=%u)",
                      p, port_val, (port_val & PORTSC_CCS) ? 1 : 0, (port_val & PORTSC_PED) ? 1 : 0, (port_val & (1U << 9)) ? 1 : 0);
        }

        if (port_val & PORTSC_CCS) {
            klog_info("xHCI port %u: device connected, resetting...", p);

            /* Reset Port: preserve Port Power (bit 9), clear RWC change bits, set Port Reset (bit 4) */
            uint32_t pval = *portsc;
            pval &= ~((1U << 1) | (0x7FU << 17)); /* Clear RWC bits (PED and Change bits) */
            pval |= (1U << 9) | PORTSC_PR;         /* Preserve PP (Port Power), trigger PR (Port Reset) */
            *portsc = pval;

            timeout = 50000;
            while ((*portsc & PORTSC_PR) && --timeout > 0) {
                for (int dly = 0; dly < 100; dly++) io_wait();
            }

            /* Wait for Port Enabled */
            timeout = 50000;
            while (!(*portsc & PORTSC_PED) && --timeout > 0) {
                for (int dly = 0; dly < 100; dly++) io_wait();
            }

            /* Clear Port Reset Change (PRC, bit 21) and wait 20ms USB TRSTRCY recovery time */
            uint32_t clear_prc = *portsc;
            clear_prc &= ~((1U << 1) | (0x7FU << 17));
            clear_prc |= (1U << 9) | (1U << 21);
            *portsc = clear_prc;
            for (int ms = 0; ms < 20; ms++) {
                for (int j = 0; j < 1000; j++) io_wait();
            }

            uint32_t final_portsc = *portsc;
            uint8_t speed = (final_portsc >> 10) & 0x0F;
            if (!(final_portsc & PORTSC_PED)) {
                klog_warn("xHCI port %u: reset failed (port not enabled)", p);
                continue;
            }

            /* Per USB 2.0 / xHCI 4.3: start Full-Speed (1) and Low-Speed (2) with 8-byte EP0 packet size */
            uint32_t ep0_max_packet = 64;
            if (speed == 4 || speed == 5) ep0_max_packet = 512;
            else if (speed == 1 || speed == 2) ep0_max_packet = 8;
            else ep0_max_packet = 64;

            /* 1. ENABLE_SLOT Command */
            XhciTrb comp_evt;
            memset(&comp_evt, 0, sizeof(comp_evt));
            int cmd_res = xhci_send_command(d, 0, TRB_ENABLE_SLOT_CMD, 0, &comp_evt);
            uint8_t slot_id = (comp_evt.control >> 24) & 0xFF;
            if (cmd_res != 0 || slot_id == 0 || slot_id >= MAX_XHCI_SLOTS) {
                klog_warn("xHCI port %u: ENABLE_SLOT failed (res=%d, slot=%u)", p, cmd_res, slot_id);
                continue;
            }
            d->slot_id = slot_id;

            /* 2. ADDRESS_DEVICE Command */
            memset(g_input_ctx, 0, sizeof(g_input_ctx));
            memset(d->output_ctx[slot_id], 0, sizeof(d->output_ctx[slot_id]));
            memset(d->ep0_ring[slot_id], 0, sizeof(d->ep0_ring[slot_id]));
            d->ep0_ring[slot_id][63].param = (uint64_t)(uintptr_t)&d->ep0_ring[slot_id][0];
            d->ep0_ring[slot_id][63].control = (TRB_LINK << 10) | (1U << 1) /* TC */;

            d->ep0_enqueue[slot_id] = 0;
            d->ep0_cycle[slot_id] = 1;

            uint32_t csz = d->context_size;

            /* Input Control Context: Add Slot (bit 0) and EP0 (bit 1) */
            uint32_t *icc = (uint32_t *)g_input_ctx;
            icc[1] = (1U << 0) | (1U << 1);

            /* Slot Context at offset csz */
            uint32_t *slot_ctx = (uint32_t *)(g_input_ctx + csz);
            slot_ctx[0] = (speed << 20) | (1U << 27); /* 1 Context entry */
            slot_ctx[1] = ((uint32_t)(p + 1) << 16);

            /* EP0 Context at offset 2 * csz (with mandatory CErr=3 for real xHCI silicon) */
            uint32_t *ep0_ctx = (uint32_t *)(g_input_ctx + 2 * csz);
            ep0_ctx[1] = (3U << 1) /* CErr=3 */ | (4U << 3) /* Control EP */ | (ep0_max_packet << 16);
            uint64_t ep0_phys = (uint64_t)(uintptr_t)d->ep0_ring[slot_id] | 1U;
            ep0_ctx[2] = (uint32_t)ep0_phys;
            ep0_ctx[3] = (uint32_t)(ep0_phys >> 32);
            ep0_ctx[4] = 8; /* Avg TRB length */

            d->dcbaa[slot_id] = (uint64_t)(uintptr_t)d->output_ctx[slot_id];

            int addr_res = xhci_send_command(d, (uint64_t)(uintptr_t)g_input_ctx, TRB_ADDRESS_DEV_CMD, slot_id, NULL);
            if (addr_res != 0) {
                klog_warn("xHCI port %u: ADDRESS_DEV failed (res=%d)", p, addr_res);
                continue;
            }

            /* Wait 5ms SetAddress recovery interval (USB 2.0 spec 9.2.6.3) */
            for (int ms = 0; ms < 5; ms++) {
                for (int j = 0; j < 1000; j++) io_wait();
            }

            /* 2b. Query first 8 bytes of Device Descriptor (0x0100) to discover real bMaxPacketSize0 */
            memset(g_usb_dma_buf, 0, 64);
            if (xhci_ep0_transfer(d, 0x80, 0x06, 0x0100, 0, 8, g_usb_dma_buf, 1) == 0) {
                uint8_t bMaxPacketSize0 = g_usb_dma_buf[7];
                uint32_t new_mps = ep0_max_packet;
                if (speed == 4 || speed == 5) {
                    new_mps = 512;
                } else if (bMaxPacketSize0 == 8 || bMaxPacketSize0 == 16 || bMaxPacketSize0 == 32 || bMaxPacketSize0 == 64) {
                    new_mps = bMaxPacketSize0;
                }
                if (new_mps != ep0_max_packet) {
                    ep0_max_packet = new_mps;
                    memset(g_input_ctx, 0, sizeof(g_input_ctx));
                    icc = (uint32_t *)g_input_ctx;
                    icc[1] = (1U << 1); /* Evaluate EP0 Context */
                    ep0_ctx = (uint32_t *)(g_input_ctx + 2 * csz);
                    ep0_ctx[1] = (3U << 1) | (4U << 3) | (ep0_max_packet << 16);
                    xhci_send_command(d, (uint64_t)(uintptr_t)g_input_ctx, TRB_EVALUATE_CTX_CMD, slot_id, NULL);
                }
            }

            /* Read full 18-byte Device Descriptor for logging VID:PID */
            uint16_t usb_vid = 0, usb_pid = 0;
            uint8_t dev_class = 0;
            memset(g_usb_dma_buf, 0, 64);
            if (xhci_ep0_transfer(d, 0x80, 0x06, 0x0100, 0, 18, g_usb_dma_buf, 1) == 0) {
                dev_class = g_usb_dma_buf[4];
                usb_vid = *(uint16_t *)&g_usb_dma_buf[8];
                usb_pid = *(uint16_t *)&g_usb_dma_buf[10];
                klog_info("xHCI port %u: USB device VID=%04x PID=%04x class=0x%02x speed=%u ep0_mps=%u",
                          p, usb_vid, usb_pid, dev_class, speed, ep0_max_packet);
            }

            /* 3. GET_DESCRIPTOR: 2-step Configuration Descriptor fetch (avoids short-packet desync) */
            memset(g_usb_dma_buf, 0, 512);
            int desc_res = xhci_ep0_transfer(d, 0x80, 0x06, 0x0200, 0, 9, g_usb_dma_buf, 1);
            if (desc_res != 0) {
                klog_warn("xHCI port %u: GET_DESCRIPTOR(Config 9B) failed (res=%d)", p, desc_res);
                continue;
            }

            uint8_t *desc = g_usb_dma_buf;
            uint16_t total_desc_len = *(uint16_t *)&desc[2];
            uint8_t config_val = desc[5] != 0 ? desc[5] : 1;
            if (total_desc_len < 9) total_desc_len = 9;
            if (total_desc_len > 512) total_desc_len = 512;

            if (total_desc_len > 9) {
                memset(g_usb_dma_buf, 0, 512);
                desc_res = xhci_ep0_transfer(d, 0x80, 0x06, 0x0200, 0, total_desc_len, g_usb_dma_buf, 1);
                if (desc_res != 0) {
                    klog_warn("xHCI port %u: GET_DESCRIPTOR(Config %uB) failed (res=%d)", p, total_desc_len, desc_res);
                    continue;
                }
            }

            uint8_t bulk_in_ep = 0;
            uint8_t bulk_out_ep = 0;
            uint16_t bulk_max_packet = (speed == 4) ? 1024 : 512;
            int is_mass_storage = 0;

            typedef struct {
                uint8_t  if_num;
                uint8_t  if_subclass;
                uint8_t  if_proto;
                uint8_t  hid_type; /* 1 = keyboard, 2 = mouse */
                uint8_t  in_ep;
                uint16_t max_packet;
                uint8_t  interval;
                uint16_t report_desc_len;
            } HidIfCand;

            HidIfCand hid_cands[4];
            int hid_cand_count = 0;
            int cur_cand_idx = -1;
            int cur_if_class = 0;

            uint16_t off = desc[0];
            while (off + 2 <= total_desc_len) {
                uint8_t len = desc[off];
                uint8_t type = desc[off + 1];
                if (len == 0) break;

                if (type == 0x04 && len >= 9) { /* Interface Descriptor */
                    uint8_t if_num = desc[off + 2];
                    uint8_t alt_setting = desc[off + 3];
                    cur_if_class = desc[off + 5];
                    uint8_t if_subclass = desc[off + 6];
                    uint8_t if_proto = desc[off + 7];
                    cur_cand_idx = -1;

                    if (alt_setting == 0) {
                        if (cur_if_class == 0x08) {
                            is_mass_storage = 1;
                        } else if (cur_if_class == 0x03 && hid_cand_count < 4) {
                            cur_cand_idx = hid_cand_count++;
                            memset(&hid_cands[cur_cand_idx], 0, sizeof(HidIfCand));
                            hid_cands[cur_cand_idx].if_num = if_num;
                            hid_cands[cur_cand_idx].if_subclass = if_subclass;
                            hid_cands[cur_cand_idx].if_proto = if_proto;
                            hid_cands[cur_cand_idx].max_packet = 8;
                            hid_cands[cur_cand_idx].interval = 6;
                            if (if_proto == 0x01) {
                                hid_cands[cur_cand_idx].hid_type = 1; /* Keyboard */
                            } else {
                                hid_cands[cur_cand_idx].hid_type = 2; /* Mouse / Generic HID Pointer */
                            }
                        }
                    }
                } else if (type == 0x21 && len >= 9 && cur_cand_idx >= 0) { /* HID Descriptor */
                    hid_cands[cur_cand_idx].report_desc_len = desc[off + 7] | ((uint16_t)desc[off + 8] << 8);
                } else if (type == 0x05 && len >= 7) { /* Endpoint Descriptor */
                    uint8_t ep_addr = desc[off + 2];
                    uint8_t ep_attr = desc[off + 3];
                    if (is_mass_storage && cur_if_class == 0x08 && (ep_attr & 0x03) == 0x02) { /* Bulk Endpoint */
                        if (ep_addr & 0x80) {
                            bulk_in_ep = ep_addr & 0x0F;
                        } else {
                            bulk_out_ep = ep_addr & 0x0F;
                        }
                    } else if (cur_cand_idx >= 0 && ((ep_attr & 0x03) == 0x03) && (ep_addr & 0x80)) { /* Interrupt IN */
                        if (hid_cands[cur_cand_idx].in_ep == 0) {
                            hid_cands[cur_cand_idx].in_ep = ep_addr & 0x0F;
                            hid_cands[cur_cand_idx].max_packet = (desc[off + 4] | ((uint16_t)desc[off + 5] << 8)) & 0x07FF;
                            hid_cands[cur_cand_idx].interval = desc[off + 6];
                        }
                    }
                }
                off += len;
            }

            /* Handle USB HID Device(s) (Mouse, Touchpad, Wireless Receiver, Keyboard) */
            if (hid_cand_count > 0 && !is_mass_storage) {
                /* 1. SET_CONFIGURATION first */
                if (xhci_ep0_transfer(d, 0x00, 0x09, config_val, 0, 0, NULL, 0) != 0) {
                    klog_warn("xHCI port %u: HID SET_CONFIGURATION(%u) failed", p, config_val);
                    continue;
                }

                /* 2. Build single CONFIGURE_ENDPOINT input context for all valid HID Interrupt IN endpoints */
                memset(g_input_ctx, 0, sizeof(g_input_ctx));
                icc = (uint32_t *)g_input_ctx;
                icc[1] = (1U << 0); /* Slot Context */
                uint32_t max_dci = 1;
                int first_hid_idx = g_hid_dev_count;
                int added_eps = 0;

                for (int c = 0; c < hid_cand_count && g_hid_dev_count < 16; c++) {
                    if (hid_cands[c].in_ep == 0) continue;
                    uint8_t hid_dci = hid_cands[c].in_ep * 2 + 1;
                    if (hid_dci >= 32) continue;

                    uint16_t hid_mps = hid_cands[c].max_packet;
                    if (hid_mps == 0 || hid_mps > 64) hid_mps = 8;

                    /* Convert USB bInterval to xHCI microframe Interval exponent (3..10) per xHCI 6.2.3.6 */
                    uint32_t xhci_interval = 6; /* Default 2^6 * 125us = 8ms (125 Hz) */
                    uint8_t raw_intv = hid_cands[c].interval;
                    if (speed == 1 || speed == 2) {
                        /* Full/Low-speed bInterval is in 1ms frames (1..255). Convert to 125us microframes (x8) */
                        uint32_t uframes = (uint32_t)(raw_intv > 0 ? raw_intv : 8) * 8U;
                        xhci_interval = 3;
                        while (xhci_interval < 10 && (1U << (xhci_interval + 1)) <= uframes) {
                            xhci_interval++;
                        }
                    } else if (raw_intv >= 1 && raw_intv <= 16) {
                        xhci_interval = (uint32_t)(raw_intv - 1);
                        if (xhci_interval < 3) xhci_interval = 3;
                        if (xhci_interval > 10) xhci_interval = 10;
                    }

                    XhciHidDevice *hid = &g_hid_devices[g_hid_dev_count++];
                    memset(hid, 0, sizeof(XhciHidDevice));
                    hid->active = 1;
                    hid->ctrl = d;
                    hid->slot_id = slot_id;
                    hid->dci = hid_dci;
                    hid->type = hid_cands[c].hid_type;
                    hid->max_packet = hid_mps;
                    hid->enqueue = 0;
                    hid->cycle = 1;
                    hid->ring[63].param = (uint64_t)(uintptr_t)&hid->ring[0];
                    hid->ring[63].control = (TRB_LINK << 10) | (1U << 1) /* TC */;

                    icc[1] |= (1U << hid_dci);
                    if (hid_dci > max_dci) max_dci = hid_dci;

                    uint32_t *ep_ctx = (uint32_t *)(g_input_ctx + (hid_dci + 1) * csz);
                    ep_ctx[0] = (xhci_interval << 16);
                    ep_ctx[1] = (3U << 1) /* CErr=3 */ | (7U << 3) /* Interrupt IN */ | ((uint32_t)hid_mps << 16);
                    uint64_t ep_phys = (uint64_t)(uintptr_t)hid->ring | 1U;
                    ep_ctx[2] = (uint32_t)ep_phys;
                    ep_ctx[3] = (uint32_t)(ep_phys >> 32);
                    ep_ctx[4] = ((uint32_t)hid_mps << 16) | hid_mps; /* Max ESIT Payload + Avg TRB Len */
                    added_eps++;
                }

                if (added_eps == 0) continue;

                slot_ctx = (uint32_t *)(g_input_ctx + csz);
                slot_ctx[0] = (speed << 20) | (max_dci << 27);
                slot_ctx[1] = ((uint32_t)(p + 1) << 16);

                if (xhci_send_command(d, (uint64_t)(uintptr_t)g_input_ctx, TRB_CONFIGURE_EP_CMD, slot_id, NULL) != 0) {
                    klog_warn("xHCI port %u: HID CONFIGURE_ENDPOINT failed", p);
                    g_hid_dev_count = first_hid_idx;
                    continue;
                }

                /* 3. Configure Boot Protocol / Idle and inspect Report Descriptor if Non-Boot */
                int h_cursor = first_hid_idx;
                for (int c = 0; c < hid_cand_count && h_cursor < g_hid_dev_count; c++) {
                    if (hid_cands[c].in_ep == 0) continue;
                    XhciHidDevice *hid = &g_hid_devices[h_cursor++];
                    uint8_t if_num = hid_cands[c].if_num;

                    int boot_ok = -1;
                    if (hid_cands[c].if_subclass == 1) {
                        /* Boot Interface Subclass: switch to standard Boot Protocol (0) */
                        boot_ok = xhci_ep0_transfer(d, 0x21, 0x0B, 0, if_num, 0, NULL, 0);
                    }
                    /* SET_IDLE (0) */
                    xhci_ep0_transfer(d, 0x21, 0x0A, 0, if_num, 0, NULL, 0);

                    /* If non-boot interface (or SET_PROTOCOL failed), inspect HID Report Descriptor */
                    if (boot_ok != 0 && hid_cands[c].report_desc_len > 0) {
                        uint16_t rlen = hid_cands[c].report_desc_len;
                        if (rlen > 256) rlen = 256;
                        memset(g_usb_dma_buf, 0, 256);
                        if (xhci_ep0_transfer(d, 0x81, 0x06, 0x2200, if_num, rlen, g_usb_dma_buf, 1) == 0) {
                            int seen_mouse_usage = 0;
                            for (uint16_t ri = 0; ri + 1 < rlen; ri++) {
                                if (g_usb_dma_buf[ri] == 0x05 && g_usb_dma_buf[ri + 1] == 0x01) {
                                    if (ri + 3 < rlen && g_usb_dma_buf[ri + 2] == 0x09 &&
                                        (g_usb_dma_buf[ri + 3] == 0x02 || g_usb_dma_buf[ri + 3] == 0x01)) {
                                        seen_mouse_usage = 1;
                                    }
                                }
                                if (g_usb_dma_buf[ri] == 0x85) { /* Report ID tag */
                                    hid->has_report_id = 1;
                                }
                                if (seen_mouse_usage && g_usb_dma_buf[ri] == 0x75 && g_usb_dma_buf[ri + 1] == 0x10) {
                                    hid->xy_16bit = 1;
                                }
                            }
                            if (seen_mouse_usage) {
                                hid->type = 2;
                            }
                        }
                    }

                    /* Enqueue initial Interrupt IN TRB */
                    hid_requeue_in_trb(d, hid);

                    klog_info("xHCI: USB %s registered on port %u (Slot %u, IF %u, DCI %u, mps=%u, rep_id=%u, 16b=%u)",
                              (hid->type == 1) ? "Keyboard" : "Mouse", p, slot_id, if_num,
                              hid->dci, hid->max_packet, hid->has_report_id, hid->xy_16bit);
                }

                d->active = 1;
                g_xhci_active = 1;
                continue;
            }

            /* Handle USB Mass Storage Device */
            if (is_mass_storage && bulk_in_ep != 0 && bulk_out_ep != 0) {
                d->storage_slot_id = slot_id;
                d->bulk_in_dci = bulk_in_ep * 2 + 1;
                d->bulk_out_dci = bulk_out_ep * 2;
                d->bulk_max_packet = bulk_max_packet;

                /* 4. CONFIGURE_ENDPOINT Command: Add Bulk IN & Bulk OUT */
                memset(g_input_ctx, 0, sizeof(g_input_ctx));
                memset(d->bulk_in_ring_buf, 0, sizeof(d->bulk_in_ring_buf));
                d->bulk_in_ring[63].param = (uint64_t)(uintptr_t)&d->bulk_in_ring[0];
                d->bulk_in_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */;

                memset(d->bulk_out_ring_buf, 0, sizeof(d->bulk_out_ring_buf));
                d->bulk_out_ring[63].param = (uint64_t)(uintptr_t)&d->bulk_out_ring[0];
                d->bulk_out_ring[63].control = (TRB_LINK << 10) | (1 << 1) /* TC */;

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
                uint64_t out_phys = (uint64_t)(uintptr_t)d->bulk_out_ring | 1U;
                out_ctx[2] = (uint32_t)out_phys;
                out_ctx[3] = (uint32_t)(out_phys >> 32);
                out_ctx[4] = 512;

                /* Bulk IN Context (DCI = bulk_in_dci) */
                uint32_t *in_ctx = (uint32_t *)(g_input_ctx + (d->bulk_in_dci + 1) * csz);
                in_ctx[1] = (6U << 3) | ((uint32_t)bulk_max_packet << 16); /* Bulk IN */
                uint64_t in_phys = (uint64_t)(uintptr_t)d->bulk_in_ring | 1U;
                in_ctx[2] = (uint32_t)in_phys;
                in_ctx[3] = (uint32_t)(in_phys >> 32);
                in_ctx[4] = 512;

                int cfg_ep_res = xhci_send_command(d, (uint64_t)(uintptr_t)g_input_ctx, TRB_CONFIGURE_EP_CMD, slot_id, NULL);
                if (cfg_ep_res != 0) {
                    klog_warn("xHCI port %u: CONFIGURE_EP failed (res=%d)", p, cfg_ep_res);
                    continue;
                }

                /* 5. SET_CONFIGURATION (1) */
                int set_cfg_res = xhci_ep0_transfer(d, 0x00, 0x09, 1, 0, 0, NULL, 0);
                if (set_cfg_res != 0) {
                    klog_warn("xHCI port %u: SET_CONFIGURATION failed (res=%d)", p, set_cfg_res);
                    continue;
                }

                d->active = 1;
                g_xhci_active = 1;

                /* 6. Execute SCSI INQUIRY (0x12) */
                char drive_name[64];
                if (scsi_inquiry(d, drive_name, sizeof(drive_name)) != 0) {
                    strcpy(drive_name, "USB Mass Storage Drive");
                }
                if (strstr(drive_name, "QEMU") != NULL) {
                    strcpy(drive_name, "Virtual QEMU USB xHCI Block Device");
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

                const char *speed_str = "USB 3.1 Gen 1 SuperSpeed (5 Gbps)";
                uint32_t usb_ver = 0x0310;
                if (speed == 4) {
                    speed_str = "USB 3.1 Gen 1 SuperSpeed (5 Gbps)";
                    usb_ver = 0x0310;
                } else if (speed == 5) {
                    speed_str = "USB 3.1 Gen 2 SuperSpeedPlus (10 Gbps)";
                    usb_ver = 0x0312;
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
                dev.flush = usb_flush_cache_impl;
                dev.shutdown = usb_shutdown_impl;

                storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
                snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/USB(0x%X,0x0)", slot, func, p);

                storage_register_device(&dev);
                klog_info("xHCI port %u: registered storage device '%s' (%s, path=%s)",
                          p, dev.name, dev.size_str, dev.devpath);
                continue;
            }
        }
    }

    if (d->active) {
        g_xhci_ctrl_count++;
    }
}

/* =========================================================================
 * EHCI (USB 2.0 High-Speed) Host Controller & Mass Storage BOT Driver
 * ========================================================================= */

static inline uint32_t ehci_phys(const void *ptr) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    if (addr >= KERNEL_VIRTUAL_BASE) {
        addr -= KERNEL_VIRTUAL_BASE;
    }
    return (uint32_t)addr;
}

static void ehci_delay_ms(int ms) {
    for (int i = 0; i < ms; i++) {
        for (int j = 0; j < 1000; j++) {
            io_wait();
        }
    }
}

static inline uint32_t ehci_make_token(uint8_t pid, uint32_t bytes, uint8_t toggle, uint8_t ioc) {
    return (1U << 7) /* Active */ |
           ((uint32_t)pid << 8) |
           (3U << 10) /* Cerr = 3 retries */ |
           ((ioc ? 1U : 0U) << 15) |
           ((bytes & 0x7FFFU) << 16) |
           ((uint32_t)(toggle & 1) << 31);
}

static void ehci_fill_qtd_buffers(EhciQtd *qtd, uint32_t phys_buf, uint32_t len) {
    qtd->buffer[0] = phys_buf;
    if (len > 0) {
        uint32_t cur = phys_buf;
        for (int i = 1; i < 5; i++) {
            cur = (cur + 0x1000) & ~0xFFFULL;
            qtd->buffer[i] = cur;
        }
    } else {
        for (int i = 1; i < 5; i++) qtd->buffer[i] = 0;
    }
}

static int ehci_control_transfer(EhciDriver *d, uint8_t req_type, uint8_t request,
                                uint16_t value, uint16_t index, uint16_t length,
                                void *data) {
    (void)d;
    int is_in = (req_type & 0x80) != 0;

    g_ehci_setup_pkt[0] = req_type;
    g_ehci_setup_pkt[1] = request;
    g_ehci_setup_pkt[2] = (uint8_t)(value & 0xFF);
    g_ehci_setup_pkt[3] = (uint8_t)(value >> 8);
    g_ehci_setup_pkt[4] = (uint8_t)(index & 0xFF);
    g_ehci_setup_pkt[5] = (uint8_t)(index >> 8);
    g_ehci_setup_pkt[6] = (uint8_t)(length & 0xFF);
    g_ehci_setup_pkt[7] = (uint8_t)(length >> 8);

    EhciQtd *base = &g_ehci_ctrl_qtds[g_ehci_ctrl_set ? 3 : 0];
    g_ehci_ctrl_set = !g_ehci_ctrl_set;
    EhciQtd *q_setup = &base[0];
    EhciQtd *q_data  = (length > 0) ? &base[1] : NULL;
    EhciQtd *q_stat  = (length > 0) ? &base[2] : &base[1];

    memset(q_setup, 0, sizeof(EhciQtd));
    q_setup->token = ehci_make_token(2 /* SETUP */, 8, 0 /* DATA0 */, 0);
    ehci_fill_qtd_buffers(q_setup, ehci_phys(g_ehci_setup_pkt), 8);
    q_setup->alt_next_qtd = 1;

    if (length > 0) {
        memset(q_data, 0, sizeof(EhciQtd));
        q_data->token = ehci_make_token(is_in ? 1 : 0, length, 1 /* DATA1 */, 0);
        ehci_fill_qtd_buffers(q_data, ehci_phys(data), length);
        q_data->next_qtd = ehci_phys(q_stat);
        q_data->alt_next_qtd = ehci_phys(q_stat); /* short packet jumps to status */

        q_setup->next_qtd = ehci_phys(q_data);
    } else {
        q_setup->next_qtd = ehci_phys(q_stat);
    }

    memset(q_stat, 0, sizeof(EhciQtd));
    uint8_t stat_pid = (length > 0 && is_in) ? 0 : 1;
    q_stat->token = ehci_make_token(stat_pid, 0, 1 /* DATA1 */, 1 /* IOC */);
    ehci_fill_qtd_buffers(q_stat, 0, 0);
    q_stat->next_qtd = 1;
    q_stat->alt_next_qtd = 1;

    g_ehci_qh_ep0.overlay_next_qtd = ehci_phys(q_setup);
    g_ehci_qh_ep0.overlay_alt_next_qtd = 1;
    __asm__ volatile ("mfence" ::: "memory");
    g_ehci_qh_ep0.overlay_token = 0;
    __asm__ volatile ("mfence" ::: "memory");

    int timeout = 1000000;
    while (--timeout > 0) {
        if (!(q_stat->token & 0x80)) break;
        if ((g_ehci_qh_ep0.overlay_token & 0x40) || (q_stat->token & 0x40)) break;
        io_wait();
    }

    if (timeout <= 0) {
        klog_warn("EHCI: timeout: setup=0x%08X data=0x%08X stat=0x%08X cur=0x%08X next=0x%08X qh_tok=0x%08X",
                  q_setup->token, q_data ? q_data->token : 0, q_stat->token,
                  g_ehci_qh_ep0.current_qtd, g_ehci_qh_ep0.overlay_next_qtd, g_ehci_qh_ep0.overlay_token);
        return -1;
    }
    uint32_t final_stat = (g_ehci_qh_ep0.current_qtd == ehci_phys(q_stat)) ? g_ehci_qh_ep0.overlay_token : q_stat->token;
    if (final_stat & 0x7E) {
        klog_warn("EHCI: control transfer error (setup=0x%08X stat=0x%08X final=0x%08X)", q_setup->token, q_stat->token, final_stat);
        return -1;
    }
    return 0;
}

static int ehci_bulk_transfer(EhciDriver *d, EhciQh *qh, void *buf, uint32_t len, int is_in) {
    (void)d;
    if (len == 0) return 0;

    EhciQtd *qtd = &g_ehci_bulk_qtds[g_ehci_bulk_qtd_idx];
    g_ehci_bulk_qtd_idx = (g_ehci_bulk_qtd_idx + 1) % 8;
    memset(qtd, 0, sizeof(EhciQtd));
    qtd->token = ehci_make_token(is_in ? 1 : 0, len, 0, 1 /* IOC */);
    ehci_fill_qtd_buffers(qtd, ehci_phys(buf), len);
    qtd->next_qtd = 1;
    qtd->alt_next_qtd = 1;

    qh->overlay_next_qtd = ehci_phys(qtd);
    qh->overlay_alt_next_qtd = 1;
    __asm__ volatile ("mfence" ::: "memory");
    qh->overlay_token &= (1U << 31); /* Preserve data toggle, clear active/halted */
    __asm__ volatile ("mfence" ::: "memory");

    int timeout = 5000000;
    while (--timeout > 0) {
        if (!(qtd->token & 0x80)) break;
        if (qtd->token & 0x40) break;
        io_wait();
    }

    if (timeout <= 0) {
        klog_warn("EHCI: bulk transfer timeout (is_in=%d len=%u token=0x%08X cur=0x%08X qh_tok=0x%08X)",
                  is_in, len, qtd->token, qh->current_qtd, qh->overlay_token);
        return -1;
    }
    if (qtd->token & 0x7E) {
        klog_warn("EHCI: bulk transfer error (token=0x%08X qh_tok=0x%08X)", qtd->token, qh->overlay_token);
        return -1;
    }
    return 0;
}

static void ehci_clear_halt(EhciDriver *d, uint8_t ep_addr, EhciQh *qh) {
    /* Send standard CLEAR_FEATURE (ENDPOINT_HALT) on EP0 */
    /* bmRequestType = 0x02, bRequest = 0x01, wValue = 0x0000, wIndex = ep_addr, wLength = 0 */
    ehci_control_transfer(d, 0x02, 0x01, 0x0000, ep_addr, 0, NULL);
    if (qh) {
        qh->overlay_token = 0;
        qh->overlay_next_qtd = 1;
    }
}

static void ehci_bot_reset(EhciDriver *d) {
    /* Standard USB Mass Storage Class Bulk-Only Reset (BOMSR) */
    /* bmRequestType = 0x21, bRequest = 0xFF, wValue = 0x0000, wIndex = 0, wLength = 0 */
    ehci_control_transfer(d, 0x21, 0xFF, 0x0000, 0, 0, NULL);
    ehci_clear_halt(d, d->bulk_in_ep | 0x80, &g_ehci_qh_bulk_in);
    ehci_clear_halt(d, d->bulk_out_ep, &g_ehci_qh_bulk_out);
}

static int ehci_bot_exec(EhciDriver *d, const uint8_t *cdb, uint8_t cdb_len,
                         void *data, uint32_t data_len, int is_in) {
    memset(&g_ehci_cbw, 0, sizeof(UsbCbw));
    g_ehci_cbw.signature = 0x43425355; /* "USBC" */
    g_ehci_cbw.tag = ++d->cbw_tag;
    g_ehci_cbw.data_transfer_len = data_len;
    g_ehci_cbw.flags = is_in ? 0x80 : 0x00;
    g_ehci_cbw.lun = 0;
    g_ehci_cbw.cb_len = cdb_len;
    memcpy(g_ehci_cbw.cb, cdb, cdb_len);

    int cbw_res = ehci_bulk_transfer(d, &g_ehci_qh_bulk_out, &g_ehci_cbw, sizeof(UsbCbw), 0);
    if (cbw_res != 0) {
        klog_warn("BOT tag=%u: CBW failed (%d)", d->cbw_tag, cbw_res);
        ehci_bot_reset(d);
        return -1;
    }

    if (data_len > 0 && data != NULL) {
        if (is_in) {
            int d_res = ehci_bulk_transfer(d, &g_ehci_qh_bulk_in, data, data_len, 1);
            if (d_res != 0) {
                klog_warn("BOT tag=%u: DATA IN failed (%d)", d->cbw_tag, d_res);
                ehci_bot_reset(d);
                return -1;
            }
        } else {
            int d_res = ehci_bulk_transfer(d, &g_ehci_qh_bulk_out, data, data_len, 0);
            if (d_res != 0) {
                klog_warn("BOT tag=%u: DATA OUT failed (%d)", d->cbw_tag, d_res);
                ehci_bot_reset(d);
                return -1;
            }
        }
    }

    memset(&g_ehci_csw, 0, sizeof(UsbCsw));
    int csw_res = ehci_bulk_transfer(d, &g_ehci_qh_bulk_in, &g_ehci_csw, sizeof(UsbCsw), 1);
    if (csw_res != 0) {
        klog_warn("BOT tag=%u: CSW bulk transfer failed (%d), attempting recovery", d->cbw_tag, csw_res);
        ehci_clear_halt(d, d->bulk_in_ep | 0x80, &g_ehci_qh_bulk_in);
        memset(&g_ehci_csw, 0, sizeof(UsbCsw));
        csw_res = ehci_bulk_transfer(d, &g_ehci_qh_bulk_in, &g_ehci_csw, sizeof(UsbCsw), 1);
        if (csw_res != 0) {
            klog_warn("BOT tag=%u: CSW retry failed (%d)", d->cbw_tag, csw_res);
            ehci_bot_reset(d);
            return -1;
        }
    }

    if (g_ehci_csw.signature == 0x53425355 && g_ehci_csw.tag == d->cbw_tag && g_ehci_csw.status == 0) {
        return 0;
    }
    klog_warn("BOT tag=%u: CSW bad response: sig=0x%08X tag=%u (exp %u) stat=%u",
              d->cbw_tag, g_ehci_csw.signature, g_ehci_csw.tag, d->cbw_tag, g_ehci_csw.status);
    ehci_bot_reset(d);
    return -1;
}

static int ehci_scsi_inquiry(EhciDriver *d, char *out_name, size_t max_len) {
    uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, 36, 0x00 };
    memset(g_ehci_dma_buf, 0, 64);
    if (ehci_bot_exec(d, cdb, 6, g_ehci_dma_buf, 36, 1) != 0) return -1;

    char vendor[9];
    char product[17];
    memcpy(vendor, &g_ehci_dma_buf[8], 8);
    vendor[8] = '\0';
    trim(vendor);

    memcpy(product, &g_ehci_dma_buf[16], 16);
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

static int ehci_scsi_read_capacity(EhciDriver *d, uint64_t *out_sectors, uint32_t *out_sec_size) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    memset(g_ehci_dma_buf, 0, 16);
    int ok = 0;
    for (int retry = 0; retry < 5; retry++) {
        if (ehci_bot_exec(d, cdb, 10, g_ehci_dma_buf, 8, 1) == 0) {
            ok = 1;
            break;
        }
        ehci_delay_ms(20);
    }

    if (!ok) {
        if (out_sectors) *out_sectors = 1024000;
        if (out_sec_size) *out_sec_size = 512;
        return 0;
    }

    uint32_t last_lba = ((uint32_t)g_ehci_dma_buf[0] << 24) | ((uint32_t)g_ehci_dma_buf[1] << 16) |
                        ((uint32_t)g_ehci_dma_buf[2] << 8)  | (uint32_t)g_ehci_dma_buf[3];
    uint32_t blk_size = ((uint32_t)g_ehci_dma_buf[4] << 24) | ((uint32_t)g_ehci_dma_buf[5] << 16) |
                        ((uint32_t)g_ehci_dma_buf[6] << 8)  | (uint32_t)g_ehci_dma_buf[7];

    if (out_sectors) *out_sectors = (uint64_t)last_lba + 1;
    if (out_sec_size) *out_sec_size = blk_size > 0 ? blk_size : 512;
    return 0;
}

static int ehci_read_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, void *buf) {
    EhciDriver *d = (EhciDriver *)dev->driver_priv;
    if (!d || !buf || count == 0) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        uint16_t chunk = remaining > 32 ? 32 : (uint16_t)remaining;
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
        if (ehci_bot_exec(d, cdb, 10, g_ehci_dma_buf, byte_len, 1) != 0) {
            return -1;
        }

        memcpy(dst, g_ehci_dma_buf, byte_len);
        dst += byte_len;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int ehci_write_sectors_impl(StorageDevice *dev, uint64_t lba, uint32_t count, const void *buf) {
    EhciDriver *d = (EhciDriver *)dev->driver_priv;
    if (!d || !buf || count == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0) {
        uint16_t chunk = remaining > 32 ? 32 : (uint16_t)remaining;
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
        memcpy(g_ehci_dma_buf, src, byte_len);

        if (ehci_bot_exec(d, cdb, 10, g_ehci_dma_buf, byte_len, 0) != 0) {
            return -1;
        }

        src += byte_len;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int ehci_flush_cache_impl(StorageDevice *dev) {
    (void)dev;
    return 0;
}

static void ehci_shutdown_impl(StorageDevice *dev) {
    if (!dev || !dev->driver_priv) return;
    EhciDriver *d = (EhciDriver *)dev->driver_priv;
    ehci_flush_cache_impl(dev);
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1B; /* START STOP UNIT */
    cdb[4] = 0x00; /* STOP */
    ehci_bot_exec(d, cdb, 6, NULL, 0, 0);
}

static void probe_ehci_controller(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base) {
    uint64_t bar0_phys = mmio_base & ~0xFFFULL;
    uint64_t bar0_virt = KERNEL_VIRTUAL_BASE + bar0_phys;
    size_t num_pages = 4; /* 16 KB EHCI MMIO space */
    vmm_map_pages(vmm_get_kernel_pml4(), bar0_virt, bar0_phys, num_pages, PTE_WRITABLE | PTE_PCD | PTE_PWT);
    vmm_map_pages(vmm_get_kernel_pml4(), bar0_phys, bar0_phys, num_pages, PTE_WRITABLE | PTE_PCD | PTE_PWT);
    mmio_base = bar0_virt + (mmio_base & 0xFFFULL);

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)mmio_base;
    uint8_t caplength = cap[0];
    if (caplength == 0 || caplength > 0x80) caplength = 0x20;
    uintptr_t op_base = mmio_base + caplength;

    /* BIOS Handoff (USBLEGSUP) */
    uint32_t hccparams = *(volatile uint32_t *)(uintptr_t)(mmio_base + 0x08);
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFF);
    if (eecp >= 0x40) {
        uint32_t legsup = pci_read32(bus, slot, func, eecp);
        if (legsup & (1 << 16)) {
            pci_write32(bus, slot, func, eecp, legsup | (1 << 24));
            int wait_count = 50;
            while (wait_count-- > 0) {
                uint32_t cur = pci_read32(bus, slot, func, eecp);
                if (!(cur & (1 << 16))) break;
                ehci_delay_ms(1);
            }
        }
        pci_write32(bus, slot, func, eecp + 4, 0); /* Disable SMIs */
    }

    /* Stop Controller if running */
    volatile uint32_t *usbcmd = (volatile uint32_t *)(op_base + 0x00);
    volatile uint32_t *usbsts = (volatile uint32_t *)(op_base + 0x04);

    if (!(*usbsts & (1 << 12))) {
        *usbcmd &= ~(1 << 0);
        int wait_hch = 10000;
        while (!(*usbsts & (1 << 12)) && --wait_hch > 0) io_wait();
    }

    /* Reset Controller */
    *usbcmd |= (1 << 1); /* HCRESET */
    int wait_rst = 50000;
    while ((*usbcmd & (1 << 1)) && --wait_rst > 0) io_wait();
    ehci_delay_ms(10);

    /* Program CTRLDSSEGMENT (32-bit DMA) */
    volatile uint32_t *ctrldssegment = (volatile uint32_t *)(op_base + 0x10);
    *ctrldssegment = 0;

    /* Clear Status & disable interrupts for polling */
    *usbsts = 0x3F;
    volatile uint32_t *usbintr = (volatile uint32_t *)(op_base + 0x08);
    *usbintr = 0;

    /* Initialize Queue Heads */
    memset(&g_ehci_qh_ep0, 0, sizeof(EhciQh));
    memset(&g_ehci_qh_bulk_out, 0, sizeof(EhciQh));
    memset(&g_ehci_qh_bulk_in, 0, sizeof(EhciQh));

    /* Link into circular async ring */
    g_ehci_qh_ep0.qh_link = ehci_phys(&g_ehci_qh_bulk_out) | 0x02;
    g_ehci_qh_bulk_out.qh_link = ehci_phys(&g_ehci_qh_bulk_in) | 0x02;
    g_ehci_qh_bulk_in.qh_link = ehci_phys(&g_ehci_qh_ep0) | 0x02;

    /* EP0 initial setup */
    g_ehci_qh_ep0.ep_char = (0 /* DevAddr */) |
                            (0 << 8 /* EP 0 */) |
                            (2 << 12 /* High-Speed */) |
                            (1 << 14 /* DTC=1 */) |
                            (1 << 15 /* H=1 Head of reclamation */) |
                            (64 << 16 /* MaxPacket 64 */);
    g_ehci_qh_ep0.ep_cap = (1 << 30); /* Mult=1 */
    g_ehci_qh_ep0.overlay_next_qtd = 1;

    /* Bulk OUT */
    g_ehci_qh_bulk_out.ep_char = (0) | (1 << 8) | (2 << 12) | (0 << 14) | (512 << 16);
    g_ehci_qh_bulk_out.ep_cap = (1 << 30);
    g_ehci_qh_bulk_out.overlay_next_qtd = 1;

    /* Bulk IN */
    g_ehci_qh_bulk_in.ep_char = (0) | (2 << 8) | (2 << 12) | (0 << 14) | (512 << 16);
    g_ehci_qh_bulk_in.ep_cap = (1 << 30);
    g_ehci_qh_bulk_in.overlay_next_qtd = 1;

    /* Set ASYNCLISTADDR */
    volatile uint32_t *asynclistaddr = (volatile uint32_t *)(op_base + 0x18);
    *asynclistaddr = ehci_phys(&g_ehci_qh_ep0);

    /* Route all ports to EHCI root hub (CONFIGFLAG = 1) */
    volatile uint32_t *configflag = (volatile uint32_t *)(op_base + 0x40);
    *configflag = 1;
    ehci_delay_ms(10);

    /* Start Controller & Enable Async Schedule */
    *usbcmd |= (1 << 0) /* RS */ | (1 << 5) /* ASE */ | (8 << 16) /* ITC=8 microframes */;

    int wait_as = 10000;
    while (!(*usbsts & (1 << 5)) && --wait_as > 0) io_wait();

    klog_info("EHCI controller initialized: mmio=0x%llX op=0x%llX", mmio_base, op_base);

    /* Query number of ports */
    uint32_t hcsparams = *(volatile uint32_t *)(uintptr_t)(mmio_base + 0x04);
    uint8_t n_ports = (uint8_t)(hcsparams & 0x0F);
    klog_info("EHCI: %u ports detected", n_ports);

    /* Power up ports */
    for (uint8_t p = 0; p < n_ports; p++) {
        volatile uint32_t *portsc = (volatile uint32_t *)(op_base + 0x44 + (p * 4));
        *portsc |= (1 << 12); /* PP = 1 */
    }
    ehci_delay_ms(50);

    /* Scan and initialize connected devices */
    for (uint8_t p = 0; p < n_ports; p++) {
        volatile uint32_t *portsc = (volatile uint32_t *)(op_base + 0x44 + (p * 4));
        uint32_t status = *portsc;
        klog_info("EHCI port %u: status=0x%08X", p, status);
        if (!(status & 1)) continue; /* CCS == 0 */

        uint32_t ls = (status >> 10) & 3;
        if (ls == 1) {
            klog_info("EHCI port %u: Low-speed device detected, releasing to companion", p);
            *portsc |= (1 << 13); /* Port Owner = 1 */
            continue;
        }

        klog_info("EHCI port %u: device connected, resetting...", p);
        /* Assert Port Reset (PR=1), avoid W1C bits (1, 3, 5), keep PP=1 */
        uint32_t val = *portsc;
        val &= ~((1 << 1) | (1 << 3) | (1 << 5));
        val |= (1 << 8) | (1 << 12);
        *portsc = val;
        ehci_delay_ms(50);

        /* Deassert Port Reset */
        val = *portsc;
        val &= ~((1 << 1) | (1 << 3) | (1 << 5) | (1 << 8));
        val |= (1 << 12);
        *portsc = val;
        ehci_delay_ms(20);

        status = *portsc;
        klog_info("EHCI port %u: post-reset status=0x%08X", p, status);
        if (!(status & (1 << 2))) {
            klog_warn("EHCI port %u: port not enabled (PE=0), skipping", p);
            continue;
        }

        EhciDriver *d = &g_ehci_driver;
        memset(d, 0, sizeof(EhciDriver));
        d->mmio_base = mmio_base;
        d->op_base = op_base;
        d->bus = bus;
        d->slot = slot;
        d->func = func;
        d->dev_addr = 0;

        /* Step A: GET_DESCRIPTOR (Device) 8 bytes */
        memset(g_ehci_desc_buf, 0, sizeof(g_ehci_desc_buf));
        if (ehci_control_transfer(d, 0x80, 0x06, 0x0100, 0, 8, g_ehci_desc_buf) != 0) {
            klog_warn("EHCI port %u: failed to read device descriptor", p);
            continue;
        }
        uint8_t ep0_mps = g_ehci_desc_buf[7];
        if (ep0_mps == 0) ep0_mps = 64;
        g_ehci_qh_ep0.ep_char = (g_ehci_qh_ep0.ep_char & ~(0x7FF << 16)) | (ep0_mps << 16);

        /* Step B: SET_ADDRESS */
        uint8_t new_addr = (uint8_t)(p + 1);
        if (ehci_control_transfer(d, 0x00, 0x05, new_addr, 0, 0, NULL) != 0) {
            klog_warn("EHCI port %u: SET_ADDRESS failed", p);
            continue;
        }
        ehci_delay_ms(10);
        d->dev_addr = new_addr;
        g_ehci_qh_ep0.ep_char = (g_ehci_qh_ep0.ep_char & ~0x7F) | (new_addr & 0x7F);

        /* Step C: GET_DESCRIPTOR (Device) full 18 bytes */
        memset(g_ehci_desc_buf, 0, sizeof(g_ehci_desc_buf));
        if (ehci_control_transfer(d, 0x80, 0x06, 0x0100, 0, 18, g_ehci_desc_buf) != 0) {
            klog_warn("EHCI port %u: failed to read full device descriptor", p);
            continue;
        }
        uint16_t vid = (uint16_t)g_ehci_desc_buf[8] | ((uint16_t)g_ehci_desc_buf[9] << 8);
        uint16_t pid = (uint16_t)g_ehci_desc_buf[10] | ((uint16_t)g_ehci_desc_buf[11] << 8);
        klog_info("EHCI port %u: device addr %u, VID=0x%04X, PID=0x%04X", p, new_addr, vid, pid);

        /* Step D: GET_DESCRIPTOR (Configuration) */
        memset(g_ehci_desc_buf, 0, sizeof(g_ehci_desc_buf));
        if (ehci_control_transfer(d, 0x80, 0x06, 0x0200, 0, 9, g_ehci_desc_buf) != 0) {
            klog_warn("EHCI port %u: failed to read config descriptor header", p);
            continue;
        }
        uint16_t total_len = (uint16_t)g_ehci_desc_buf[2] | ((uint16_t)g_ehci_desc_buf[3] << 8);
        if (total_len > sizeof(g_ehci_desc_buf)) total_len = sizeof(g_ehci_desc_buf);

        if (ehci_control_transfer(d, 0x80, 0x06, 0x0200, 0, total_len, g_ehci_desc_buf) != 0) {
            klog_warn("EHCI port %u: failed to read full config descriptor", p);
            continue;
        }

        /* Parse Configuration Descriptor */
        uint8_t config_val = g_ehci_desc_buf[5];
        uint8_t bulk_in = 0, bulk_out = 0;
        uint16_t in_mps = 512, out_mps = 512;
        int is_storage = 0;

        size_t off = 0;
        while (off + 2 <= total_len) {
            uint8_t len = g_ehci_desc_buf[off];
            uint8_t type = g_ehci_desc_buf[off + 1];
            if (len == 0 || off + len > total_len) break;

            if (type == 0x04 && len >= 9) { /* Interface */
                uint8_t if_class = g_ehci_desc_buf[off + 5];
                uint8_t if_subclass = g_ehci_desc_buf[off + 6];
                uint8_t if_proto = g_ehci_desc_buf[off + 7];
                if (if_class == 0x08 && if_subclass == 0x06 && if_proto == 0x50) {
                    is_storage = 1;
                }
            } else if (type == 0x05 && len >= 7) { /* Endpoint */
                uint8_t ep_addr = g_ehci_desc_buf[off + 2];
                uint8_t attr = g_ehci_desc_buf[off + 3];
                uint16_t mps = (uint16_t)g_ehci_desc_buf[off + 4] | ((uint16_t)g_ehci_desc_buf[off + 5] << 8);
                if ((attr & 0x03) == 0x02) { /* Bulk */
                    if (ep_addr & 0x80) {
                        bulk_in = ep_addr & 0x0F;
                        in_mps = mps;
                    } else {
                        bulk_out = ep_addr & 0x0F;
                        out_mps = mps;
                    }
                }
            }
            off += len;
        }

        if (!is_storage || bulk_in == 0 || bulk_out == 0) {
            klog_info("EHCI port %u: not a supported USB Mass Storage device", p);
            continue;
        }

        klog_info("EHCI port %u: Mass Storage detected: Bulk IN EP%u (mps=%u), Bulk OUT EP%u (mps=%u)",
                  p, bulk_in, in_mps, bulk_out, out_mps);

        /* Step E: SET_CONFIGURATION */
        if (ehci_control_transfer(d, 0x00, 0x09, config_val, 0, 0, NULL) != 0) {
            klog_warn("EHCI port %u: SET_CONFIGURATION failed", p);
            continue;
        }
        ehci_delay_ms(10);

        /* Configure Bulk Queue Heads */
        d->bulk_in_ep = bulk_in;
        d->bulk_out_ep = bulk_out;
        d->bulk_in_mps = in_mps;
        d->bulk_out_mps = out_mps;

        g_ehci_qh_bulk_out.ep_char = (new_addr & 0x7F) |
                                     ((bulk_out & 0x0F) << 8) |
                                     (2 << 12 /* High Speed */) |
                                     (0 << 14 /* DTC=0: HW manages toggle */) |
                                     (out_mps << 16);
        g_ehci_qh_bulk_out.ep_cap = (1 << 30);
        g_ehci_qh_bulk_out.overlay_next_qtd = 1;
        g_ehci_qh_bulk_out.overlay_token = 0;

        g_ehci_qh_bulk_in.ep_char = (new_addr & 0x7F) |
                                    ((bulk_in & 0x0F) << 8) |
                                    (2 << 12 /* High Speed */) |
                                    (0 << 14 /* DTC=0: HW manages toggle */) |
                                    (in_mps << 16);
        g_ehci_qh_bulk_in.ep_cap = (1 << 30);
        g_ehci_qh_bulk_in.overlay_next_qtd = 1;
        g_ehci_qh_bulk_in.overlay_token = 0;

        /* Step F: SCSI INQUIRY */
        char drive_name[64];
        if (ehci_scsi_inquiry(d, drive_name, sizeof(drive_name)) != 0 || drive_name[0] == '\0') {
            strncpy(drive_name, "USB Mass Storage Drive", sizeof(drive_name) - 1);
        }
        if (strstr(drive_name, "QEMU") != NULL) {
            strcpy(drive_name, "Virtual QEMU USB EHCI Block Device");
        }

        /* Step G: SCSI READ CAPACITY */
        uint64_t total_sectors = 0;
        uint32_t sector_size = 512;
        if (ehci_scsi_read_capacity(d, &total_sectors, &sector_size) != 0) {
            klog_warn("EHCI port %u: SCSI READ CAPACITY failed", p);
            continue;
        }
        d->total_sectors = total_sectors;
        d->sector_size = sector_size;

        /* Step H: Register Storage Device */
        StorageDevice dev;
        memset(&dev, 0, sizeof(StorageDevice));
        strncpy(dev.name, drive_name, sizeof(dev.name) - 1);
        strcpy(dev.type_str, "USB");
        strncpy(dev.bus_speed, "USB 2.0 High-Speed (480 Mbps)", sizeof(dev.bus_speed) - 1);
        dev.type = STORAGE_TYPE_EXTERNAL_USB;
        dev.usb_version = 0x0200;
        dev.total_sectors = total_sectors;
        dev.sector_size = sector_size;
        dev.pci_bus = bus;
        dev.pci_slot = slot;
        dev.pci_func = func;
        dev.driver_priv = d;
        dev.read_sectors = ehci_read_sectors_impl;
        dev.write_sectors = ehci_write_sectors_impl;
        dev.flush = ehci_flush_cache_impl;
        dev.shutdown = ehci_shutdown_impl;

        storage_format_size(dev.total_sectors * dev.sector_size, dev.size_str, sizeof(dev.size_str));
        snprintf(dev.devpath, sizeof(dev.devpath), "PciRoot(0x0)/Pci(0x%X,0x%X)/USB(0x%X,0x0)", slot, func, p);

        storage_register_device(&dev);
        g_ehci_active = 1;
        klog_info("EHCI port %u: registered storage device '%s' (%s, path=%s)",
                  p, dev.name, dev.size_str, dev.devpath);
    }
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

    if (prog_if == 0x30) {
        probe_xhci_controller(bus, slot, func, mmio_base);
    } else if (prog_if == 0x20) {
        probe_ehci_controller(bus, slot, func, mmio_base);
    }
}

void usb_storage_init(void) {
    g_xhci_ctrl_count = 0;
    g_hid_dev_count = 0;
    g_xhci_active = 0;
    g_ehci_active = 0;
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
                }
            }
        }
    }
}
