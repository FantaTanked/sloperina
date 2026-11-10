#include "pouch.h"

#include "gamedata.h"
#include "job.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

/* The name argument of the pouch functions: {char *top; s32 size = 0x80; char buf[]}
 * (built on the stack exactly like this by the old assembly hook, tools/hook_asm.py). */
typedef struct {
  char   *top;
  int32_t size;
  char    buf[0x80];
} pouch_name_t;

typedef bool (*pouch_add_t)(void *mgr, pouch_name_t *name, int32_t a2, int32_t count, int64_t reason,
                            int32_t a5, int32_t a6, bool equip_direct, bool stack_arg);
typedef void (*pouch_notify_t)(void *target, pouch_name_t *name, bool ok, int64_t a3);
typedef void (*pouch_remove_t)(void *mgr, pouch_name_t *name, int32_t count, int32_t a3, int32_t a4,
                               int32_t a5);
typedef void (*equip_slot_t)(uintptr_t component, int32_t slot);
typedef void (*equip_all_t)(uintptr_t owner, int32_t zero);
typedef void (*ui_method_t)(uintptr_t screen);

static bool same_string(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void set_name(pouch_name_t *name, const char *item)
{
  int i = 0;
  for (; item[i] && i < (int)sizeof(name->buf) - 1; i++)
    name->buf[i] = item[i];
  name->buf[i] = 0;
  name->top = name->buf;
  name->size = sizeof(name->buf);
}

static void *pouch_manager(void)
{
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->pouch_mgr_ptr));
  return (void *)(holder ? sys_ptr(holder) : 0);
}

/* murmur3 of prefix + item + suffix (field names such as "PouchData.<item>.StockNum") */
static uint32_t hash_prefixed(const char *prefix, const char *item, const char *suffix)
{
  char buf[160];
  snprintf(buf, sizeof(buf), "%s%s%s", prefix, item, suffix);
  return murmur3(buf);
}

int32_t pouch_stock(const char *item)
{
  int32_t stock;
  return gd_get_int(hash_prefixed("PouchData.", item, ".StockNum"), &stock) ? stock : -1;
}

/* --- single calls (game thread only) ------------------------------------------------------ */

bool pouch_add_now(const char *item, bool equip_direct)
{
  void *mgr = pouch_manager();
  if (!mgr)
    return false;
  pouch_name_t name;
  set_name(&name, item);
  /* as the OneShotAddOrRemovePouchContent event node calls it (Saria gives the ocarina with
   * IsEquipDirect = false) */
  bool ok = MAIN_FN(pouch_add_t, z64hd_game->pouch_add)(mgr, &name, 0, 1, 0, 0, 0, equip_direct, false) & 1;
  /* the notification, where this version has one (1.0.0 has none: its offsets are 0, and MAIN(0) is the
   * module's own header, not a pointer) */
  uintptr_t target = z64hd_game->pouch_notify && z64hd_game->pouch_notify_ptr
                       ? sys_ptr(MAIN(z64hd_game->pouch_notify_ptr)) : 0;
  if (target)
    MAIN_FN(pouch_notify_t, z64hd_game->pouch_notify)((void *)target, &name, ok, 0);
  log_printf("add %s: %s", item, ok ? "ok" : "refused");
  return ok;
}

bool pouch_remove_now(const char *item)
{
  void *mgr = pouch_manager();
  if (!mgr)
    return false;
  pouch_name_t name;
  set_name(&name, item);
  /* what the event node does for Num < 0 (AddPouch with a negative count does nothing) */
  MAIN_FN(pouch_remove_t, z64hd_game->pouch_remove)(mgr, &name, 1, 0, 0, 0);
  log_printf("remove %s", item);
  return true;
}

/* The UI manager's screens (FINDINGS §4) */
#define UI_MGR_SCREEN_COUNT  0x8   /* u32 */
#define UI_MGR_SCREENS       0x10  /* pointer to an array of screen pointers */
#define UI_MAX_SCREENS       256   /* sanity limit on the count */
#define HUD_DPAD_WIDGET      0x330 /* the HUD screen's D-pad widget pointer */

static uintptr_t ui_manager(void)
{
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->ui_mgr_ptr));
  return holder ? sys_ptr(holder) : 0;
}

