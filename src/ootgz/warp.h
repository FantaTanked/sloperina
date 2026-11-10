/* Warps (port of game.py arm_warp): the next exit Link takes leads to the chosen place. */
#ifndef WARP_H
#define WARP_H

#include <stdbool.h>

#include "data.h"

extern bool warp_arrival_cutscenes; /* use the spawn's own arrival instead of the standard one */

void warp_arm(const warp_map_t *map, const spawn_t *spawn);
void warp_cancel(void);
void warp_frame(void);              /* frame hook */
const char *warp_state(void);       /* for the warps menu */
bool warp_pending(void);            /* requested or armed, not yet happened */
bool warp_abandoned(void);          /* cancelled or timed out before Link left the map */

#endif
