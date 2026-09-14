#include "pit.h"
#include "io.h"
#include "scheduler.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND_PORT  0x43

static volatile uint64_t g_pit_ticks = 0;

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 100;
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;

    /* Channel 0, access lobyte/hibyte, Mode 2 (rate generator), 16-bit binary */
    outb(PIT_COMMAND_PORT, 0x34);
    io_wait();
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
    io_wait();
}

void pit_tick(void) {
    g_pit_ticks++;
}

uint64_t pit_get_ticks(void) {
    return g_pit_ticks;
}

void pit_sleep_ms(uint32_t ms) {
    uint64_t target = g_pit_ticks + (ms + 9) / 10;
    while (g_pit_ticks < target) {
        if (scheduler_is_enabled()) {
            scheduler_yield();
        } else {
            __asm__ volatile ("sti\nhlt");
        }
    }
}

