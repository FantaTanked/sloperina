#include "gamedata.h"

#include "sys.h"
#include "z64hd.h"

/* --- hash ------------------------------------------------------------------------------ */

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

/* MurmurHash3_x86_32 with seed 0 (the constants are the algorithm's own). */
uint32_t murmur3(const char *s)
{
  const uint32_t c1 = 0xCC9E2D51, c2 = 0x1B873593;
  uint32_t hash = 0, len = 0;
  while (s[len])
    len++;
  const uint8_t *bytes = (const uint8_t *)s;
  uint32_t i;
  for (i = 0; i + 4 <= len; i += 4) {
    uint32_t block = bytes[i] | bytes[i + 1] << 8 | bytes[i + 2] << 16 | (uint32_t)bytes[i + 3] << 24;
    block = rotl32(block * c1, 15) * c2;
    hash = rotl32(hash ^ block, 13) * 5 + 0xE6546B64;
  }
  uint32_t tail = 0;
  switch (len & 3) {
    case 3: tail ^= bytes[i + 2] << 16; /* fall through */
    case 2: tail ^= bytes[i + 1] << 8;  /* fall through */
    case 1: tail ^= bytes[i];
            hash ^= rotl32(tail * c1, 15) * c2;
  }
  hash ^= len;
  hash = (hash ^ (hash >> 16)) * 0x85EBCA6B;
  hash = (hash ^ (hash >> 13)) * 0xC2B2AE35;
  return hash ^ (hash >> 16);
}

/* --- manager and type entries ---------------------------------------------------------- */

typedef bool (*gd_getint_t)(void *mgr, int32_t *out, uint32_t hash);
typedef void (*gd_setint_t)(void *mgr, int32_t value, uint32_t hash);

/* Where each type's values live in its field objects. */
static const struct {
  uintptr_t vtable_off; /* type entry vtable (main+), 0 = fixed offset */
  uint32_t  stride;     /* object size */
  uint32_t  value_off;  /* value offset in the object; a copy follows it */
} type_layouts[GD_NTYPES] = {
  [GD_INT]   = { 0,                   0x28, 0x1C },
  [GD_BOOL]  = { Z64HD_VT_BOOL_TYPE,  0x18, 0x16 },
  [GD_FLOAT] = { Z64HD_VT_FLOAT_TYPE, 0x28, 0x1C },
  [GD_ENUM]  = { Z64HD_VT_ENUM_TYPE,  0x38, 0x1C },
};

/* sanity limits for a type entry read from memory */
#define MAX_FIELD_COUNT     0x10000
#define MAX_HASH_TABLE_SIZE 0x100000

#define MAIN_IMAGE_SIZE_MAX 0x6000000 /* main's image is smaller than this: vtables lie inside it */
#define MIN_MAP_NAME_LEN    4         /* a real map name is at least this long */

static uint32_t type_entry_off[GD_NTYPES]; /* offset of each type entry in the manager; 0 = not found */
static uintptr_t str_base;                 /* address of String64 index 0's text */
static void *str_mgr;                      /* manager str_base was found for */
static gd_type_t *str_type;                /* the String64 type entry */

void *gd_manager(void)
{
  uintptr_t holder = sys_ptr(MAIN(Z64HD_GD_MGR_PTR));
  return (void *)(holder ? sys_ptr(holder) : 0);
}

void gd_reset(void)
{
  for (int i = 0; i < GD_NTYPES; i++)
    type_entry_off[i] = 0;
  str_base = 0;
  str_mgr = 0;
}

static bool type_plausible(const gd_type_t *type)
{
  if (!sys_mapped((uintptr_t)type, sizeof(*type), SYS_PERM_R))
    return false;
  if (type->count == 0 || type->count >= MAX_FIELD_COUNT || type->table_size == 0 ||
      type->table_size >= MAX_HASH_TABLE_SIZE)
    return false;
  return sys_mapped((uintptr_t)type->table, (size_t)type->table_size * 8, SYS_PERM_R) &&
         sys_mapped((uintptr_t)type->objects, 8, SYS_PERM_R | SYS_PERM_W);
}

/* The manager's type entry for `kind`: Int at its fixed offset, the others found by vtable
 * (the offset is cached). */