/* The first of the UI manager's screens that pick(screen, arg) accepts, or 0. */
static uintptr_t find_screen(bool (*pick)(uintptr_t screen, uintptr_t arg), uintptr_t arg)
{
  uintptr_t ui_mgr = ui_manager();
  uint32_t screen_count;
  if (!ui_mgr || !sys_read32(ui_mgr + UI_MGR_SCREEN_COUNT, &screen_count))
    return 0;
  uintptr_t screens = sys_ptr(ui_mgr + UI_MGR_SCREENS);
  for (uint32_t i = 0; screens && i < screen_count && i < UI_MAX_SCREENS; i++) {
    uintptr_t screen = sys_ptr(screens + 8 * i);
    if (screen && pick(screen, arg))
      return screen;
  }
  return 0;
}

static bool pick_vtable(uintptr_t screen, uintptr_t vtable) { return sys_ptr(screen) == vtable; }

/* the PlayerStatus (HUD) screen: the one holding the D-pad widget */
static bool pick_hud(uintptr_t screen, uintptr_t unused)
{
  (void)unused;
  uintptr_t widget = sys_ptr(screen + HUD_DPAD_WIDGET);
  return widget && sys_ptr(widget) == MAIN(z64hd_game->vt_dpad);
}

static bool call_refresh(uintptr_t screen)
{
  uintptr_t vtable = sys_ptr(screen);
  uintptr_t refresh = vtable ? sys_ptr(vtable + 8 * Z64HD_UI_REFRESH) : 0;
  if (!refresh)
    return false;
  ((ui_method_t)refresh)(screen);
  return true;
}

bool ui_refresh_now(uintptr_t vtable_off)
{
  uintptr_t screen = find_screen(pick_vtable, MAIN(vtable_off));
  return screen && call_refresh(screen);
}

bool hud_refresh_now(void)
{
  uintptr_t screen = find_screen(pick_hud, 0);
  return screen && call_refresh(screen);
}

/* --- queued operations --------------------------------------------------------------------- */

/* A job's key: its kind in the top byte, then 24 bits that tell apart jobs of one kind (usually
 * the item's hash), so a newer request for the same thing replaces the queued one. */
#define JOB_KEY(kind, n) ((uint32_t)(kind) << 24 | ((uint32_t)(n) & 0xFFFFFF))
enum { JOB_GIVE = 1, JOB_GEAR, JOB_UNEQUIP, JOB_REFRESH, JOB_UPGRADE, JOB_OCARINA };

#define EQUIP_SLOT_OCARINA 0xD /* EquipOcarina */

static bool job_give_here(job_t *job)
{
  const char *item = (const char *)job->arg[0];
  if (job->step == 0)
    return !pouch_add_now(item, false);
  hud_refresh_now();
  return true;
}

void pouch_give_here(const char *item)
{
  job_submit(JOB_KEY(JOB_GIVE, murmur3(item)), job_give_here, (intptr_t)item, 0, 0, 0);
}

/* FINDINGS §5.1 / §5.4: the pouch and PlayerStatus.Equip* are only data; Link's usable gear is
 * created by main+0x2df6c80(component, slot) (slot 0xd = EquipOcarina) and taken off by the full
 * re-apply main+0xf201b0(owner, 0) once the slot is empty. Only ocarinas use this.
 * Steps: 0 change the data, 1 create / take off the gear, 2 tidy up and redraw the HUD. */
static bool job_gear(job_t *job)
{
  const char *item = (const char *)job->arg[0];
  bool remove = job->arg[1];
  switch (job->step) {
    case 0: {
      const char *equipped = gd_get_str(GD_STR_OCARINA);
      if (!remove)
        gd_set_str(GD_STR_OCARINA, item);
      else if (equipped && same_string(equipped, item))
        gd_set_str(GD_STR_OCARINA, "");
      if (remove)
        pouch_remove_now(item);
      else
        pouch_add_now(item, false);
      return false;
    }
    case 1:
      if (remove) {
        uintptr_t owner = link_equip_owner();
        if (!owner) {
          log_printf("%s: owner of Link's equipment not found", item);
          return true;
        }
        MAIN_FN(equip_all_t, z64hd_game->equip_all_fn)(owner, 0);
      } else {
        uintptr_t component = link_equip_component();
        if (!component) {
          log_printf("%s: Link's equipment component not found", item);
          return true;
        }
        MAIN_FN(equip_slot_t, z64hd_game->equip_slot_fn)(component, EQUIP_SLOT_OCARINA);
      }
      return false;
    default:
      if (remove)
        gd_set_bool(hash_prefixed("IsGet.", item, ""), false);
      hud_refresh_now();
      log_printf("%s %s: done", remove ? "remove" : "give", item);
      return true;
  }
}

