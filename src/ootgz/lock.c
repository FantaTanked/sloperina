/* Locks (1.1.1). The patch redirects the game's stores to Link's position (z64hd.h) to stubs that skip or
 * change a store. Setting a bit holds that axis where the game would move it.
 * The module still writes Link itself (save / load position, levitate)
 * so a locked Link stays where the lock was put.
 *
 * Two sources set bits. The menu's locks stay until let go or the map changes, since a warp or a loading
 * zone sets Link's position through the same stores. lock_hold is save / load position's, while a load
 * settles. Any lock also freezes the collision resolver's stores, or the collision would push him back. */
#include "lock.h"

#include "gamedata.h"
#include "gfx.h"
#include "z64hd.h"

static bool menu_axis[3]; /* the menu's locks, by lock_axis_t */
static bool hold;         /* a position load is settling */
static char lock_map[64]; /* the map the menu's locks were set in */

static bool same_str(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static bool any_menu_lock(void) { return menu_axis[LOCK_X] || menu_axis[LOCK_Y] || menu_axis[LOCK_Z]; }

bool lock_available(void) { return z64hd_game->pair != 0 && z64hd_game->lock_slot != 0; }

bool lock_is_set(lock_axis_t axis) { return menu_axis[axis]; }

static void apply(void)
{
  uint64_t word = 0;
  if (menu_axis[LOCK_X] || hold)
    word |= 1u << LOCK_BIT_X;
  if (menu_axis[LOCK_Y] || hold)
    word |= 1u << LOCK_BIT_Y;
  if (menu_axis[LOCK_Z] || hold)
    word |= 1u << LOCK_BIT_Z;
  if (word)
    word |= 1u << LOCK_BIT_COLLISION;
  *(volatile uint64_t *)MAIN(z64hd_game->lock_slot) = word;
}

void lock_set(lock_axis_t axis, bool on)
{
  if (!lock_available())
    return;
  if (on && !any_menu_lock()) {
    const char *m = gd_get_str(GD_STR_MAP);
    uint32_t i = 0;
    for (; m && m[i] && i + 1 < sizeof(lock_map); i++)
      lock_map[i] = m[i];
    lock_map[i] = 0;
  }
  menu_axis[axis] = on;
  apply();
}

void lock_hold_set(bool on)
{
  if (!lock_available() || hold == on)
    return;
  hold = on;
  apply();
}

void lock_frame(void)
{
  if (!any_menu_lock())
    return;
  const char *m = gd_get_str(GD_STR_MAP);
  if (m && !same_str(m, lock_map)) {
    for (int a = 0; a < 3; a++)
      menu_axis[a] = false;
    apply();
    gfx_notice("locks released: the map changed");
  }
}