static gd_type_t *find_type_entry(gd_kind_t kind)
{
  uint8_t *mgr = gd_manager();
  if (!mgr)
    return 0;
  if (kind == GD_INT)
    return type_plausible((gd_type_t *)(mgr + Z64HD_GD_INT_TYPE)) ?
           (gd_type_t *)(mgr + Z64HD_GD_INT_TYPE) : 0;
  uintptr_t vtable = MAIN(type_layouts[kind].vtable_off);
  if (type_entry_off[kind]) {
    gd_type_t *type = (gd_type_t *)(mgr + type_entry_off[kind]);
    if (type->vtable == vtable && type_plausible(type))
      return type;
    type_entry_off[kind] = 0;
  }
  if (!sys_mapped((uintptr_t)mgr, Z64HD_GD_TYPES_SCAN + sizeof(gd_type_t), SYS_PERM_R))
    return 0;
  for (uint32_t offset = 0; offset < Z64HD_GD_TYPES_SCAN; offset += 8) {
    gd_type_t *type = (gd_type_t *)(mgr + offset);
    if (type->vtable == vtable && type_plausible(type)) {
      type_entry_off[kind] = offset;
      return type;
    }
  }
  return 0;
}

/* The (hash, index) pair for `hash`, probing like GetInt does. */
static uint32_t *gd_lookup(const gd_type_t *type, uint32_t hash)
{
  if (!hash || !type->table_size)
    return 0;
  uint32_t start = hash % type->table_size;
  uint32_t slot = start;
  do {
    uint32_t *pair = type->table + 2 * slot;
    if (pair[0] == hash)
      return pair;
    if (pair[0] == 0)
      return 0;
    slot = slot + 1 >= type->table_size ? 0 : slot + 1;
  } while (slot != start);
  return 0;
}

/* The value of field `hash` of type `kind` (its copy follows it), or 0. */
static uint8_t *gd_value(gd_kind_t kind, uint32_t hash)
{
  gd_type_t *type = find_type_entry(kind);
  if (!type)
    return 0;
  uint32_t *pair = gd_lookup(type, hash);
  if (!pair || pair[1] >= type->count)
    return 0;
  return type->objects + (size_t)pair[1] * type_layouts[kind].stride + type_layouts[kind].value_off;
}

bool gd_ready(void)
{
  gd_type_t *type = find_type_entry(GD_INT);
  return type && sys_ptr((uintptr_t)type->objects) == MAIN(Z64HD_VT_INT_FIELD);
}

/* --- Int ------------------------------------------------------------------------------- */

bool gd_get_int(uint32_t hash, int32_t *out)
{
  void *mgr = gd_manager();
  if (!mgr || !gd_ready())
    return false;
  *out = 0;
  return MAIN_FN(gd_getint_t, Z64HD_GD_GETINT)(mgr, out, hash);
}

void gd_set_int(uint32_t hash, int32_t value)
{
  void *mgr = gd_manager();
  if (mgr && gd_ready())
    MAIN_FN(gd_setint_t, Z64HD_GD_SETINT)(mgr, value, hash);
}

bool gd_poke_int(uint32_t hash, int32_t value)
{
  int32_t *v = (int32_t *)gd_value(GD_INT, hash);
  if (!v)
    return false;
  v[0] = v[1] = value;
  return true;
}

/* --- other types (direct, value + copy, as game.py) -------------------------------------- */

bool gd_get_bool(uint32_t hash, bool *out)
{
  uint8_t *v = gd_value(GD_BOOL, hash);
  if (v)
    *out = v[0] != 0;
  return v != 0;
}

bool gd_set_bool(uint32_t hash, bool value)
{
  uint8_t *v = gd_value(GD_BOOL, hash);
  if (v)
    v[0] = v[1] = value;
  return v != 0;
}

bool gd_get_float(uint32_t hash, float *out)
{
  float *v = (float *)gd_value(GD_FLOAT, hash);
  if (v)
    *out = v[0];
  return v != 0;
}

bool gd_set_float(uint32_t hash, float value)
{
  float *v = (float *)gd_value(GD_FLOAT, hash);
  if (v)
    v[0] = v[1] = value;
  return v != 0;
}

