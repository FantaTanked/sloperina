/* Locks: Link's X (horizontal), Y (height) and Z (horizontal) held where the game writes them
 * (1.1.1; the patch's store stubs, z64hd.h). */
#ifndef LOCK_H
#define LOCK_H

#include <stdbool.h>

typedef enum { LOCK_X, LOCK_Y, LOCK_Z } lock_axis_t;

bool lock_available(void);             /* the patch has the store stubs (1.1.1) */
bool lock_is_set(lock_axis_t axis);
void lock_set(lock_axis_t axis, bool on);
void lock_hold_set(bool on);
void lock_frame(void);                 /* the menu's locks let go when the map changes */

#endif
