/* Enemy health: a bar with "life/max" over each living enemy near Link, handed to the render thread as
 * bars (overlayui.c, on the GPU) or as rectangles (nvn.c, gz's style), like the air walls. */
#ifndef ENEMYHP_H
#define ENEMYHP_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h" /* aw_dot_t */

extern bool enemy_hp_shown;
extern bool enemy_hp_xray; /* also for enemies behind the scenery (else their bars fade out) */

/* an enemy, as actors.c reads it */
typedef struct {
  uintptr_t id;        /* its actor: the same enemy from frame to frame */
  float pos[3];        /* where it stands (its feet), world space */
  float height;        /* how far above pos to draw the bar */
  int32_t life, max_life;
} enemy_t;

/* actors.c: the enemies in the scene, up to max */
int enemies_read(enemy_t *out, int max);

/* the colours of the rectangles (nvn.c) */
enum { EH_BACK, EH_FILL, EH_LOW, EH_TEXT, EH_NCOLORS };

/* an enemy's bar for the GPU (the game's style): where over its head (0..1 across the visible picture),
 * its life, and how far it is from the camera (metres); and points up the enemy (feet to bar) on the
 * picture, each with the depth the scene must be nearer than for it to count as hidden there (the
 * point's own, moved toward the camera by a margin; scenedepth.c) */
#define EH_PROBES 4
typedef struct {
  uintptr_t id;
  float x, y, distance;
  int32_t life, max_life;
  int n_probes;
  float probe_x[EH_PROBES], probe_y[EH_PROBES], probe_depth[EH_PROBES];
} eh_bar_t;

/* game thread, once per frame: bars for the GPU, or (gpu false) rectangles */
void enemyhp_frame(bool gpu);
/* render thread: once a frame, before the two after it, takes the newest frame the game thread made (publish.h);
 * enemyhp_release when done with it */
void enemyhp_take(void);
const aw_dot_t *enemyhp_published(int *n);
const eh_bar_t *enemyhp_bars(int *n);
void enemyhp_release(void);

#endif
