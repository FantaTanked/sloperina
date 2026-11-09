/* Phive's collision layer table: which layers each layer hits (PhiveConfig LayerEntityFilterTable). */
#ifndef LAYERS_H
#define LAYERS_H

#include <stdbool.h>

/* Every frame; while `wanted`, the heap is searched for the table (once, a slice per frame). */
void layers_frame(bool wanted);

/* Whether Link (the Player layer) hits the ground and wall layers: GroundObject, GroundHighRes,
 * GroundHighResSmooth, GroundHighResRough and GroundStaticTree. Off clears those bits in every copy of
 * the table the game built at boot; on puts back what was there. False if the table hasn't been found
 * (yet): nothing changed. */
bool layers_player_hits_ground(bool on);

#endif
