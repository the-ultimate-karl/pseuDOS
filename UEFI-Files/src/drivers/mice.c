#include "mice.h"
#include "io.h"
#include "lib.h"
#include "klog.h"
#include "pit.h"
#include "vmm.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define MOUSE_QUEUE_SIZE 128

static volatile mouse_event_t g_event_queue[MOUSE_QUEUE_SIZE];
static volatile uint8_t g_queue_head = 0;
static volatile uint8_t g_queue_tail = 0;

static volatile int32_t g_mouse_x = 640;
static volatile int32_t g_mouse_y = 360;
static volatile uint8_t g_buttons = 0;
static volatile int8_t  g_scroll_wheel = 0;

static uint32_t g_max_x = 1279;
static uint32_t g_max_y = 719;

static int g_has_wheel = 0;
static int g_packet_size = 3;
static uint8_t g_packet[4];
static int g_packet_idx = 0;
static uint64_t g_last_byte_tick = 0;
static int g_mice_initialized = 0;

/* ============================================================================
 * AMD FCH / Synopsys DesignWare APB I2C + Microsoft I2C-HID Touchpad Driver
 * Supports ASUS Vivobook (ASUE1200 / ELAN1200 / ELAN1300 / SYNA) on Ryzen FCH
 * ============================================================================ */
#define DW_IC_CON            0x00
#define DW_IC_TAR            0x04
#define DW_IC_DATA_CMD       0x10
#define DW_IC_SS_SCL_HCNT    0x14
#define DW_IC_SS_SCL_LCNT    0x18
#define DW_IC_FS_SCL_HCNT    0x1C
#define DW_IC_FS_SCL_LCNT    0x20
#define DW_IC_INTR_MASK      0x30
#define DW_IC_RAW_INTR_STAT  0x34
#define DW_IC_RX_TL          0x38
#define DW_IC_TX_TL          0x3C
#define DW_IC_CLR_INTR       0x40
#define DW_IC_CLR_TX_ABRT    0x54
#define DW_IC_ENABLE         0x6C
#define DW_IC_STATUS         0x70
#define DW_IC_TXFLR          0x74
#define DW_IC_RXFLR          0x78
#define DW_IC_TX_ABRT_SOURCE 0x80
#define DW_IC_ENABLE_STATUS  0x9C
#define DW_IC_COMP_TYPE      0xFC

#define DW_IC_COMP_TYPE_VALUE 0x44570140U /* "DW\x01@" */

typedef struct {
    int       active;
    uintptr_t base;
    uint8_t   slave_addr;
    uint16_t  hid_desc_reg;
    uint16_t  report_desc_len;
    uint16_t  report_desc_reg;
    uint16_t  input_reg;
    uint16_t  max_input_len;
    uint16_t  cmd_reg;
    uint16_t  data_reg;
    uint16_t  vid;
    uint16_t  pid;
    uint8_t   mouse_report_id;
    uint8_t   ptp_report_id;
    uint8_t   mouse_xy_16bit;
    int       has_prev_abs;
    int32_t   prev_abs_x;
    int32_t   prev_abs_y;
    uint64_t  last_poll_tick;
} I2cHidTouchpad;

static I2cHidTouchpad g_i2c_tp;

