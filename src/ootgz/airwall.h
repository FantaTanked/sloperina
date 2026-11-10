/* Air walls (port of airwall.py): which walls are in the current map, whether Link is inside one,
 * and their outlines projected with the game's camera, handed to the render thread to draw: as lines
 * (overlayui.c, on the GPU) or as dots and short rectangles (nvn.c's cleared rectangles, gz's style). */
#ifndef AIRWALL_H
#define AIRWALL_H

#include <stdbool.h>
#include <stdint.h>

extern bool airwalls_shown; /* draw them */
extern bool airwalls_all;   /* also the kinds that don't stop Link (enemies, sight, objects) */

/* a piece of an outline: a rectangle of the visible picture, 0..65535 each way, colour index. A side
 * thinner than the line width (a dot, or a straight piece of a line) is widened to it when drawn. */
enum { AW_BLOCKS, AW_INSIDE, AW_OTHER, AW_NCOLORS };
typedef struct { uint16_t x0, y0, x1, y1; uint8_t color; } aw_dot_t;

/* the same outlines as lines, for the GPU (the game's style): ends 0..1 across the visible picture, the
 * nearer end's distance from the camera (metres), colour index */
typedef struct { float x0, y0, x1, y1, near; uint8_t color; } aw_line_t;

/* game thread, once per frame: lines for the GPU, or (gpu false) rectangles */
void airwall_frame(bool gpu);
/* render thread: once a frame, before the two after it, takes the newest frame the game thread made (publish.h);
 * airwall_release when done with it */
void airwall_take(void);
const aw_dot_t *airwall_published(int *n);
const aw_line_t *airwall_lines(int *n);
void airwall_release(void);
void airwall_summary(char *buf, int len);     /* "12 walls, 3 within 60" */
void airwall_link_text(char *buf, int len);   /* the air wall watch: depth inside, or "outside" */

#endif