void pouch_gear(const char *item, bool remove)
{
  job_submit(JOB_KEY(JOB_GEAR, murmur3(item)), job_gear, (intptr_t)item, remove, 0, 0);
}

/* --- ocarina choice ---------------------------------------------------------------------------- */

enum { OCARINA_NONE, OCARINA_FAIRY, OCARINA_TIME };
static const char *const ocarina_items[3] = { 0, "Item_Ocarina_Fairy", "Item_Ocarina_Time" };
const char *const ocarina_names[3] = { "none", "fairy ocarina", "ocarina of time" };
static int ocarina_wanted;

static bool ocarina_busy(void)
{
  return job_pending(JOB_KEY(JOB_OCARINA, 0)) ||
         job_pending(JOB_KEY(JOB_GEAR, murmur3(ocarina_items[OCARINA_FAIRY]))) ||
         job_pending(JOB_KEY(JOB_GEAR, murmur3(ocarina_items[OCARINA_TIME])));
}

int pouch_ocarina(void)
{
  if (ocarina_busy())
    return ocarina_wanted;
  if (pouch_stock(ocarina_items[OCARINA_TIME]) > 0)
    return OCARINA_TIME;
  return pouch_stock(ocarina_items[OCARINA_FAIRY]) > 0 ? OCARINA_FAIRY : OCARINA_NONE;
}

/* Decided when it runs (not when chosen), so it starts from what Link holds by then; it
 * queues the removal of the other ocarina and then the give, each the full gear sequence. */
static bool job_ocarina(job_t *job)
{
  int wanted = (int)job->arg[0];
  for (int ocarina = OCARINA_FAIRY; ocarina <= OCARINA_TIME; ocarina++)
    if (ocarina != wanted && pouch_stock(ocarina_items[ocarina]) > 0)
      pouch_gear(ocarina_items[ocarina], true);
  if (wanted && pouch_stock(ocarina_items[wanted]) <= 0)
    pouch_gear(ocarina_items[wanted], false);
  return true;
}

void pouch_set_ocarina(int choice)
{
  if (choice < OCARINA_NONE || choice > OCARINA_TIME)
    return;
  ocarina_wanted = choice;
  job_submit(JOB_KEY(JOB_OCARINA, 0), job_ocarina, choice, 0, 0, 0);
}

/* Steps: 0 empty the slot and re-apply Link's gear, 1 redraw the HUD. */
static bool job_unequip(job_t *job)
{
  uint32_t slot = (uint32_t)job->arg[0];
  const char *item = (const char *)job->arg[1];
  if (job->step == 0) {
    const char *worn = gd_get_str(slot);
    if (!worn || !same_string(worn, item))
      return true;
    gd_set_str(slot, "");
    uintptr_t owner = link_equip_owner();
    if (!owner)
      return true;
    MAIN_FN(equip_all_t, z64hd_game->equip_all_fn)(owner, 0);
    return false;
  }
  hud_refresh_now();
  return true;
}

void pouch_unequip(uint32_t str_slot, const char *item)
{
  job_submit(JOB_KEY(JOB_UNEQUIP, murmur3(item)), job_unequip, str_slot, (intptr_t)item, 0, 0);
}

static bool job_refresh(job_t *job)
{
  ui_refresh_now((uintptr_t)job->arg[0]);
  return true;
}

void pouch_refresh_screen(uintptr_t vtable_off)
{
  job_submit(JOB_KEY(JOB_REFRESH, vtable_off), job_refresh, (intptr_t)vtable_off, 0, 0, 0);
}

/* --- item buttons -------------------------------------------------------------------------- */

/* The four item-button Enums (names not recovered); values are murmur3 of the choice name. */
static const uint32_t button_slots[] = { 0x6BEE602D, 0x76F85E6D, 0xA0029837, 0xBC32CE24 };
#define N_BUTTON_SLOTS (sizeof(button_slots) / sizeof(*button_slots))

/* The item wheel keeps its own copy of the buttons and writes it back when it closes, so a
 * clear is repeated for a while (FINDINGS §5.6). */