static inline uint64_t mice_get_rflags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static inline uint32_t dw_readl(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void dw_writel(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static void dw_i2c_set_enable(uintptr_t base, int enable) {
    dw_writel(base, DW_IC_ENABLE, enable ? 1U : 0U);
    int timeout = 10000;
    while (((dw_readl(base, DW_IC_ENABLE_STATUS) & 1U) != (enable ? 1U : 0U)) && --timeout > 0) {
        io_wait();
    }
}

static void dw_i2c_wait_idle(uintptr_t base) {
    int timeout = 10000;
    while ((dw_readl(base, DW_IC_STATUS) & 1U /* ACTIVITY */) && --timeout > 0) {
        io_wait();
    }
}

static void dw_i2c_configure_target(uintptr_t base, uint8_t addr) {
    uint32_t cur_tar = dw_readl(base, DW_IC_TAR) & 0x7FU;
    uint32_t cur_en = dw_readl(base, DW_IC_ENABLE) & 1U;
    if (cur_en && cur_tar == addr) {
        (void)dw_readl(base, DW_IC_CLR_INTR);
        (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
        return;
    }

    dw_i2c_wait_idle(base);
    uint32_t orig_con = dw_readl(base, DW_IC_CON);
    dw_i2c_set_enable(base, 0);

    /* Preserve firmware SCL counts if valid, otherwise set safe 400kHz Fast Mode counts */
    uint32_t fs_hcnt = dw_readl(base, DW_IC_FS_SCL_HCNT);
    uint32_t fs_lcnt = dw_readl(base, DW_IC_FS_SCL_LCNT);
    if (fs_hcnt < 10 || fs_hcnt > 0xFFFF || fs_lcnt < 10 || fs_lcnt > 0xFFFF) {
        dw_writel(base, DW_IC_FS_SCL_HCNT, 125);
        dw_writel(base, DW_IC_FS_SCL_LCNT, 200);
        dw_writel(base, DW_IC_SS_SCL_HCNT, 600);
        dw_writel(base, DW_IC_SS_SCL_LCNT, 700);
    }

    /* Preserve firmware speed selection (Standard=1 or Fast=2), default to Fast (2) */
    uint32_t speed_bits = orig_con & (3U << 1);
    if (speed_bits != (1U << 1) && speed_bits != (2U << 1)) {
        speed_bits = (2U << 1);
    }

    /* Master mode (bit 0) | speed_bits | Restart_en (bit 5) | Slave_disable (bit 6) */
    dw_writel(base, DW_IC_CON, (1U << 0) | speed_bits | (1U << 5) | (1U << 6));
    dw_writel(base, DW_IC_TAR, (uint32_t)(addr & 0x7F));
    dw_writel(base, DW_IC_INTR_MASK, 0U);
    (void)dw_readl(base, DW_IC_CLR_INTR);
    (void)dw_readl(base, DW_IC_CLR_TX_ABRT);

    dw_i2c_set_enable(base, 1);
}

static void dw_i2c_flush_rx(uintptr_t base) {
    int limit = 64;
    while ((dw_readl(base, DW_IC_STATUS) & (1U << 3)) && --limit > 0) {
        (void)dw_readl(base, DW_IC_DATA_CMD);
    }
    (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
    (void)dw_readl(base, DW_IC_CLR_INTR);
}

static int dw_i2c_write_read(uintptr_t base, uint8_t addr, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen) {
    dw_i2c_configure_target(base, addr);
    dw_i2c_flush_rx(base);

    /* 1. Write phase */
    for (size_t i = 0; i < wlen; i++) {
        int timeout = 50000;
        while (!(dw_readl(base, DW_IC_STATUS) & (1U << 1) /* TFNF */) && --timeout > 0) {
            if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6) /* TX_ABRT */) {
                (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
                dw_i2c_wait_idle(base);
                return -1;
            }
            io_wait();
        }
        if (timeout <= 0) return -1;

        uint32_t cmd = wbuf[i];
        if (rlen == 0 && i == wlen - 1) {
            cmd |= (1U << 9); /* STOP */
        }
        dw_writel(base, DW_IC_DATA_CMD, cmd);
    }

    /* Wait for write bytes to shift out so we detect address NAK before queuing reads */
    if (wlen > 0) {
        int wwait = 50000;
        while (!(dw_readl(base, DW_IC_STATUS) & (1U << 2) /* TFE */) && --wwait > 0) {
            if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6) /* TX_ABRT */) {
                (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
                dw_i2c_wait_idle(base);
                return -1;
            }
            io_wait();
        }
        if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6)) {
            (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
            dw_i2c_wait_idle(base);
            return -1;
        }
    }

    /* 2. Paced Read phase (keep <= 4 bytes in flight so 8/16-byte RX FIFO never overflows) */
    size_t tx_queued = 0;
    size_t rx_idx = 0;
    int wait = 200000;

    while (rx_idx < rlen && --wait > 0) {
        if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6) /* TX_ABRT */) {
            (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
            dw_i2c_wait_idle(base);
            return -1;
        }

        while (rx_idx < rlen && (dw_readl(base, DW_IC_STATUS) & (1U << 3) /* RFNE */)) {
            rbuf[rx_idx++] = (uint8_t)(dw_readl(base, DW_IC_DATA_CMD) & 0xFF);
        }

        while (tx_queued < rlen && (tx_queued - rx_idx) < 4 && (dw_readl(base, DW_IC_STATUS) & (1U << 1) /* TFNF */)) {
            uint32_t cmd = (1U << 8); /* Read */
            if (tx_queued == 0 && wlen > 0) {
                cmd |= (1U << 10); /* RESTART */
            }
            if (tx_queued == rlen - 1) {
                cmd |= (1U << 9);  /* STOP */
            }
            dw_writel(base, DW_IC_DATA_CMD, cmd);
            tx_queued++;
        }

        io_wait();
    }

    if (rlen == 0) {
        dw_i2c_wait_idle(base);
    }

    return (rx_idx == rlen) ? 0 : -1;
}

