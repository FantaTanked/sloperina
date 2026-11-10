/* Link's age and reloading the map (gz's "age" and "reload scene"). The game keeps the age in
 * Sequence_IsChildhood and most outdoor maps come in a _Child and an _Adult version, so a new age
 * takes effect when a map loads: the age switch only records it, the reload applies it. */
#ifndef AGE_H
#define AGE_H

#include <stdbool.h>

enum { AGE_CHILD, AGE_ADULT };

int age_current(void);   /* the game's, or -1 if unknown */
int age_wanted(void);    /* what the next reload gives (the game's until the switch is changed) */
void age_want(int age);
void age_frame(void);      /* frame hook, after warp_frame */
void age_reload_map(void); /* reload the map at the start point Link last arrived at, at age_wanted */

#endif