bool gd_get_enum(uint32_t hash, uint32_t *out)
{
  uint32_t *v = (uint32_t *)gd_value(GD_ENUM, hash);
  if (v)
    *out = v[0];
  return v != 0;
}

bool gd_set_enum(uint32_t hash, uint32_t value)
{
  uint32_t *v = (uint32_t *)gd_value(GD_ENUM, hash);
  if (v)
    v[0] = v[1] = value;
  return v != 0;
}

/* --- String64 ---------------------------------------------------------------------------- */

/* Looks like a map name: letters, digits and '_', at least MIN_MAP_NAME_LEN long and terminated. */
static bool plausible_name(const char *s)
{
  int len = 0;
  for (; len < GD_STR_LEN && s[len]; len++) {
    char c = s[len];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return len >= MIN_MAP_NAME_LEN && len < GD_STR_LEN;
}

/* game.py finds the pair (Sequence_CurrentBanc, 0x1F) in the String64 hash table and
 * takes index 0's text at pair + 0x207C - 0x1F * 0x108. Same here, but the table is found
 * among the manager's type entries instead of by scanning the heap. */
static bool find_strings(void)
{
  uint8_t *mgr = gd_manager();
  if (!mgr)
    return false;
  if (str_base && str_mgr == mgr && plausible_name((char *)str_base + GD_STR_MAP * Z64HD_STR_STRIDE))
    return true;
  str_base = 0;
  if (!sys_mapped((uintptr_t)mgr, Z64HD_GD_TYPES_SCAN + sizeof(gd_type_t), SYS_PERM_R))
    return false;
  for (uint32_t offset = 0; offset < Z64HD_GD_TYPES_SCAN; offset += 8) {
    gd_type_t *type = (gd_type_t *)(mgr + offset);
    if (type->vtable < z64hd_main || type->vtable >= z64hd_main + MAIN_IMAGE_SIZE_MAX || !type_plausible(type))
      continue;
    uint32_t *pair = gd_lookup(type, Z64HD_STR_ANCHOR_HASH);
    if (!pair || pair[1] != Z64HD_STR_ANCHOR_IDX)
      continue;
    uintptr_t base = (uintptr_t)pair + Z64HD_STR_FIRST_VALUE - Z64HD_STR_ANCHOR_IDX * Z64HD_STR_STRIDE;
    uintptr_t map_name = base + GD_STR_MAP * Z64HD_STR_STRIDE;
    if (sys_mapped(map_name, GD_STR_LEN, SYS_PERM_R | SYS_PERM_W) && plausible_name((char *)map_name)) {
      str_base = base;
      str_mgr = mgr;
      str_type = type;
      return true;
    }
  }
  return false;
}

const char *gd_get_str(int idx)
{
  if (!find_strings())
    return 0;
  return (const char *)(str_base + (uintptr_t)idx * Z64HD_STR_STRIDE);
}

bool gd_set_str(int idx, const char *s)
{
  if (!find_strings())
    return false;
  char *dest = (char *)(str_base + (uintptr_t)idx * Z64HD_STR_STRIDE);
  int i = 0;
  for (; i < GD_STR_LEN - 1 && s[i]; i++)
    dest[i] = s[i];
  for (; i < GD_STR_LEN; i++) /* zero the rest of the field */
    dest[i] = 0;
  return true;
}

int gd_str_index(uint32_t hash)
{
  if (!find_strings() || !str_type)
    return -1;
  uint32_t *pair = gd_lookup(str_type, hash);
  return pair && pair[1] < str_type->count ? (int)pair[1] : -1;
}

/* --- the game's pause ---------------------------------------------------------------------------- */

#define GD_SEQUENCE_IS_PAUSE 0x604AEBE1u                   /* Sequence_IsPause (bool) */
#define GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT 0x64A61098u   /* Sequence_GamePauseRequestCount (int) */

bool gd_game_paused(bool *is_pause, int32_t *pause_requests)
{
  bool pause = false;
  int32_t requests = 0;
  gd_get_bool(GD_SEQUENCE_IS_PAUSE, &pause);
  gd_get_int(GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT, &requests);
  if (is_pause)
    *is_pause = pause;
  if (pause_requests)
    *pause_requests = requests;
  return pause || requests > 0;
}
