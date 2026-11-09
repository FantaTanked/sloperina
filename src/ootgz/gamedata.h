/* GameData: the live save fields, keyed by MurmurHash3_32 (seed 0) of the field name.
 * Port of game.py's field access, done in-process (FINDINGS §1). */
#ifndef GAMEDATA_H
#define GAMEDATA_H

#include <stdbool.h>
#include <stdint.h>

uint32_t murmur3(const char *s);

/* A type entry inside the GameData manager (0x60 bytes; the Int one is at manager+0xC8).
 * Layout from GameData GetInt (main+0x163d20). */
typedef struct {
  uintptr_t vtable;     /* 0x00 */
  uint32_t  count;      /* 0x08 */
  uint32_t  _0c;
  uint8_t  *objects;    /* 0x10: count objects, `stride` bytes apart */
  uint32_t *table;      /* 0x18: (hash, index) pairs, open addressing, hash 0 = empty */
  uint32_t  _20;
  uint32_t  table_size; /* 0x24 */
} gd_type_t;

typedef enum { GD_INT, GD_BOOL, GD_FLOAT, GD_ENUM, GD_NTYPES } gd_kind_t;

void     *gd_manager(void);
bool      gd_ready(void);           /* manager present and the Int fields look valid */
void      gd_reset(void);           /* forget cached type entries (call on save load) */

/* Int: through the game's own GetInt/SetInt (SetInt is queued and applied at the flush). */
bool      gd_get_int(uint32_t hash, int32_t *out);
void      gd_set_int(uint32_t hash, int32_t value);
/* Int, written directly with its copy (applies at once, like game.py set_field). */
bool      gd_poke_int(uint32_t hash, int32_t value);

bool      gd_get_bool(uint32_t hash, bool *out);
bool      gd_set_bool(uint32_t hash, bool value);
bool      gd_get_float(uint32_t hash, float *out);
bool      gd_set_float(uint32_t hash, float value);
bool      gd_get_enum(uint32_t hash, uint32_t *out);  /* value = murmur3(choice) */
bool      gd_set_enum(uint32_t hash, uint32_t value);

/* String64 fields by index (game.py STR_IDX). */
enum {
  GD_STR_TUNIC = 0x10, GD_STR_ARROW = 0x11, GD_STR_BOOTS = 0x12, GD_STR_OCARINA = 0x15,
  GD_STR_SHIELD = 0x16, GD_STR_SWORD = 0x17, GD_STR_MAP = 0x1F, GD_STR_EVENT = 0x20,
  GD_STR_STARTER = 0x21, GD_STR_NEXT_MAP = 0x22, GD_STR_SPAWN = 0x23,
};
#define GD_STR_LEN 64
const char *gd_get_str(int idx);           /* NULL if not found */
bool        gd_set_str(int idx, const char *s);
int         gd_str_index(uint32_t hash);   /* a String64 field's index by name hash, or -1 */

/* The game's own pause (its pause screen or an item menu): Sequence_IsPause, or a pause request
 * counted in Sequence_GamePauseRequestCount. Frame advance holds the game without setting either.
 * Either out pointer may be 0. */
bool        gd_game_paused(bool *is_pause, int32_t *pause_requests);

#endif
