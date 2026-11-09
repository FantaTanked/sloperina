/* Inventory changes through the game's own code (port of patch.py; FINDINGS §3-§5).
 * All of these queue a job (job.h) and return at once; the work runs on the game's thread. */
#ifndef POUCH_H
#define POUCH_H

#include <stdbool.h>
#include <stdint.h>

/* Single calls; only valid on the game's thread (inside a job or the frame hook). */
bool pouch_add_now(const char *item, bool equip_direct);
bool pouch_remove_now(const char *item);
bool ui_refresh_now(uintptr_t vtable_off); /* virtual method 98 of the screen with that vtable */
bool hud_refresh_now(void);
int32_t pouch_stock(const char *item);     /* PouchData.<item>.StockNum, or -1 */

/* Queued operations. */
void pouch_give_here(const char *item);                  /* as a chest does, then HUD redraw */
void pouch_gear(const char *item, bool remove);          /* ocarinas: give / take away for real */
/* The ocarina as one choice: 0 none, 1 Fairy Ocarina, 2 Ocarina of Time. Setting it takes
 * away the other one and gives the chosen one (queued; the newest choice wins). */
extern const char *const ocarina_names[3];
int  pouch_ocarina(void);     /* the choice shown: pending one while queued, else what Link holds */
void pouch_set_ocarina(int choice);
void pouch_unequip(uint32_t str_slot, const char *item); /* take worn gear off Link */
void pouch_refresh_screen(uintptr_t vtable_off);
void pouch_clear_buttons(const char *choice);            /* take an item off the item buttons */
void pouch_clear_buttons_cancel(const char *choice);     /* stop doing so (item given back) */
void pouch_frame(void);                                  /* per-frame upkeep; frame hook only */

/* Upgrades (patch.UPGRADES): the pouch items the game's screens and story checks use. */
typedef enum {
  UPG_STICK, UPG_NUT, UPG_SEED, UPG_BOMB, UPG_QUIVER, UPG_WALLET, UPG_MAGIC,
  UPG_DEFENSE, UPG_BRACELET, UPG_GAUNTLETS, UPG_SCALE, UPG_COUNT
} upgrade_t;

typedef struct {
  const char *name;
  uint32_t    capacity_hash;   /* Int field also written (0 = none) */
  int         n_levels;        /* level 0 = none */
  const char *level_name[4];
  int32_t     capacity[4];
  const char *items[4][2];     /* pouch items held at each level */
} upgrade_def_t;

extern const upgrade_def_t upgrades[UPG_COUNT];
int  upgrade_level(upgrade_t u);            /* from the pouch; -1 if inconsistent/unknown */
void upgrade_set(upgrade_t u, int level);   /* queued; the newest request per upgrade wins */
int  upgrade_pending_level(upgrade_t u);    /* level last asked for while queued, else -1 */

#endif