static void i2c_hid_send_cmd(I2cHidTouchpad *tp, uint16_t opcode) {
    uint8_t cmd[4];
    cmd[0] = (uint8_t)(tp->cmd_reg & 0xFF);
    cmd[1] = (uint8_t)((tp->cmd_reg >> 8) & 0xFF);
    cmd[2] = (uint8_t)(opcode & 0xFF);
    cmd[3] = (uint8_t)((opcode >> 8) & 0xFF);
    dw_i2c_write_read(tp->base, tp->slave_addr, cmd, 4, NULL, 0);
}

static void i2c_hid_init(void) {
    memset(&g_i2c_tp, 0, sizeof(g_i2c_tp));

    /* Map AMD FCH DesignWare I2C0..I2C5 MMIO pages (0xFEDC2000 .. 0xFEDC7FFF) */
    uint64_t fch_i2c_phys = 0xFEDC2000ULL;
    uint64_t fch_i2c_virt = KERNEL_VIRTUAL_BASE + fch_i2c_phys;
    vmm_map_pages(vmm_get_kernel_pml4(), fch_i2c_virt, fch_i2c_phys, 6, PTE_WRITABLE | PTE_PCD | PTE_PWT);
    vmm_map_pages(vmm_get_kernel_pml4(), fch_i2c_phys, fch_i2c_phys, 6, PTE_WRITABLE | PTE_PCD | PTE_PWT);

    static const uint8_t cand_addrs[] = { 0x15, 0x2C, 0x10, 0x40, 0x4C, 0x38, 0x20 };
    static const uint16_t cand_regs[] = { 0x0001, 0x0020 };

    for (int bus = 0; bus < 6; bus++) {
        uintptr_t base = (uintptr_t)(fch_i2c_virt + (uint64_t)bus * 0x1000ULL);
        uint32_t comp_type = dw_readl(base, DW_IC_COMP_TYPE);
        if (comp_type != DW_IC_COMP_TYPE_VALUE) {
            continue;
        }

        uint8_t fw_tar = (uint8_t)(dw_readl(base, DW_IC_TAR) & 0x7F);
        klog_info("I2C%d: DesignWare APB I2C controller detected at 0x%llx (FW TAR=0x%02x, EN=%u)",
                  bus, (unsigned long long)(fch_i2c_phys + (uint64_t)bus * 0x1000ULL),
                  fw_tar, dw_readl(base, DW_IC_ENABLE) & 1U);

        uint8_t probe_list[10];
        int probe_count = 0;
        if (fw_tar >= 0x08 && fw_tar <= 0x77 && fw_tar != 0x55) {
            probe_list[probe_count++] = fw_tar;
        }
        for (size_t a = 0; a < sizeof(cand_addrs); a++) {
            int dup = 0;
            for (int k = 0; k < probe_count; k++) {
                if (probe_list[k] == cand_addrs[a]) dup = 1;
            }
            if (!dup) probe_list[probe_count++] = cand_addrs[a];
        }

        for (int a = 0; a < probe_count; a++) {
            uint8_t addr = probe_list[a];
            for (size_t r = 0; r < 2; r++) {
                uint16_t dreg = cand_regs[r];
                uint8_t wbuf[2] = { (uint8_t)(dreg & 0xFF), (uint8_t)(dreg >> 8) };
                uint8_t desc[30];
                memset(desc, 0, sizeof(desc));

                if (dw_i2c_write_read(base, addr, wbuf, 2, desc, 30) == 0) {
                    uint16_t desc_len = *(uint16_t *)&desc[0];
                    uint16_t bcd_ver  = *(uint16_t *)&desc[2];
                    uint16_t rep_len  = *(uint16_t *)&desc[4];
                    uint16_t rep_reg  = *(uint16_t *)&desc[6];
                    uint16_t inp_reg  = *(uint16_t *)&desc[8];
                    uint16_t max_inp  = *(uint16_t *)&desc[10];
                    uint16_t cmd_reg  = *(uint16_t *)&desc[14];
                    uint16_t dat_reg  = *(uint16_t *)&desc[16];
                    uint16_t vid      = *(uint16_t *)&desc[18];
                    uint16_t pid      = *(uint16_t *)&desc[20];

                    if (desc_len == 0x001E && bcd_ver == 0x0100 && max_inp >= 4 && max_inp <= 256) {
                        g_i2c_tp.active = 1;
                        g_i2c_tp.base = base;
                        g_i2c_tp.slave_addr = addr;
                        g_i2c_tp.hid_desc_reg = dreg;
                        g_i2c_tp.report_desc_len = rep_len;
                        g_i2c_tp.report_desc_reg = rep_reg;
                        g_i2c_tp.input_reg = inp_reg;
                        g_i2c_tp.max_input_len = max_inp;
                        g_i2c_tp.cmd_reg = cmd_reg;
                        g_i2c_tp.data_reg = dat_reg;
                        g_i2c_tp.vid = vid;
                        g_i2c_tp.pid = pid;
                        g_i2c_tp.mouse_report_id = 1;
                        g_i2c_tp.ptp_report_id = 4;

                        /* Send SET_POWER(ON = 0x0800) to wake touchpad */
                        i2c_hid_send_cmd(&g_i2c_tp, 0x0800);
                        for (int dly = 0; dly < 15000; dly++) io_wait();

                        /* Inspect Report Descriptor (up to 128B) to check if Mouse report uses 16-bit X/Y */
                        if (rep_len > 0) {
                            uint16_t r_fetch = rep_len > 128 ? 128 : rep_len;
                            uint8_t rw[2] = { (uint8_t)(rep_reg & 0xFF), (uint8_t)(rep_reg >> 8) };
                            uint8_t rbuf[128];
                            memset(rbuf, 0, sizeof(rbuf));
                            if (dw_i2c_write_read(base, addr, rw, 2, rbuf, r_fetch) == 0) {
                                int in_mouse = 0;
                                for (uint16_t ri = 0; ri + 1 < r_fetch; ri++) {
                                    if (rbuf[ri] == 0x05 && rbuf[ri + 1] == 0x01 &&
                                        ri + 3 < r_fetch && rbuf[ri + 2] == 0x09 &&
                                        (rbuf[ri + 3] == 0x02 || rbuf[ri + 3] == 0x01)) {
                                        in_mouse = 1;
                                    } else if (rbuf[ri] == 0x05 && rbuf[ri + 1] == 0x0D) {
                                        in_mouse = 0;
                                    }
                                    if (in_mouse && rbuf[ri] == 0x85) {
                                        g_i2c_tp.mouse_report_id = rbuf[ri + 1];
                                    }
                                    if (in_mouse && rbuf[ri] == 0x75 && rbuf[ri + 1] == 0x10) {
                                        g_i2c_tp.mouse_xy_16bit = 1;
                                    }
                                }
                            }
                        }

                        klog_info("I2C%d: I2C-HID Touchpad active at 0x%02x (VID=%04x PID=%04x max_in=%u cmd=0x%04x 16b=%u)",
                                  bus, addr, vid, pid, max_inp, cmd_reg, g_i2c_tp.mouse_xy_16bit);
                        return;
                    }
                }
            }
        }
    }
}

