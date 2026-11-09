/* Enemy health: a bar with "life/max" over each living enemy near Link, drawn as rectangles over the
 * picture (like the air walls). */
#ifndef ENEMYHP_H
#define ENEMYHP_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h" /* aw_dot_t */

extern bool enemy_hp_shown;

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

void enemyhp_frame(void);                     /* game thread, once per frame */
const aw_dot_t *enemyhp_published(int *n);    /* render thread */

#endif
