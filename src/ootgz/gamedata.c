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
  uint32_t  stride;     /* object size */
  uint32_t  value_off;  /* value offset in the object; a copy follows it */
} type_layouts[GD_NTYPES] = {
  [GD_INT]   = { 0x28, 0x1C },
  [GD_BOOL]  = { 0x18, 0x16 },
  [GD_FLOAT] = { 0x28, 0x1C },
  [GD_ENUM]  = { 0x38, 0x1C },
};

/* sanity limits for a type entry read from memory */
#define MAX_FIELD_COUNT     0x10000
#define MAX_HASH_TABLE_SIZE 0x100000


static uint32_t type_entry_off[GD_NTYPES]; /* offset of each type entry in the manager; 0 = not found */

void *gd_manager(void)
{
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->gd_mgr_ptr));
  return (void *)(holder ? sys_ptr(holder) : 0);
}

void gd_reset(void)
{
  for (int i = 0; i < GD_NTYPES; i++)
    type_entry_off[i] = 0;
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
  const uint32_t vtable_offsets[GD_NTYPES] = {
    [GD_BOOL] = z64hd_game->vt_bool_type,
    [GD_FLOAT] = z64hd_game->vt_float_type,
    [GD_ENUM] = z64hd_game->vt_enum_type,
  };
  uintptr_t vtable = MAIN(vtable_offsets[kind]);
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
  return type && sys_ptr((uintptr_t)type->objects) == MAIN(z64hd_game->vt_int_field);
}

/* --- Int ------------------------------------------------------------------------------- */

bool gd_get_int(uint32_t hash, int32_t *out)
{
  void *mgr = gd_manager();
  if (!mgr || !gd_ready())
    return false;
  *out = 0;
  return MAIN_FN(gd_getint_t, z64hd_game->gd_getint)(mgr, out, hash);
}

void gd_set_int(uint32_t hash, int32_t value)
{
  void *mgr = gd_manager();
  if (mgr && gd_ready())
    MAIN_FN(gd_setint_t, z64hd_game->gd_setint)(mgr, value, hash);
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

/* Follow the same hash table and value pointer as the game's GetString64. */
static char *string_value(uint32_t hash)
{
  uint8_t *mgr = gd_manager();
  if (!mgr)
    return 0;
  const gd_type_t *t = (const gd_type_t *)(mgr + Z64HD_GD_STRING_TYPE);
  if (!type_plausible(t))
    return 0;
  uint32_t index = 0;
  if (hash) {
    const uint32_t *e = gd_lookup(t, hash);
    if (!e || e[1] >= t->count)
      return 0;
    index = e[1];
  }
  uintptr_t field = (uintptr_t)t->objects + (size_t)index * Z64HD_STR_STRIDE + Z64HD_STR_VALUE;
  uintptr_t text = sys_ptr(field);
  uint32_t capacity;
  if (!text || !sys_read32(field + sizeof(uintptr_t), &capacity) || capacity < GD_STR_LEN ||
      !sys_mapped(text, GD_STR_LEN, SYS_PERM_R | SYS_PERM_W))
    return 0;
  return (char *)text;
}

const char *gd_get_str(uint32_t hash)
{
  return string_value(hash);
}

bool gd_set_str(uint32_t hash, const char *s)
{
  char *d = hash ? string_value(hash) : 0;
  if (!d)
    return false;
  int i = 0;
  for (; i < GD_STR_LEN - 1 && s[i]; i++)
    d[i] = s[i];
  for (; i < GD_STR_LEN; i++)
    d[i] = 0;
  return true;
}