static void i2c_hid_poll(void) {
    if (!g_i2c_tp.active) return;

    /* Rate-limit I2C polling to ~50 Hz (every 2 PIT ticks) */
    uint64_t now = pit_get_ticks();
    if (now - g_i2c_tp.last_poll_tick < 2) return;
    g_i2c_tp.last_poll_tick = now;

    uintptr_t base = g_i2c_tp.base;

    /* Clear any previous TX_ABRT and flush stale RX bytes */
    if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6)) {
        (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
    }
    dw_i2c_flush_rx(base);

    if (!(dw_readl(base, DW_IC_STATUS) & (1U << 1) /* TFNF */)) return;

    /* Queue 2 Read commands for the 2-byte I2C-HID length header (DW_apb_i2c stretches SCL after byte 1) */
    dw_writel(base, DW_IC_DATA_CMD, (1U << 8) /* Read byte 0 */);
    dw_writel(base, DW_IC_DATA_CMD, (1U << 8) /* Read byte 1 */);

    int wait = 3000;
    while (--wait > 0) {
        if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6) /* TX_ABRT = NAK (idle) */) {
            (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
            return;
        }
        if (dw_readl(base, DW_IC_RXFLR) >= 2) {
            break;
        }
        io_wait();
    }

    if (dw_readl(base, DW_IC_RXFLR) < 2) {
        /* Terminate transaction cleanly with a STOP read byte */
        dw_writel(base, DW_IC_DATA_CMD, (1U << 8) | (1U << 9));
        for (int i = 0; i < 400; i++) io_wait();
        dw_i2c_flush_rx(base);
        return;
    }

    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)(dw_readl(base, DW_IC_DATA_CMD) & 0xFF);
    buf[1] = (uint8_t)(dw_readl(base, DW_IC_DATA_CMD) & 0xFF);

    uint16_t pkt_len = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (pkt_len < 4 || pkt_len == 0xFFFF || pkt_len > 64) {
        /* Empty report (0x0000): issue 1 dummy read with STOP to release SCL */
        dw_writel(base, DW_IC_DATA_CMD, (1U << 8) | (1U << 9));
        for (int i = 0; i < 400; i++) {
            if (dw_readl(base, DW_IC_STATUS) & (1U << 3)) break;
            io_wait();
        }
        dw_i2c_flush_rx(base);
        return;
    }

    size_t total_to_read = (pkt_len > 30) ? 30 : (size_t)pkt_len;
    size_t tx_queued = 2;
    size_t rx_idx = 2;
    wait = 20000;

    while (rx_idx < total_to_read && --wait > 0) {
        if (dw_readl(base, DW_IC_RAW_INTR_STAT) & (1U << 6)) {
            (void)dw_readl(base, DW_IC_CLR_TX_ABRT);
            break;
        }
        while (rx_idx < total_to_read && (dw_readl(base, DW_IC_STATUS) & (1U << 3) /* RFNE */)) {
            buf[rx_idx++] = (uint8_t)(dw_readl(base, DW_IC_DATA_CMD) & 0xFF);
        }
        while (tx_queued < total_to_read && (tx_queued - rx_idx) < 4 && (dw_readl(base, DW_IC_STATUS) & (1U << 1) /* TFNF */)) {
            uint32_t cmd = (1U << 8);
            if (tx_queued == total_to_read - 1) cmd |= (1U << 9); /* STOP */
            dw_writel(base, DW_IC_DATA_CMD, cmd);
            tx_queued++;
        }
        io_wait();
    }

    uint8_t rep_id = buf[2];
    if (rep_id == g_i2c_tp.mouse_report_id || pkt_len <= 9) {
        uint8_t btns = buf[3] & 0x07;
        int32_t dx = 0;
        int32_t dy = 0;
        int32_t dz = 0;
        if (g_i2c_tp.mouse_xy_16bit && pkt_len >= 8) {
            dx = (int32_t)(int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
            dy = (int32_t)(int16_t)((uint16_t)buf[6] | ((uint16_t)buf[7] << 8));
            if (pkt_len >= 9) dz = (int32_t)(int8_t)buf[8];
        } else {
            dx = (int32_t)(int8_t)buf[4];
            dy = (int32_t)(int8_t)buf[5];
            if (pkt_len >= 7) dz = (int32_t)(int8_t)buf[6];
        }
        if (dx != 0 || dy != 0 || dz != 0 || btns != g_buttons) {
            mice_post_event(dx, dy, dz, btns);
        }
    } else if (pkt_len >= 10) {
        /* Precision Touchpad / Absolute Finger Report (ELAN / ASUE / Synaptics) */
        uint8_t tip_state = buf[3];
        int finger_down = (tip_state & 0x03) != 0;
        uint8_t btns = (rx_idx >= 12 && (buf[10] & 0x01)) ? 1 : ((tip_state & 0x01) ? g_buttons : 0);
        int32_t abs_x = (int32_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
        int32_t abs_y = (int32_t)((uint16_t)buf[6] | ((uint16_t)buf[7] << 8));

        if (finger_down) {
            if (g_i2c_tp.has_prev_abs) {
                int32_t dx = (abs_x - g_i2c_tp.prev_abs_x) / 3;
                int32_t dy = (abs_y - g_i2c_tp.prev_abs_y) / 3;
                if (dx > -120 && dx < 120 && dy > -120 && dy < 120) {
                    mice_post_event(dx, dy, 0, btns);
                }
            }
            g_i2c_tp.prev_abs_x = abs_x;
            g_i2c_tp.prev_abs_y = abs_y;
            g_i2c_tp.has_prev_abs = 1;
        } else {
            g_i2c_tp.has_prev_abs = 0;
            if (btns != g_buttons) {
                mice_post_event(0, 0, 0, btns);
            }
        }
    }
}

/* ============================================================================
 * 8042 PS/2 Auxiliary Port Driver
 * ============================================================================ */
static int mice_wait_write(void) {
    int timeout = 50000;
    while ((inb(PS2_STATUS_PORT) & 0x02) && --timeout > 0) {
        io_wait();
    }
    return timeout > 0;
}

static int mice_wait_read(void) {
    int timeout = 50000;
    while (!(inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        io_wait();
    }
    return timeout > 0;
}

static void mice_write_cmd(uint8_t cmd) {
    mice_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

static void mice_write_data(uint8_t data) {
    mice_wait_write();
    outb(PS2_DATA_PORT, data);
}

static uint8_t mice_read_data(void) {
    if (mice_wait_read()) {
        return inb(PS2_DATA_PORT);
    }
    return 0;
}

static uint8_t mice_send_to_mouse(uint8_t val) {
    mice_write_cmd(0xD4);
    mice_write_data(val);
    return mice_read_data();
}

void mice_set_bounds(uint32_t max_x, uint32_t max_y) {
    if (max_x > 0) g_max_x = max_x - 1;
    if (max_y > 0) g_max_y = max_y - 1;
    if (g_mouse_x > (int32_t)g_max_x) g_mouse_x = (int32_t)g_max_x;
    if (g_mouse_y > (int32_t)g_max_y) g_mouse_y = (int32_t)g_max_y;
}

void mice_init(const FramebufferInfo *fb) {
    uint64_t saved_flags = mice_get_rflags();
    __asm__ volatile ("cli");

    if (fb && fb->width > 0 && fb->height > 0) {
        g_max_x = fb->width - 1;
        g_max_y = fb->height - 1;
        g_mouse_x = (int32_t)(fb->width / 2);
        g_mouse_y = (int32_t)(fb->height / 2);
    }

    g_packet_idx = 0;
    g_queue_head = 0;
    g_queue_tail = 0;

    /* Flush output buffer before probing AUX */
    int timeout = 256;
    while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        inb(PS2_DATA_PORT);
    }

    /* 1. Enable Auxiliary Device */
    mice_write_cmd(0xA8);

    /* 2. Probe if a physical PS/2 AUX mouse/touchpad responds to Set Defaults (0xF6) */
    uint8_t probe_ack = mice_send_to_mouse(0xF6);
    if (probe_ack == 0xFA) {
        /* Real PS/2 AUX device present: enable IRQ 1 + IRQ 12 + Set 1 Translation */
        mice_write_cmd(0x20);
        uint8_t config = mice_read_data();
        if (config == 0 || config == 0xFF || config == 0xFA) {
            config = 0x47;
        }
        config |= 0x43;  /* Bits 0 (KBD IRQ), 1 (AUX IRQ), 6 (Translation) */
        config &= ~0x30; /* Clear bits 4 and 5 (enable both clocks) */
        mice_write_cmd(0x60);
        mice_write_data(config);

        /* IntelliMouse magic sequence */
        mice_send_to_mouse(0xF3); mice_send_to_mouse(200);
        mice_send_to_mouse(0xF3); mice_send_to_mouse(100);
        mice_send_to_mouse(0xF3); mice_send_to_mouse(80);

        mice_send_to_mouse(0xF2);
        uint8_t mouse_id = mice_read_data();
        if (mouse_id == 3 || mouse_id == 4) {
            g_has_wheel = 1;
            g_packet_size = 4;
            klog_info("PS/2 IntelliMouse detected (ID %u, 4-byte wheel packets armed)", mouse_id);
        } else {
            g_has_wheel = 0;
            g_packet_size = 3;
            klog_info("Standard PS/2 mouse detected (ID %u, 3-byte packets armed)", mouse_id);
        }

        /* Set sample rate 100 and enable data reporting (0xF4) */
        mice_send_to_mouse(0xF3);
        mice_send_to_mouse(100);
        mice_send_to_mouse(0xF4);
    } else {
        /* No PS/2 AUX device responded — disable AUX port so EC keyboard is never blocked */
        mice_write_cmd(0xA7);
        mice_write_cmd(0xAE); /* Ensure Port 1 (Keyboard) is enabled */
        mice_write_cmd(0x20);
        uint8_t config = mice_read_data();
        if (config == 0 || config == 0xFF || config == 0xFA) {
            config = 0x45;
        }
        config |= 0x41;  /* Bit 0: KBD IRQ, Bit 6: Set 1 Translation */
        config &= ~0x12; /* Bit 4=0 (KBD clock on), Bit 1=0 (AUX IRQ off) */
        mice_write_cmd(0x60);
        mice_write_data(config);

        /* Re-affirm Keyboard Scanning (0xF4) on Port 1 */
        mice_write_data(0xF4);
        (void)mice_read_data();
        klog_info("PS/2 AUX port not present (ack=0x%02x); EC keyboard port kept active", probe_ack);
    }

    /* Flush any leftover ACK bytes in controller buffer */
    timeout = 256;
    while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        inb(PS2_DATA_PORT);
    }

    if (saved_flags & 0x200) {
        __asm__ volatile ("sti");
    }

    /* 6. Probe AMD FCH DesignWare I2C-HID Precision Touchpad (ASUS Vivobook ASUE/ELAN) */
    i2c_hid_init();

    g_mice_initialized = 1;
    klog_info("Mice subsystem armed (bounds: %ux%u, initial pos: %d,%d)", g_max_x + 1, g_max_y + 1, g_mouse_x, g_mouse_y);
}

void mice_handle_irq(void) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & 0x01)) {
        return;
    }
    if (!(status & 0x20)) {
        /* Not mouse data (keyboard) */
        return;
    }

    uint64_t now = pit_get_ticks();
    if (g_packet_idx > 0 && (now - g_last_byte_tick > 20)) {
        /* Desync recovery: discard stale incomplete packet after 200ms idle */
        g_packet_idx = 0;
    }
    g_last_byte_tick = now;

    uint8_t byte = inb(PS2_DATA_PORT);

    /* Ignore stray command ACKs (0xFA) or BAT (0xAA) at byte 0 */
    if (g_packet_idx == 0 && (byte == 0xFA || byte == 0xAA || !(byte & 0x08))) {
        return;
    }

    g_packet[g_packet_idx++] = byte;

    if (g_packet_idx >= g_packet_size) {
        g_packet_idx = 0;

        uint8_t flags = g_packet[0];
        int32_t dx = (int32_t)g_packet[1];
        int32_t dy = (int32_t)g_packet[2];
        int32_t dz = 0;

        if (flags & 0x10) dx |= ~0xFF;
        if (flags & 0x20) dy |= ~0xFF;

        if (flags & 0xC0) {
            return;
        }

        if (g_has_wheel) {
            int8_t wheel_val = (int8_t)(g_packet[3] & 0x0F);
            if (wheel_val & 0x08) wheel_val |= ~0x0F;
            dz = (int32_t)wheel_val;
            g_scroll_wheel += wheel_val;
        }

        uint8_t btns = flags & 0x07;
        mice_post_event(dx, -dy, dz, btns);
    }
}

