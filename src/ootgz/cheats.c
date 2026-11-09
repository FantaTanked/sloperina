#include "cheats.h"

#include "gamedata.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

const char *const cheat_names[CHEAT_COUNT] = {
  "energy", "rupees", "magic", "deku sticks", "deku nuts", "deku seeds", "bombs", "arrows",
  "bombchus", "magic beans", "2x movement speed",
};
bool cheats[CHEAT_COUNT];

/* Int field hashes (field_hashes.json titles) */
#define HASH_RUPEES          0xA77921D7u
#define HASH_WALLET_CAPACITY 0xE0010A50u
#define HASH_MAGIC           0x76B18200u /* Current magic */
#define HASH_MAGIC_CAPACITY  0x12D63A97u /* Magic capacity */

/* The ammo cheats, CHEAT_STICKS..CHEAT_BEANS: each count is filled to its capacity field, or to a
 * fixed maximum for those without one. */
static const struct {
  uint32_t count_hash;
  uint32_t capacity_hash; /* 0: use fixed_max */
  int32_t fixed_max;
} ammo_fields[] = {
  [CHEAT_STICKS - CHEAT_STICKS]   = { 0xB9494411, 0x457616B0, 0 },
  [CHEAT_NUTS - CHEAT_STICKS]     = { 0xF30D9F54, 0x735B00F7, 0 },
  [CHEAT_SEEDS - CHEAT_STICKS]    = { 0x5FADF7FC, 0xAE8CC742, 0 },
  [CHEAT_BOMBS - CHEAT_STICKS]    = { 0xDB1BCD4F, 0xE753508B, 0 },
  [CHEAT_ARROWS - CHEAT_STICKS]   = { 0x518EC74B, 0x418DB791, 0 },
  [CHEAT_BOMBCHUS - CHEAT_STICKS] = { 0xDF7C517A, 0, 50 },
  [CHEAT_BEANS - CHEAT_STICKS]    = { 0xB003124D, 0, 10 },
};

/* Set an Int field to `target` unless it's already there (SetInt is queued, so skip needless ones). */
static void fill_to(uint32_t field_hash, int32_t target)
{
  int32_t value;
  if (gd_get_int(field_hash, &value) && value != target)
    gd_set_int(field_hash, target);
}

/* The magic meter object holds the real current magic: it writes clamp(value, 0, capacity)
 * into the save field every frame (FINDINGS §1 "Magic"). It holds the two field hashes at
 * +0x38/+0x3C and the value at +0x44. */
#define MAGIC_OBJ_HASHES     0x38 /* u32 magic hash, then u32 capacity hash */
#define MAGIC_OBJ_VALUE      0x44 /* s32 current magic */
#define MAGIC_OBJ_SIZE       0x48 /* what must be mapped from the object's start */
#define MAGIC_SEARCH_FRAMES  150  /* ~5 s between searches while it's missing */

static uintptr_t magic_obj;
static uint32_t magic_next_search; /* frame_counter() value */

/* mem_find_u64 filter: the hashes found are inside a writable magic meter object */
static bool magic_candidate_ok(uintptr_t hashes_addr, void *unused)
{
  (void)unused;
  return hashes_addr >= MAGIC_OBJ_HASHES &&
         sys_mapped(hashes_addr - MAGIC_OBJ_HASHES, MAGIC_OBJ_SIZE, SYS_PERM_R | SYS_PERM_W);
}

/* The meter's current magic, or 0 if the object can't be found (searched for at most every
 * MAGIC_SEARCH_FRAMES). */
static int32_t *magic_value(void)
{
  const uint64_t hashes = (uint64_t)HASH_MAGIC_CAPACITY << 32 | HASH_MAGIC;
  if (magic_obj && sys_mapped(magic_obj + MAGIC_OBJ_HASHES, 8, SYS_PERM_R) &&
      *(uint64_t *)(magic_obj + MAGIC_OBJ_HASHES) == hashes)
    return (int32_t *)(magic_obj + MAGIC_OBJ_VALUE);
  magic_obj = 0;
  uint32_t now = frame_counter();
  if ((int32_t)(now - magic_next_search) < 0)
    return 0;
  magic_next_search = now + MAGIC_SEARCH_FRAMES;
  uintptr_t found = mem_find_u64(hashes, magic_candidate_ok, 0);
  magic_obj = found ? found - MAGIC_OBJ_HASHES : 0;
  return magic_obj ? (int32_t *)(magic_obj + MAGIC_OBJ_VALUE) : 0;
}

void magic_set(int32_t value)
{
  int32_t *meter_value = magic_value();
  if (meter_value)
    *meter_value = value;
  gd_set_int(HASH_MAGIC, value);
}

void cheats_frame(void)
{
  if (!gd_ready())
    return;
  if (cheats[CHEAT_ENERGY]) {
    int32_t *current, *max;
    if (link_health(&current, &max) && *current < *max)
      *current = *max;
  }
  if (cheats[CHEAT_RUPEES]) {
    int32_t wallet;
    if (gd_get_int(HASH_WALLET_CAPACITY, &wallet))
      fill_to(HASH_RUPEES, wallet);
  }
  if (cheats[CHEAT_MAGIC]) {
    int32_t capacity, current;
    if (gd_get_int(HASH_MAGIC_CAPACITY, &capacity) && capacity > 0 && gd_get_int(HASH_MAGIC, &current) &&
        current != capacity) {
      magic_set(capacity);
    }
  }
  for (int cheat = CHEAT_STICKS; cheat <= CHEAT_BEANS; cheat++) {
    if (!cheats[cheat])
      continue;
    int32_t target = ammo_fields[cheat - CHEAT_STICKS].fixed_max;
    uint32_t capacity_hash = ammo_fields[cheat - CHEAT_STICKS].capacity_hash;
    if (capacity_hash && !gd_get_int(capacity_hash, &target))
      continue;
    fill_to(ammo_fields[cheat - CHEAT_STICKS].count_hash, target);
  }
}
