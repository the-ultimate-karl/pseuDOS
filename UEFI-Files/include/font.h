#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/* Standard 8x16 VGA Bitmap Font (ASCII 32 to 127) */
extern const uint8_t g_font_8x16[96][16];

#endif /* FONT_H */