#define BUTTON_WATCH_FRAMES (120 * GAME_FPS)
#define BUTTON_WATCHES 8
static struct {
  uint32_t choice; /* murmur3 of the choice to keep off the buttons; 0 = unused */
  uint32_t until;  /* frame_counter() value */
} button_watch[BUTTON_WATCHES];

void pouch_clear_buttons(const char *choice)
{
  uint32_t hash = murmur3(choice);
  int free_slot = -1; /* the watch for this choice if there is one, else the first unused one */
  for (int i = 0; i < BUTTON_WATCHES; i++) {
    if (button_watch[i].choice == hash)
      free_slot = i;
    else if (!button_watch[i].choice && free_slot < 0)
      free_slot = i;
  }
  if (free_slot >= 0) {
    button_watch[free_slot].choice = hash;
    button_watch[free_slot].until = frame_counter() + BUTTON_WATCH_FRAMES;
  }
}

void pouch_clear_buttons_cancel(const char *choice)
{
  uint32_t hash = murmur3(choice);
  for (int i = 0; i < BUTTON_WATCHES; i++)
    if (button_watch[i].choice == hash)
      button_watch[i].choice = 0;
}

void pouch_frame(void)
{
  uint32_t now = frame_counter();
  uint32_t empty = 0; /* murmur3("Empty"), worked out when first needed */
  bool changed = false;
  for (int i = 0; i < BUTTON_WATCHES; i++) {
    if (!button_watch[i].choice)
      continue;
    if ((int32_t)(now - button_watch[i].until) > 0) {
      button_watch[i].choice = 0;
      continue;
    }
    for (unsigned slot = 0; slot < N_BUTTON_SLOTS; slot++) {
      uint32_t on_button;
      if (gd_get_enum(button_slots[slot], &on_button) && on_button == button_watch[i].choice) {
        if (!empty)
          empty = murmur3("Empty");
        gd_set_enum(button_slots[slot], empty);
        changed = true;
      }
    }
  }
  if (changed)
    pouch_refresh_screen(z64hd_game->vt_hud);
}

/* --- upgrades ------------------------------------------------------------------------------ */

/* A bag with three sizes: levels none / small / medium / large, named by capacity, each held as
 * one pouch item (<item_prefix>_Small...). */