void mice_post_event(int32_t dx, int32_t dy, int32_t dz, uint8_t buttons) {
    uint8_t btns = buttons & 0x07;
    uint8_t prev_btns = g_buttons;
    g_buttons = btns;

    int32_t ax = dx;
    int32_t ay = dy;
    int abs_x = ax < 0 ? -ax : ax;
    int abs_y = ay < 0 ? -ay : ay;
    int mult_x = (abs_x > 5) ? 4 : ((abs_x > 2) ? 3 : 2);
    int mult_y = (abs_y > 5) ? 4 : ((abs_y > 2) ? 3 : 2);

    ax *= mult_x;
    ay *= mult_y;

    g_mouse_x += ax;
    g_mouse_y += ay;

    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_x > (int32_t)g_max_x) g_mouse_x = (int32_t)g_max_x;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_y > (int32_t)g_max_y) g_mouse_y = (int32_t)g_max_y;

    mouse_event_t ev;
    ev.x = g_mouse_x;
    ev.y = g_mouse_y;
    ev.dx = dx;
    ev.dy = dy;
    ev.dz = dz;
    ev.buttons = btns;
    ev.reserved = 0;

    if (btns != prev_btns) {
        ev.event_type = MOUSE_EVENT_BUTTON;
    } else if (dz != 0) {
        ev.event_type = MOUSE_EVENT_WHEEL;
    } else {
        ev.event_type = MOUSE_EVENT_MOVE;
    }

    uint8_t next_head = (g_queue_head + 1) % MOUSE_QUEUE_SIZE;
    if (next_head != g_queue_tail) {
        g_event_queue[g_queue_head] = ev;
        g_queue_head = next_head;
    }
}

int mice_get_event(mouse_event_t *ev) {
    extern void ps2_poll_controller(void);
    extern void usb_hid_poll(void);
    ps2_poll_controller();
    usb_hid_poll();
    i2c_hid_poll();

    if (!ev) return 0;
    if (g_queue_head == g_queue_tail) {
        return 0;
    }
    *ev = g_event_queue[g_queue_tail];
    g_queue_tail = (g_queue_tail + 1) % MOUSE_QUEUE_SIZE;
    return 1;
}

void mice_get_state(mouse_state_t *state) {
    if (!state) return;
    state->x = g_mouse_x;
    state->y = g_mouse_y;
    state->buttons = g_buttons;
    state->scroll_wheel = g_scroll_wheel;
    state->max_x = g_max_x;
    state->max_y = g_max_y;
    state->has_wheel = g_has_wheel;
}
