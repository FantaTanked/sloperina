/* ASTC 4x4 blocks (LDR) to RGBA, for the game's item icons (astc.c) */
#ifndef ASTC_H
#define ASTC_H

#include <stdbool.h>
#include <stdint.h>

/* one 16-byte block to its 4x4 texels, row by row, RGBA */
void astc_block(const uint8_t block[16], uint8_t out[16][4]);

#endif