#define BAG3(name, hash, small, medium, large, item_prefix)                                             \
  { name, hash, 4, { "none", #small, #medium, #large }, { 0, small, medium, large },                   \
    { { 0 }, { item_prefix "_Small" }, { item_prefix "_Medium" }, { item_prefix "_Large" } } }

const upgrade_def_t upgrades[UPG_COUNT] = {
  [UPG_STICK]  = BAG3("deku stick bag", 0x457616B0, 10, 20, 30, "Item_StickBag"),
  [UPG_NUT]    = BAG3("deku nut bag",   0x735B00F7, 20, 30, 40, "Item_NutBag"),
  [UPG_SEED]   = BAG3("deku seed bag",  0xAE8CC742, 30, 40, 50, "Item_BulletBag"),
  [UPG_BOMB]   = BAG3("bomb bag",       0xE753508B, 20, 30, 40, "Item_BombBag"),
  [UPG_QUIVER] = BAG3("quiver",         0x418DB791, 30, 40, 50, "Item_Quiver"),
  [UPG_WALLET] = BAG3("wallet",         0xE0010A50, 200, 500, 1000, "Item_Wallet"),
  [UPG_MAGIC]  = { "magic meter", 0x12D63A97, 3, { "none", "single", "double" }, { 0, 240, 480 },
                   { { 0 }, { "Item_FairyBless_MP" }, { "Item_FairyBless_MP", "Item_FairyBless_MPDouble" } } },
  [UPG_DEFENSE] = { "double defense", 0, 2, { "off", "on" }, { 0 },
                    { { 0 }, { "Item_FairyBless_DefenseDouble" } } },
  /* strength: the bracelet (child) and the gauntlets (adult) are separate pouch items */
  [UPG_BRACELET] = { "goron's bracelet", 0, 2, { "off", "on" }, { 0 }, { { 0 }, { "Armor_Child_Gauntlet_Goron" } } },
  [UPG_GAUNTLETS] = { "gauntlets", 0, 3, { "none", "silver", "gold" }, { 0 },
                      { { 0 }, { "Armor_Adult_Gauntlet_Silver" }, { "Armor_Adult_Gauntlet_Gold" } } },
  [UPG_SCALE]  = { "scale", 0, 3, { "none", "silver scale", "golden scale" }, { 0 },
                   { { 0 }, { "Item_Scale_Silver" }, { "Item_Scale_Gold" } } },
};

#define ITEMS_PER_LEVEL   2 /* upgrade_def_t.items[level][] */
#define MAX_UPGRADE_ITEMS 8 /* 4 levels x ITEMS_PER_LEVEL */

static int pending_level[UPG_COUNT] = { [0 ... UPG_COUNT - 1] = -1 };

/* every pouch item an upgrade uses, once each */
static int upgrade_items(upgrade_t upgrade, const char **out)
{
  const upgrade_def_t *def = &upgrades[upgrade];
  int n = 0;
  for (int level = 1; level < def->n_levels; level++)
    for (int k = 0; k < ITEMS_PER_LEVEL; k++) {
      const char *item = def->items[level][k];
      bool seen = !item;
      for (int m = 0; item && m < n; m++)
        seen |= same_string(out[m], item);
      if (!seen)
        out[n++] = item;
    }
  return n;
}

static bool level_has(const upgrade_def_t *def, int level, const char *item)
{
  for (int k = 0; k < ITEMS_PER_LEVEL; k++)
    if (def->items[level][k] && same_string(def->items[level][k], item))
      return true;
  return false;
}

/* The level whose items are exactly the ones held; failing that, the level with the most items
 * that are all held; 0 if none of the items is held, -1 if no level fits. */
int upgrade_level(upgrade_t u)
{
  const upgrade_def_t *def = &upgrades[u];
  const char *items[MAX_UPGRADE_ITEMS];
  int n_items = upgrade_items(u, items);
  int best_level = 0;
  int best_item_count = 0;
  bool any_held = false;
  for (int i = 0; i < n_items; i++)
    any_held |= pouch_stock(items[i]) > 0;
  if (!any_held)
    return 0;
  for (int level = 1; level < def->n_levels; level++) {
    bool exact = true;  /* held == this level's items */
    bool subset = true; /* this level's items are all held */
    int level_item_count = 0;
    for (int i = 0; i < n_items; i++) {
      bool held = pouch_stock(items[i]) > 0;
      bool wanted = level_has(def, level, items[i]);
      exact &= held == wanted;
      subset &= !wanted || held;
      level_item_count += wanted;
    }
    if (exact)
      return level;
    if (subset && level_item_count > best_item_count) {
      best_level = level;
      best_item_count = level_item_count;
    }
  }
  return best_level ? best_level : -1;
}

int upgrade_pending_level(upgrade_t u)
{
  return job_pending(JOB_KEY(JOB_UPGRADE, u)) ? pending_level[u] : -1;
}

/* Remove the upgrade's other items, then add the missing ones, one per frame; then write the
 * capacity and redraw the HUD (patch.set_upgrade). job->i counts through the items twice: the
 * first pass removes, the second adds. */
static bool job_upgrade(job_t *job)
{
  upgrade_t upgrade = (upgrade_t)job->arg[0];
  int level = (int)job->arg[1];
  const upgrade_def_t *def = &upgrades[upgrade];
  const char *items[MAX_UPGRADE_ITEMS];
  int n_items = upgrade_items(upgrade, items);
  while (job->i < 2 * n_items) {
    int k = job->i++;
    bool removing_pass = k < n_items;
    const char *item = items[k % n_items];
    bool wanted = level_has(def, level, item);
    int32_t stock = pouch_stock(item);
    if (removing_pass && !wanted && stock > 0) {
      pouch_remove_now(item);
      gd_set_bool(hash_prefixed("IsGet.", item, ""), false);
      return false;
    }
    if (!removing_pass && wanted && stock == 0) {
      pouch_add_now(item, false);
      return false;
    }
  }
  if (def->capacity_hash)
    gd_set_int(def->capacity_hash, def->capacity[level]);
  hud_refresh_now();
  log_printf("%s = %s", def->name, def->level_name[level]);
  return true;
}

void upgrade_set(upgrade_t u, int level)
{
  if (u >= UPG_COUNT || level < 0 || level >= upgrades[u].n_levels)
    return;
  pending_level[u] = level;
  job_submit(JOB_KEY(JOB_UPGRADE, u), job_upgrade, u, level, 0, 0);
}
