/* 8x8 bitmap font for the on-screen menu (gz's default, Press Start 2P). */
#ifndef FONT_H_INCLUDED
#define FONT_H_INCLUDED

#include <stdint.h>

#define FONT_FIRST 33
#define FONT_COUNT 96
#define FONT_W 8
#define FONT_H 8

/* row bits, MSB = leftmost pixel; characters FONT_FIRST .. FONT_FIRST + FONT_COUNT - 1 */
extern const uint8_t font_8x8[FONT_COUNT][8];

#endif
