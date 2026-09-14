#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "idt.h"
#include "process.h"

#define DEFAULT_TIME_SLICE 5 /* 5 ticks at 100Hz = 50ms quantum */

void scheduler_init(void);
void scheduler_start(void);
void scheduler_stop(void);
void scheduler_tick(interrupt_frame_t *frame, registers_t *regs);
void scheduler_schedule(interrupt_frame_t *frame, registers_t *regs);
void scheduler_yield(void);
int scheduler_is_enabled(void);

#endif /* SCHEDULER_H */
