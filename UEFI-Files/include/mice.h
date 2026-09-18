#ifndef MICE_H
#define MICE_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

/* Mouse Button Flags */
#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

/* Mouse Event Types */
#define MOUSE_EVENT_MOVE    1
#define MOUSE_EVENT_BUTTON  2
#define MOUSE_EVENT_WHEEL   3

typedef struct {
    int32_t x;          /* Clamped absolute screen X coordinate */
    int32_t y;          /* Clamped absolute screen Y coordinate */
    int32_t dx;         /* Relative X delta */
    int32_t dy;         /* Relative Y delta */
    int32_t dz;         /* Scroll wheel delta (IntelliMouse) */
    uint8_t buttons;    /* Bitmask of pressed buttons (MOUSE_BTN_*) */
    uint8_t event_type; /* MOUSE_EVENT_* */
    uint16_t reserved;
} mouse_event_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
    int8_t  scroll_wheel;
    uint32_t max_x;
    uint32_t max_y;
    int has_wheel;
} mouse_state_t;

void mice_init(const FramebufferInfo *fb);
void mice_handle_irq(void);
int mice_get_event(mouse_event_t *ev);
void mice_get_state(mouse_state_t *state);
void mice_set_bounds(uint32_t max_x, uint32_t max_y);

#endif /* MICE_H */