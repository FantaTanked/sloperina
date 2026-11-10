#ifndef CCT_H
#define CCT_H

#include "link.h"

/* Temporary collision bypass for Link only. All operations resolve the current
 * controller again; a lost/replaced controller fails closed without stale writes. */
bool cct_noclip_begin(void);
bool cct_noclip_move(const vec3f_t *position);
void cct_noclip_end(void);
bool cct_noclip_active(void);

/* Resolve on every call. These work with collision enabled as well. */
bool cct_reset_vertical_velocity(uintptr_t expected_actor);
bool cct_on_ground(uintptr_t expected_actor, bool *grounded);
bool cct_clear_fall_distance(uintptr_t expected_actor);

/* The handle of Link's body in the physics world's body entries; 0 while it isn't known. */
uint64_t cct_body_handle(void);

#endif
