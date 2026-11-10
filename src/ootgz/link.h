/* Link, the camera and other game objects (port of game.py). */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { float x, y, z; } vec3f_t;

uintptr_t link_actor(void);
bool      link_health(int32_t **cur, int32_t **max); /* quarter hearts; pointers into the game */
bool      link_drawn_pos(vec3f_t *out);              /* read-only; follows the physics record */
bool      link_facing(double *fx, double *fz);       /* the way he faces, a unit (x, z); 0 = +z */
uintptr_t camera(void);                              /* +0x38 position, +0x44 look-at */
uint32_t  frame_counter(void);

/* Link's equipment component (vtable main+0x4b49698, +0x18 = Link) and the object that
 * holds it at +0x470. Searched near Link first, then across the heap (cached). */
uintptr_t link_equip_component(void);
uintptr_t link_equip_owner(void);

/* Find an 8-byte-aligned u64 `value` in writable memory; calls ok(addr) on each hit until
 * it returns true. Returns the accepted address or 0. Slow (whole heap): cache results. */
uintptr_t mem_find_u64(uint64_t value, bool (*ok)(uintptr_t addr, void *ctx), void *ctx);
/* The same search, at most `budget` bytes a call. Zero the state to start; `done` once the whole heap
 * has been read without a hit. */
typedef struct { uintptr_t pos; bool done; } mem_scan_t;
uintptr_t mem_find_u64_step(mem_scan_t *scan, uint64_t value, bool (*ok)(uintptr_t addr, void *ctx), void *ctx,
                            size_t budget);

#endif
