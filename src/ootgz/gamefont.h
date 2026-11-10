/* The game's own UI fonts, loaded from its RomFS at start-up and rasterised with stb_truetype:
 * Clarimo UD PE (the menus' type) and External (the button glyphs: A, B, ZR, the sticks...).
 * Nothing of them is in this repository or the release: they're the user's own game files. */
#ifndef GAMEFONT_H
#define GAMEFONT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  GF_REGULAR, /* Clarimo UD PE */
  GF_BOLD,    /* Clarimo UD PE DemiBold */
  GF_GLYPHS,  /* External: the button glyphs, in the private use area from U+E040 */
  GF_FACES
} gf_face_t;

/* External's glyphs (see docs/NATIVE_UI.md for the whole table) */
enum {
  GF_BTN_A = 0xE040, GF_BTN_B, GF_BTN_X, GF_BTN_Y, GF_BTN_L, GF_BTN_R, GF_BTN_ZL, GF_BTN_ZR,
  GF_BTN_POWER, GF_DPAD, GF_HOME, GF_PLUS, GF_MINUS,
  GF_GEAR = 0xE067, /* the game's settings */
  GF_LSTICK_DOWN = 0xE050, GF_RSTICK_DOWN = 0xE051, GF_LSTICK_PRESS = 0xE056, GF_RSTICK_PRESS = 0xE057,
  GF_LSTICK = 0xE05E, GF_RSTICK = 0xE05F,
  GF_DPAD_UP = 0xE060, GF_DPAD_DOWN, GF_DPAD_LEFT, GF_DPAD_RIGHT, GF_DPAD_UP_DOWN, GF_DPAD_LEFT_RIGHT,
  GF_ARROW_RIGHT = 0xE087, GF_ARROW_LEFT = 0xE088, GF_ARROW_UP = 0xE089, GF_ARROW_DOWN = 0xE08A,
  GF_CLOSE = 0xE08F, GF_TRIFORCE = 0xE090,
};

typedef struct {
  const uint8_t *coverage; /* w x h, 0..255 */
  int16_t w, h;
  int16_t x0, y0;          /* the bitmap's top-left from the pen, y0 negative above the baseline */
  int16_t advance;         /* in 1/64 pixel */
} gf_glyph_t;

/* game thread, once (it reads files): true when every face is ready */
bool gamefont_load(void);
bool gamefont_ready(void);

/* render thread. em: the em square in pixels. NULL when the face has no such character, or the cache
 * is full (it starts again at the next gamefont_begin_draw). A glyph stays valid until then. */
const gf_glyph_t *gamefont_glyph(gf_face_t face, int em, uint32_t codepoint);
/* render thread: before drawing a panel (the cache may start afresh here), and after it: false when a
 * glyph was left out for want of room (draw again after gamefont_begin_draw, or another way) */
void gamefont_begin_draw(void);
bool gamefont_draw_complete(void);
/* the em (pixels) whose ascent plus descent is `height` pixels, and an em's ascent (pixels) */
int gamefont_em_for_height(gf_face_t face, int height);
int gamefont_ascent(gf_face_t face, int em);
/* kerning between two characters, 1/64 pixel */
int gamefont_kern(gf_face_t face, int em, uint32_t left, uint32_t right);
/* the pen advance of a string (1/64 pixel) */
int gamefont_measure(gf_face_t face, int em, const char *s, int n);

#endif
