#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_FREQUENCY_HZ     100
#define PIT_BASE_FREQUENCY   1193182

void pit_init(uint32_t frequency_hz);
void pit_tick(void);
uint64_t pit_get_ticks(void);
void pit_sleep_ms(uint32_t ms);

#endif /* PIT_H */
