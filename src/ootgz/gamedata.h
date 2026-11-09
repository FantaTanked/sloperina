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

/* String64 field hashes. Their array indices can change when a game update adds fields. */
#define GD_STR_TUNIC    0x008D7F0Cu
#define GD_STR_ARROW    0x3615E379u
#define GD_STR_BOOTS    0xD107A85Du
#define GD_STR_OCARINA  0xB72CD3FCu
#define GD_STR_SHIELD   0xEE463235u
#define GD_STR_SWORD    0xA95A3521u
#define GD_STR_MAP      0x1D6189DAu
#define GD_STR_EVENT    0xA242CCC2u
#define GD_STR_STARTER  0x3AD1FD51u
#define GD_STR_NEXT_MAP 0xEE27BF71u
#define GD_STR_SPAWN    0x0B570AB2u
#define GD_STR_LEN 64
const char *gd_get_str(uint32_t hash); /* hash 0 returns the first string for the memory viewer */
bool        gd_set_str(uint32_t hash, const char *s);

#endif
