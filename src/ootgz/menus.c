/* The menu tree: the native port of ootmenu.pyw's build_menus. Everything here runs on the
 * game's thread (from the frame hook), so it can read and write GameData directly; changes
 * that need the game's own code go through pouch.c's jobs. */
#include "menus.h"

#include "cheats.h"
#include "data.h"
#include "gamedata.h"
#include "airwall.h"
#include "memview.h"
#include "osk.h"
#include "build_info.h" /* made by the Makefile: BUILD_WHEN, BUILD_VERSION */
#include "age.h"
#include "gfx.h"
#include "levitate.h"
#include "position.h"
#include "link.h"
#include "menu.h"
#include "input.h"
#include "job.h"
#include "pouch.h"
#include "lang.h"
#include "sys.h"
#include "warp.h"
#include "watches.h"
#include "z64hd.h"

#define ARRAY_LEN(array) ((int)(sizeof(array) / sizeof((array)[0])))

/* GameData fields (murmur3 hashes of their names) */
#define F_RUPEES     0xA77921D7u
#define F_WALLET_CAP 0xE0010A50u
#define F_MAGIC      0x76B18200u
#define F_MAGIC_CAP  0x12D63A97u

/* the most choices one of the item switches below can have ("none" and 15 items) */
#define MAX_CHOICES 16

menu_t menu_root;

/* --- small helpers -------------------------------------------------------------------------- */

static int clamp_int(int value, int low, int high)
{
  return value < low ? low : value > high ? high : value;
}

/* the furthest a list of `count` lines, `rows` at a time, can be scrolled */
static int max_top(int count, int rows)
{
  return count > rows ? count - rows : 0;
}

static bool strings_equal(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* what follows `prefix` in `text`, or NULL if `text` doesn't start with it */
static const char *after_prefix(const char *text, const char *prefix)
{
  while (*prefix && *prefix == *text) {
    prefix++;
    text++;
  }
  return *prefix ? NULL : text;
}

/* an empty read-only line */
static void text_static(menu_item_t *it, char *buf, int len);
static void add_blank_line(menu_t *menu)
{
  menu_add_text(menu, text_static, (intptr_t) "");
}

/* --- field helpers -------------------------------------------------------------------------- */

/* A field of any of the simple types, as a number (a bool is 0 or 1). */
static bool field_get(field_t field, int32_t *value)
{
  bool flag;
  uint32_t choice;
  switch (field.type) {
    case FT_INT:
      return gd_get_int(field.hash, value);
    case FT_BOOL:
      if (!gd_get_bool(field.hash, &flag))
        return false;
      *value = flag;
      return true;
    case FT_ENUM:
      if (!gd_get_enum(field.hash, &choice))
        return false;
      *value = (int32_t)choice;
      return true;
    default:
      return false;
  }
}

static void field_set(field_t field, int32_t value)
{
  switch (field.type) {
    case FT_INT:
      gd_set_int(field.hash, value);
      break;
    case FT_BOOL:
      gd_set_bool(field.hash, value != 0);
      break;
    case FT_ENUM:
      gd_set_enum(field.hash, (uint32_t)value);
      break;
    default:
      break;
  }
}

/* sets an item's IsGet flag (0: it has none) */
static void mark_acquired(uint32_t is_get)
{
  if (is_get)
    gd_set_bool(is_get, true);
}

static int32_t int_or_unknown(uint32_t hash)
{
  int32_t value;
  return gd_get_int(hash, &value) ? value : NUM_UNKNOWN;
}

static uint32_t stock_hash(const char *item)
{
  char field_name[128];
  snprintf(field_name, sizeof(field_name), "PouchData.%s.StockNum", item);
  return murmur3(field_name);
}

static uint32_t is_get_hash(const char *item)
{
  char field_name[128];
  snprintf(field_name, sizeof(field_name), "IsGet.%s", item);
  return murmur3(field_name);
}

/* --- link ------------------------------------------------------------------------------------ */

static menu_t m_link;

#define QUARTERS_PER_HEART 4
#define F_MAX_HEARTS 0xFBE01DA1u /* quarter hearts (game.py INT_HASH) */
#define F_LIFE       0x31AB5580u /* PlayerStatus.Life, quarter hearts */

static void hearts_text(int32_t quarters, char *buf, int len)
{
  static const char *const fractions[] = { "", ".25", ".5", ".75" };
  snprintf(buf, len, "%d%s", (int)(quarters / QUARTERS_PER_HEART), fractions[quarters & 3]);
}

static int32_t get_health(menu_item_t *it)
{
  int32_t *current, *maximum;
  return link_health(&current, &maximum) ? *current : NUM_UNKNOWN;
}

static int32_t max_health(menu_item_t *it)
{
  int32_t *current, *maximum;
  return link_health(&current, &maximum) ? *maximum : NUM_UNKNOWN;
}

static void set_health(menu_item_t *it, int32_t value)
{
  int32_t *current, *maximum;
  if (link_health(&current, &maximum))
    *current = value;
}

static void fill_health(menu_item_t *it)
{
  int32_t *current, *maximum;
  if (link_health(&current, &maximum)) {
    *current = *maximum;
    gfx_notice("health refilled");
  }
}

static void health_text(menu_item_t *it, char *buf, int len)
{
  hearts_text(get_health(it), buf, len);
}

/* Heart containers: Link's max health (his status object), the save fields (max, and current
 * health when it's above the new max), then the HUD redrawn. Held for a second in case the game
 * puts its own values back, which is logged (so we learn which copy it reads). */
#define K_CONTAINERS (0x20u << 24) /* the job's key */
#define MAX_HEART_CONTAINERS 20
#define CONTAINER_HOLD_FRAMES 30
#define CONTAINER_LOG_FRAMES 4 /* frames on which the game's values are logged if they differ from ours */
#define CONTAINER_HUD_FRAME 2  /* the frame (counting from Link found) the HUD is first redrawn */

static int pending_containers; /* shown while the job is queued */

static int32_t get_containers(menu_item_t *it)
{
  int32_t *current, *maximum;
  if (job_pending(K_CONTAINERS))
    return pending_containers;
  return link_health(&current, &maximum) ? *maximum / QUARTERS_PER_HEART : NUM_UNKNOWN;
}

static int32_t max_containers(menu_item_t *it)
{
  return MAX_HEART_CONTAINERS;
}

/* the object a health value is in: two words before it, four from it */
static void log_health_object(const char *what, const int32_t *value)
{
  const uint32_t *words = (const uint32_t *)((const uint8_t *)value - 8);
  log_printf("hearts: %s object %p: %08x %08x | %08x %08x %08x %08x", what, (void *)words, words[0], words[1],
             words[2], words[3], words[4], words[5]);
}

/* job->i counts the frames on which Link was found */
static bool job_containers(job_t *job)
{
  int32_t target = (int32_t)job->arg[0]; /* quarter hearts */
  int32_t *current, *maximum;
  int32_t field, life;
  if (!link_health(&current, &maximum)) {
    if (job->step == 0)
      log_printf("hearts: Link's health not found; will keep trying for a second");
    return job->step > CONTAINER_HOLD_FRAMES;
  }
  bool have_field = gd_get_int(F_MAX_HEARTS, &field);
  bool have_life = gd_get_int(F_LIFE, &life);
  if (job->i == 0) { /* the first frame Link is found */
    log_health_object("current", current);
    log_health_object("max", maximum);
    log_printf("hearts: containers %d -> %d (save field %d), health %d (save field %d)",
               (int)(*maximum / QUARTERS_PER_HEART), (int)(target / QUARTERS_PER_HEART), have_field ? (int)field : -1,
               (int)*current, have_life ? (int)life : -1);
  } else if ((*maximum != target || *current > target || (have_field && field != target) ||
              (have_life && life > target)) &&
             job->i < CONTAINER_LOG_FRAMES) {
    log_printf("hearts: frame %d: the game has max %d, field %d, health %d, field %d (we set %d)", job->step,
               (int)*maximum, have_field ? (int)field : -1, (int)*current, have_life ? (int)life : -1, (int)target);
  }
  job->i++;
  /* written directly (value and its copy), not through the game's SetInt queue: queued sets
   * while scrolling left an earlier value behind */
  *maximum = target;
  if (*current > target)
    *current = target;
  if (!have_field || field != target)
    gd_poke_int(F_MAX_HEARTS, target);
  if (have_life && life > target)
    gd_poke_int(F_LIFE, target);
  if (job->i == CONTAINER_HUD_FRAME || job->step == CONTAINER_HOLD_FRAMES)
    hud_refresh_now();
  return job->step >= CONTAINER_HOLD_FRAMES;
}

static void set_containers(menu_item_t *it, int32_t containers)
{
  if (containers < 1)
    containers = 1;
  pending_containers = containers;
  job_submit(K_CONTAINERS, job_containers, containers * QUARTERS_PER_HEART, 0, 0, 0);
}

static int32_t get_rupees(menu_item_t *it)
{
  return int_or_unknown(F_RUPEES);
}

static int32_t max_rupees(menu_item_t *it)
{
  return int_or_unknown(F_WALLET_CAP);
}

static void set_rupees(menu_item_t *it, int32_t value)
{
  gd_set_int(F_RUPEES, value);
}

static void fill_rupees(menu_item_t *it)
{
  int32_t wallet_cap = max_rupees(it);
  if (wallet_cap != NUM_UNKNOWN) {
    gd_set_int(F_RUPEES, wallet_cap);
    gfx_notice("wallet filled");
  }
}

static const char *const age_names[] = { "child", "adult" };

static int get_age(menu_item_t *it)
{
  return age_wanted();
}

static void set_age(menu_item_t *it, int age)
{
  age_want(age);
}

static void act_reload_map(menu_item_t *it)
{
  gfx_logf("reload map (%s)", age_wanted() == AGE_ADULT ? "adult" : "child");
  age_reload_map();
  menu_close(); /* so the reload can be watched */
}

/* read-only lines; it->data is the text */
static void text_static(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, "%s", (const char *)it->data);
}

/* it->data: 0 x, 1 y, 2 z */
static void text_axis(menu_item_t *it, char *buf, int len)
{
  static const char axis_names[] = "xyz";
  vec3f_t pos;
  if (link_drawn_pos(&pos)) {
    float value = it->data == 0 ? pos.x : it->data == 1 ? pos.y : pos.z;
    snprintf(buf, len, "%c: %.2f", axis_names[it->data], (double)value);
  } else {
    snprintf(buf, len, "%c: -", axis_names[it->data]);
  }
}

static void text_age(menu_item_t *it, char *buf, int len)
{
  int age = age_current();
  snprintf(buf, len, TF("current age: %s"), T(age == AGE_ADULT ? "adult" : age == AGE_CHILD ? "child" : "-"));
}

/* the map as the warps menu names it, with the age */
static void text_map(menu_item_t *it, char *buf, int len)
{
  const char *map_name = gd_get_str(GD_STR_MAP);
  if (!map_name || !map_name[0]) {
    snprintf(buf, len, TF("map: -"));
    return;
  }
  for (int i = 0; i < data_n_warp_maps; i++) {
    const warp_map_t *warp_map = &data_warp_maps[i];
    if (strings_equal(warp_map->name, map_name)) {
      snprintf(buf, len, TF("map: %s%s"), T(warp_map->label),
               warp_map->category == WC_CHILD ? " (child)" : warp_map->category == WC_ADULT ? " (adult)" : "");
      return;
    }
  }
  snprintf(buf, len, TF("map: %s"), map_name);
}

static void act_save_position(menu_item_t *it)
{
  position_save();
}

static void act_load_position(menu_item_t *it)
{
  position_load();
}

static void build_link(void)
{
  menu_init(&m_link, "link", 18);
  /* what can be changed first */
  menu_item_t *health = menu_add_number(&m_link, "health", get_health, set_health, max_health, fill_health, 0);
  if (health) {
    health->text = health_text;
    health->hint = "d-left/right: change by a quarter heart   L: fill";
  }
  menu_item_t *containers =
    menu_add_number(&m_link, "heart containers", get_containers, set_containers, max_containers, 0, 0);
  if (containers)
    containers->num_min = 1;
  menu_add_number(&m_link, "rupees", get_rupees, set_rupees, max_rupees, fill_rupees, 0);
  menu_item_t *age = menu_add_option(&m_link, "age", ARRAY_LEN(age_names), age_names, get_age, set_age, 0);
  if (age)
    age->hint = "takes effect when the map reloads (reload map, below)";
  menu_item_t *reload = menu_add_action(&m_link, "reload map", act_reload_map, 0);
  if (reload)
    reload->hint = "L: reload this map at the age above";
  menu_item_t *save = menu_add_action(&m_link, "save position (R + d-up)", act_save_position, 0);
  if (save)
    save->hint = "L: remember where Link is and which way he faces";
  menu_item_t *load = menu_add_action(&m_link, "load position (R + d-down)", act_load_position, 0);
  if (load)
    load->hint = "L: put Link back there (same map only)";
  menu_add_return(&m_link);
  /* what can only be read, under return after a blank line */
  add_blank_line(&m_link);
  menu_add_heading(&m_link, "position");
  for (int axis = 0; axis < 3; axis++)
    menu_add_text(&m_link, text_axis, axis);
  menu_end_group(&m_link);
  menu_add_text(&m_link, text_age, 0);
  menu_add_text(&m_link, text_map, 0);
}

/* --- warps ------------------------------------------------------------------------------------- */

#define MAX_WARP_MAPS 96
static menu_t m_warps, m_places;
static menu_t m_warp_category[WC_COUNT];
static menu_t m_warp_map[MAX_WARP_MAPS];

static void text_warp_current(menu_item_t *it, char *buf, int len)
{
  const char *map_name = gd_get_str(GD_STR_MAP);
  snprintf(buf, len, TF("current  %s"), map_name ? map_name : "-");
}

static void text_warp_state(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, TF("warp     %s"), warp_state());
}

/* it->data: the spawn (data_spawns), it->data2: its map (data_warp_maps) */
static void act_warp(menu_item_t *it)
{
  gfx_logf("warp to %s", data_warp_maps[it->data2].label);
  warp_arm(&data_warp_maps[it->data2], &data_spawns[it->data]);
  menu_close();
}

static void act_cancel_warp(menu_item_t *it)
{
  warp_cancel();
}

static bool get_arrival(menu_item_t *it)
{
  return warp_arrival_cutscenes;
}

static void set_arrival(menu_item_t *it, bool on)
{
  warp_arrival_cutscenes = on;
}

/* places > a category (child, adult, dungeons ...) > a map > its spawns */
static void build_places(void)
{
  menu_init(&m_places, "places", WC_COUNT + 1);
  int maps_in_category[WC_COUNT] = { 0 };
  for (int i = 0; i < data_n_warp_maps; i++)
    maps_in_category[data_warp_maps[i].category]++;
  for (int cat = 0; cat < WC_COUNT; cat++) {
    menu_init(&m_warp_category[cat], data_warp_categories[cat], maps_in_category[cat] + 1);
    if (maps_in_category[cat])
      menu_add_submenu(&m_places, data_warp_categories[cat], &m_warp_category[cat]);
  }
  menu_add_return(&m_places);
  for (int i = 0; i < data_n_warp_maps && i < MAX_WARP_MAPS; i++) {
    const warp_map_t *warp_map = &data_warp_maps[i];
    menu_init(&m_warp_map[i], warp_map->label, warp_map->n + 1);
    for (int spawn = warp_map->first; spawn < warp_map->first + warp_map->n; spawn++) {
      menu_item_t *item = menu_add_action(&m_warp_map[i], data_spawns[spawn].label, act_warp, spawn);
      if (item)
        item->data2 = i;
    }
    menu_add_return(&m_warp_map[i]);
    menu_add_submenu(&m_warp_category[warp_map->category], warp_map->label, &m_warp_map[i]);
  }
  for (int cat = 0; cat < WC_COUNT; cat++)
    menu_add_return(&m_warp_category[cat]);
}

static void build_warps(void)
{
  menu_init(&m_warps, "warps", 7);
  menu_add_submenu(&m_warps, "places", &m_places);
  menu_add_action(&m_warps, "cancel warp", act_cancel_warp, 0);
  menu_add_check(&m_warps, "arrival cutscenes", get_arrival, set_arrival, 0);
  menu_add_return(&m_warps);
  /* what can only be read, under return after a blank line */
  add_blank_line(&m_warps);
  menu_add_text(&m_warps, text_warp_current, 0);
  menu_add_text(&m_warps, text_warp_state, 0);

  build_places();
}

/* --- cheats ------------------------------------------------------------------------------------ */

static menu_t m_cheats;

static bool get_cheat(menu_item_t *it)
{
  return cheats[it->data];
}

static void set_cheat(menu_item_t *it, bool on)
{
  cheats[it->data] = on;
}

static void build_cheats(void)
{
  menu_init(&m_cheats, "cheats", CHEAT_COUNT + 3);
  menu_add_heading(&m_cheats, "infinite");
  for (int cheat = 0; cheat < CHEAT_COUNT; cheat++) {
    if (cheat == CHEAT_SPEED)
      menu_add_heading(&m_cheats, "player");
    menu_add_check(&m_cheats, cheat_names[cheat], get_cheat, set_cheat, cheat);
  }
  menu_add_return(&m_cheats);
}

/* --- inventory: owned items and gear ------------------------------------------------------------- */

/* it->data: the inv_item_t */
static bool get_owned(menu_item_t *it)
{
  const inv_item_t *item = (const inv_item_t *)it->data;
  int32_t owned;
  return field_get(item->owned, &owned) && owned > 0;
}

/* Ticked: owned + acquired (the item screen needs both). Unticked: also taken off Link if he
 * wears it, and off the item buttons (FINDINGS §5.5, §5.6). */
static void set_owned(menu_item_t *it, bool on)
{
  const inv_item_t *item = (const inv_item_t *)it->data;
  field_set(item->owned, on ? 1 : 0);
  if (on) {
    mark_acquired(item->acquired);
    if (item->button)
      pouch_clear_buttons_cancel(item->button);
  } else {
    if (item->worn_slot >= 0)
      pouch_unequip(item->worn_slot, item->worn_item);
    if (item->button)
      pouch_clear_buttons(item->button);
  }
}

static bool get_acquired(menu_item_t *it)
{
  bool acquired;
  return gd_get_bool(((const inv_item_t *)it->data)->acquired, &acquired) && acquired;
}

static void set_acquired(menu_item_t *it, bool on)
{
  gd_set_bool(((const inv_item_t *)it->data)->acquired, on);
}

/* The four item buttons: Enum fields whose names weren't recovered (FINDINGS §1, "Item buttons"). */
static const uint32_t item_button_fields[] = { 0x6BEE602D, 0x76F85E6D, 0xA0029837, 0xBC32CE24 };

/* Every item button holding one of `old_items` (their button names' hashes) gets `replacement`.
 * True if any did. */
static bool replace_on_item_buttons(const uint32_t *old_items, int n_old, uint32_t replacement)
{
  bool changed = false;
  for (int slot = 0; slot < ARRAY_LEN(item_button_fields); slot++) {
    uint32_t held;
    for (int k = 0; k < n_old; k++)
      if (gd_get_enum(item_button_fields[slot], &held) && held == old_items[k]) {
        gd_set_enum(item_button_fields[slot], replacement);
        changed = true;
      }
  }
  return changed;
}

/* Trade slots and masks: one item at a time; trading switches an item button holding the old one.
 * it->data: the slot's items (pouch_one_t), it->data2: how many. Choice 0 is "none". */
static const char *trade_names[2][MAX_CHOICES]; /* [0] child, [1] adult */
static const char *mask_names[MAX_CHOICES];

static int get_trade(menu_item_t *it)
{
  const pouch_one_t *slot_items = (const pouch_one_t *)it->data;
  int n_items = (int)it->data2;
  for (int i = 0; i < n_items; i++) {
    int32_t stock;
    if (gd_get_int(stock_hash(slot_items[i].item), &stock) && stock > 0)
      return i + 1;
  }
  return 0;
}

static void set_trade(menu_item_t *it, int choice)
{
  const pouch_one_t *slot_items = (const pouch_one_t *)it->data;
  int n_items = (int)it->data2;
  uint32_t old_buttons[MAX_CHOICES];
  int n_old = 0;
  for (int i = 0; i < n_items; i++) {
    int32_t stock;
    if (i + 1 != choice && gd_get_int(stock_hash(slot_items[i].item), &stock) && stock > 0) {
      gd_set_int(stock_hash(slot_items[i].item), 0);
      gd_set_bool(is_get_hash(slot_items[i].item), false);
      old_buttons[n_old++] = murmur3(slot_items[i].button);
    }
  }
  const pouch_one_t *chosen = choice > 0 ? &slot_items[choice - 1] : 0;
  if (chosen) {
    gd_set_int(stock_hash(chosen->item), 1);
    gd_set_bool(is_get_hash(chosen->item), true);
  }
  uint32_t replacement = murmur3(chosen ? chosen->button : "Empty");
  if (replace_on_item_buttons(old_buttons, n_old, replacement))
    pouch_refresh_screen(Z64HD_VT_HUD);
}

/* Owned items that are one choice (hookshot / longshot, the big swords): it->data = the group,
 * it->data2 = its size. One that goes is also taken off Link if he wears it. */
static int get_owned_group(menu_item_t *it)
{
  const inv_item_t *const *group = (const inv_item_t *const *)it->data;
  for (int i = (int)it->data2 - 1; i >= 0; i--) { /* the best one held */
    int32_t owned;
    if (field_get(group[i]->owned, &owned) && owned > 0)
      return i + 1;
  }
  return 0;
}

static void set_owned_group(menu_item_t *it, int choice)
{
  const inv_item_t *const *group = (const inv_item_t *const *)it->data;
  int n_group = (int)it->data2;
  const inv_item_t *chosen = choice > 0 && choice <= n_group ? group[choice - 1] : 0;
  uint32_t replacement = murmur3(chosen && chosen->button ? chosen->button : "Empty");
  bool changed = false;
  for (int i = 0; i < n_group; i++) {
    const inv_item_t *item = group[i];
    int32_t owned;
    bool had = field_get(item->owned, &owned) && owned > 0;
    field_set(item->owned, item == chosen);
    if (item == chosen) {
      mark_acquired(item->acquired);
      if (item->button)
        pouch_clear_buttons_cancel(item->button);
      continue;
    }
    if (had && item->worn_slot >= 0)
      pouch_unequip(item->worn_slot, item->worn_item);
    if (had && item->button) {
      /* an item button holding the old one gets the new one (or nothing), as with trade items */
      pouch_clear_buttons_cancel(item->button);
      uint32_t old_button = murmur3(item->button);
      if (replace_on_item_buttons(&old_button, 1, replacement))
        changed = true;
    }
  }
  if (changed)
    pouch_refresh_screen(Z64HD_VT_HUD);
}

static const inv_item_t *hookshots[2];
static const char *const hookshot_names[] = { "none", "hookshot", "longshot" };
static const inv_item_t *big_swords[3];
static const char *const big_sword_names[] = { "none", "giant's knife", "broken giant's knife", "biggoron's sword" };

/* The Stone of Agony is a pouch item here, Item_AgonyStone, worn in the head row of the equipment
 * screen beside the Lens of Truth (PouchTable: Pa_HeadEquip_00); it detects Gold Skulltulas. Held
 * like the lens: its pouch stock and IsGet. (The "Stone of Agony equipped" flag did nothing.) */
static const inv_item_t agony_stone = {
  "echo gem", IK_OWNED, { 0xF3B14CE2u /* PouchData.Item_AgonyStone.StockNum */, FT_INT },
  0xDD587ED8u /* IsGet.Item_AgonyStone */, NULL, -1, NULL,
};

#define N_OCARINA_CHOICES 3 /* ocarina_names: none, fairy ocarina, ocarina of time */

static int get_ocarina(menu_item_t *it)
{
  return pouch_ocarina();
}

static void set_ocarina(menu_item_t *it, int choice)
{
  /* the D-pad (where the ocarina goes) only exists once the Navi tutorial has been seen */
  if (choice > 0)
    gd_set_bool(data_action_guide_navi, true);
  pouch_set_ocarina(choice);
}

/* --- inventory: amounts ------------------------------------------------------------------------- */

/* it->data: the ammo_t */
static int32_t get_ammo(menu_item_t *it)
{
  return int_or_unknown(((const ammo_t *)it->data)->count);
}

static int32_t max_ammo(menu_item_t *it)
{
  const ammo_t *ammo = (const ammo_t *)it->data;
  return ammo->cap ? int_or_unknown(ammo->cap) : ammo->fixed;
}

static void set_ammo(menu_item_t *it, int32_t value)
{
  const ammo_t *ammo = (const ammo_t *)it->data;
  gd_set_int(ammo->count, value);
  if (value > 0)
    mark_acquired(ammo->acquired);
}

static void fill_ammo(menu_item_t *it)
{
  int32_t cap = max_ammo(it);
  if (cap != NUM_UNKNOWN)
    set_ammo(it, cap);
}

/* it->data: the number_t; it->data2: the maximum, for max_from_data2 */
static int32_t get_number(menu_item_t *it)
{
  return int_or_unknown(((const number_t *)it->data)->hash);
}

static void set_number(menu_item_t *it, int32_t value)
{
  gd_set_int(((const number_t *)it->data)->hash, value);
}

static int32_t max_from_data2(menu_item_t *it)
{
  return (int32_t)it->data2;
}

static int32_t get_magic(menu_item_t *it)
{
  return int_or_unknown(F_MAGIC);
}

static int32_t max_magic(menu_item_t *it)
{
  return int_or_unknown(F_MAGIC_CAP);
}

static void set_magic(menu_item_t *it, int32_t value)
{
  magic_set(value);
}

static void fill_magic(menu_item_t *it)
{
  int32_t cap = max_magic(it);
  if (cap != NUM_UNKNOWN && cap > 0)
    magic_set(cap);
}

/* --- inventory: upgrades, bottles, flags ---------------------------------------------------------- */

/* the level last asked for if still queued, else the pouch's */
static int upgrade_now(upgrade_t upgrade)
{
  int pending = upgrade_pending_level(upgrade);
  return pending >= 0 ? pending : upgrade_level(upgrade);
}

/* it->data: the upgrade_t */
static int get_upgrade(menu_item_t *it)
{
  return upgrade_now((upgrade_t)it->data);
}

/* Link/Abilities/Strength (None, Low, Middle, High): the best of the bracelet and the gauntlets */
static void write_strength(int bracelet, int gauntlets)
{
  int strength = gauntlets > 0 ? 1 + gauntlets : bracelet > 0 ? 1 : 0;
  gd_set_enum(data_strength.hash, data_strength_values[strength]);
}

static void set_upgrade(menu_item_t *it, int level)
{
  upgrade_t upgrade = (upgrade_t)it->data;
  upgrade_set(upgrade, level);
  /* the ability fields game.py also wrote (level < 3: within data.c's tables) */
  if (upgrade == UPG_GAUNTLETS && level < 3)
    write_strength(upgrade_now(UPG_BRACELET), level);
  else if (upgrade == UPG_SCALE && level < 3)
    gd_set_float(data_diving_depth.hash, data_scale_depths[level]);
}

static bool get_bracelet(menu_item_t *it)
{
  return upgrade_now(UPG_BRACELET) > 0;
}

static void set_bracelet(menu_item_t *it, bool on)
{
  upgrade_set(UPG_BRACELET, on);
  write_strength(on, upgrade_now(UPG_GAUNTLETS));
}

static bool get_defense(menu_item_t *it)
{
  return upgrade_now(UPG_DEFENSE) > 0;
}

static void set_defense(menu_item_t *it, bool on)
{
  field_set(data_double_defense.f[0], on);
  upgrade_set(UPG_DEFENSE, on);
}

/* it->data: the bottle (data_bottles); choice 0 is "none", then data_bottle_names */
static const char *bottle_choices[MAX_CHOICES];

static int get_bottle(menu_item_t *it)
{
  const bottle_t *bottle = &data_bottles[it->data];
  int32_t owned;
  uint32_t contents;
  if (!field_get(bottle->own, &owned))
    return -1;
  if (!owned)
    return 0;
  if (!gd_get_enum(bottle->contents, &contents))
    return -1;
  for (int i = 0; i < data_n_bottle_contents; i++)
    if (data_bottle_values[i] == contents)
      return i + 1;
  return -1;
}

static void set_bottle(menu_item_t *it, int choice)
{
  const bottle_t *bottle = &data_bottles[it->data];
  field_set(bottle->own, choice ? 1 : 0);
  gd_set_bool(bottle->available, choice != 0);
  if (choice) {
    gd_set_enum(bottle->contents, data_bottle_values[choice - 1]);
    mark_acquired(data_acquired_bottle);
    mark_acquired(data_bottle_acquired[choice - 1]);
  }
}

/* a toggle over one or two fields; it->data: the flag_t, it->data2: a screen to refresh afterwards */
static bool get_flag(menu_item_t *it)
{
  int32_t value;
  return field_get(((const flag_t *)it->data)->f[0], &value) && value;
}

static void set_flag(menu_item_t *it, bool on)
{
  const flag_t *flag = (const flag_t *)it->data;
  for (int i = 0; i < ARRAY_LEN(flag->f); i++)
    if (flag->f[i].type != FT_NONE)
      field_set(flag->f[i], on);
  if (on)
    mark_acquired(flag->acquired);
  if (it->data2)
    pouch_refresh_screen((uintptr_t)it->data2);
}

static menu_item_t *add_flag(menu_t *menu, const flag_t *flag, uintptr_t refresh_screen)
{
  menu_item_t *item = menu_add_check(menu, flag->label, get_flag, set_flag, (intptr_t)flag);
  if (item)
    item->data2 = (intptr_t)refresh_screen;
  return item;
}

/* --- inventory: the menus ------------------------------------------------------------------------- */

#define MAX_DUNGEONS 16
#define N_SPIRITUAL_STONES 3 /* data_quest_items: the stones, then the medallions */
#define MAX_SKULLTULAS 100
#define MAX_SMALL_KEYS 9

static menu_t m_inventory, m_items, m_child_items, m_adult_items, m_universal, m_ammo, m_equipment, m_collection,
  m_capacity, m_magic, m_quest, m_songs, m_dungeon_items, m_dungeon[MAX_DUNGEONS];

/* An item by its data label (data_items, then data_gear). */
static const inv_item_t *find_item(const char *label)
{
  for (int i = 0; i < data_n_items; i++)
    if (strings_equal(data_items[i].label, label))
      return &data_items[i];
  for (int i = 0; i < data_n_gear; i++)
    if (strings_equal(data_gear[i].label, label))
      return &data_gear[i];
  log_printf("menus: no item \"%s\"", label);
  return 0;
}

/* Add the item with data label `label`, shown as `shown`: a checkbox (acquired / owned), or a switch
 * (trade slot, ocarina). */
static void add_item(menu_t *menu, const char *shown, const char *label)
{
  const inv_item_t *item = find_item(label);
  if (!item)
    return;
  switch (item->kind) {
    case IK_ACQUIRED:
      menu_add_check(menu, shown, get_acquired, set_acquired, (intptr_t)item);
      break;
    case IK_OWNED:
      menu_add_check(menu, shown, get_owned, set_owned, (intptr_t)item);
      break;
    case IK_TRADE_CHILD:
    case IK_TRADE_ADULT: {
      int adult = item->kind == IK_TRADE_ADULT;
      int n_items = adult ? data_n_trade_adult : data_n_trade_child;
      menu_item_t *trade = menu_add_option(menu, shown, n_items + 1, trade_names[adult], get_trade, set_trade,
                                           (intptr_t)(adult ? data_trade_adult : data_trade_child));
      if (trade)
        trade->data2 = n_items;
      break;
    }
    case IK_OCARINA:
      menu_add_option(menu, shown, N_OCARINA_CHOICES, ocarina_names, get_ocarina, set_ocarina, 0);
      break;
  }
}

/* items: child, adult, universal */
static void build_items(void)
{
  menu_init(&m_items, "items", 4);
  menu_add_submenu(&m_items, "child items", &m_child_items);
  menu_add_submenu(&m_items, "adult items", &m_adult_items);
  menu_add_submenu(&m_items, "universal", &m_universal);
  menu_add_return(&m_items);

  menu_init(&m_child_items, "child items", 6);
  add_item(&m_child_items, "deku sticks", "deku sticks");
  add_item(&m_child_items, "boomerang", "boomerang");
  add_item(&m_child_items, "fairy slingshot", "fairy slingshot");
  add_item(&m_child_items, "magic beans", "magic bean");
  add_item(&m_child_items, "child trade item", "child trade");
  menu_add_return(&m_child_items);

  menu_init(&m_adult_items, "adult items", 8);
  add_item(&m_adult_items, "fairy bow", "fairy bow");
  add_item(&m_adult_items, "megaton hammer", "hammer");
  add_item(&m_adult_items, "fire arrow", "arrow fire");
  add_item(&m_adult_items, "ice arrow", "arrow ice");
  add_item(&m_adult_items, "light arrow", "arrow light");
  hookshots[0] = find_item("hookshot");
  hookshots[1] = find_item("longshot");
  menu_item_t *hookshot = menu_add_option(&m_adult_items, "hookshot", ARRAY_LEN(hookshot_names), hookshot_names,
                                          get_owned_group, set_owned_group, (intptr_t)hookshots);
  if (hookshot)
    hookshot->data2 = hookshots[1] ? 2 : 1;
  add_item(&m_adult_items, "adult trade item", "adult trade");
  menu_add_return(&m_adult_items);

  bottle_choices[0] = "none";
  for (int i = 0; i < data_n_bottle_contents && i < MAX_CHOICES - 1; i++)
    bottle_choices[i + 1] = data_bottle_names[i];
  static const char *const bottle_labels[] = { "bottle a", "bottle b", "bottle c", "bottle d", "bottle e" };
  menu_init(&m_universal, "universal", 14);
  add_item(&m_universal, "bombs", "bombs");
  add_item(&m_universal, "bombchus", "bombchus");
  add_item(&m_universal, "deku nuts", "deku nuts");
  add_item(&m_universal, "din's fire", "din's fire");
  add_item(&m_universal, "farore's wind", "farore's wind");
  add_item(&m_universal, "nayru's love", "nayru's love");
  add_item(&m_universal, "ocarina", "ocarina");
  for (int bottle = 0; bottle < ARRAY_LEN(bottle_labels); bottle++)
    menu_add_option(&m_universal, bottle_labels[bottle], data_n_bottle_contents + 1, bottle_choices, get_bottle,
                    set_bottle, bottle);
  menu_add_return(&m_universal);
}

static void build_ammo(void)
{
  menu_init(&m_ammo, "ammo", data_n_ammo + 2);
  for (int i = 0; i < data_n_ammo; i++)
    menu_add_number(&m_ammo, data_ammo[i].label, get_ammo, set_ammo, max_ammo, fill_ammo,
                    (intptr_t)&data_ammo[i]);
  menu_add_return(&m_ammo);
}

static void build_equipment(void)
{
  mask_names[0] = "none";
  for (int i = 0; i < data_n_masks && i < MAX_CHOICES - 1; i++)
    mask_names[i + 1] = data_masks[i].label;
  menu_init(&m_equipment, "equipment items", 21);
  menu_add_heading(&m_equipment, "swords");
  add_item(&m_equipment, "kokiri sword", "kokiri sword");
  add_item(&m_equipment, "master sword", "master sword");
  big_swords[0] = find_item("giant's knife");
  big_swords[1] = find_item("broken giant's knife");
  big_swords[2] = find_item("biggoron's sword");
  if (big_swords[0] && big_swords[1] && big_swords[2]) {
    menu_item_t *big_sword = menu_add_option(&m_equipment, "big sword", ARRAY_LEN(big_sword_names), big_sword_names,
                                             get_owned_group, set_owned_group, (intptr_t)big_swords);
    if (big_sword)
      big_sword->data2 = ARRAY_LEN(big_swords);
  }
  menu_add_heading(&m_equipment, "shields");
  add_item(&m_equipment, "kokiri shield", "deku shield"); /* the game's Weapon_Shield_Deku */
  add_item(&m_equipment, "hylian shield", "hylian shield");
  add_item(&m_equipment, "mirror shield", "mirror shield");
  menu_add_heading(&m_equipment, "tunics");
  add_item(&m_equipment, "goron tunic", "goron tunic");
  add_item(&m_equipment, "zora tunic", "zora tunic");
  menu_add_heading(&m_equipment, "boots");
  add_item(&m_equipment, "iron boots", "iron boots");
  add_item(&m_equipment, "hover boots", "hover boots");
  menu_add_heading(&m_equipment, "other");
  add_item(&m_equipment, "lenses of truth", "lens of truth");
  menu_add_check(&m_equipment, "echo gem", get_owned, set_owned, (intptr_t)&agony_stone);
  menu_item_t *masks = menu_add_option(&m_equipment, "mask slot", data_n_masks + 1, mask_names, get_trade, set_trade,
                                       (intptr_t)data_masks);
  if (masks)
    masks->data2 = data_n_masks; /* one mask at a time, like the trade slots */
  menu_add_return(&m_equipment);
}

static void add_upgrade(menu_t *menu, const char *label, upgrade_t upgrade)
{
  menu_add_option(menu, label, upgrades[upgrade].n_levels, upgrades[upgrade].level_name, get_upgrade, set_upgrade,
                  upgrade);
}

static void build_collection(void)
{
  menu_init(&m_collection, "collection", 15);
  /* checkboxes, then strength, then the switches and numbers */
  add_item(&m_collection, "fairy ocarina", "fairy ocarina"); /* owned flag: the collection screen */
  add_item(&m_collection, "ocarina of time", "ocarina of time");
  add_item(&m_collection, "gerudo card", "gerudo membership card");
  menu_add_check(&m_collection, "double defense", get_defense, set_defense, 0);
  menu_add_heading(&m_collection, "strength");
  menu_add_check(&m_collection, "goron's bracelet", get_bracelet, set_bracelet, 0);
  add_upgrade(&m_collection, "gauntlets", UPG_GAUNTLETS);
  menu_add_heading(&m_collection, "other");
  add_upgrade(&m_collection, "scale", UPG_SCALE);
  menu_item_t *skulltulas = menu_add_number(&m_collection, "skulltulas", get_number, set_number, max_from_data2, 0,
                                            (intptr_t)&data_skulltulas);
  if (skulltulas)
    skulltulas->data2 = MAX_SKULLTULAS;
  menu_add_submenu(&m_collection, "ammo capacity", &m_capacity);
  menu_add_submenu(&m_collection, "magic", &m_magic);
  menu_end_group(&m_collection);
  menu_add_return(&m_collection);

  menu_init(&m_capacity, "ammo capacity", 7);
  for (int upgrade = UPG_STICK; upgrade <= UPG_WALLET; upgrade++)
    add_upgrade(&m_capacity, upgrades[upgrade].name, upgrade);
  menu_add_return(&m_capacity);

  menu_init(&m_magic, "magic", 4);
  menu_item_t *magic = menu_add_number(&m_magic, "magic", get_magic, set_magic, max_magic, fill_magic, 0);
  if (magic)
    magic->hint = "d-left/right: change   L: fill (set the meter first)";
  add_upgrade(&m_magic, upgrades[UPG_MAGIC].name, UPG_MAGIC);
  menu_add_return(&m_magic);
}

static void build_quest_and_songs(void)
{
  menu_init(&m_quest, "quest items", data_n_quest_items + 3);
  for (int i = 0; i < data_n_quest_items; i++) {
    if (i == 0)
      menu_add_heading(&m_quest, "spiritual stones");
    else if (i == N_SPIRITUAL_STONES)
      menu_add_heading(&m_quest, "medallions");
    add_flag(&m_quest, &data_quest_items[i], 0);
  }
  menu_add_return(&m_quest);

  menu_init(&m_songs, "songs", data_n_songs + 3);
  menu_add_heading(&m_songs, "normal songs");
  for (int i = 0; i < data_n_songs; i++) {
    if (strings_equal(data_songs[i].label, "minuet of forest")) /* data.c: the warp songs follow */
      menu_add_heading(&m_songs, "warp songs");
    add_flag(&m_songs, &data_songs[i], 0);
  }
  menu_add_return(&m_songs);
}

/* The dungeon's small keys, if it has any. Their fields are named a little differently from the
 * dungeons. */
static void add_small_keys(menu_t *menu, const char *dungeon)
{
  static const struct { const char *keys, *dungeon; } key_names[] = {
    { "forest temple", "forest temple" }, { "fire temple", "fire temple" }, { "water temple", "water temple" },
    { "shadow temple", "shadow temple" }, { "spirit temple", "spirit temple" },
    { "bottom of the well", "bottom of well" }, { "gerudo training ground", "gerudo training" },
    { "inside ganon castle", "ganon's castle" },
  };
  for (int j = 0; j < ARRAY_LEN(key_names); j++) {
    if (!strings_equal(key_names[j].dungeon, dungeon))
      continue;
    for (int i = 0; i < data_n_small_keys; i++)
      if (strings_equal(data_small_keys[i].label, key_names[j].keys)) {
        menu_item_t *keys = menu_add_number(menu, "small keys", get_number, set_number, max_from_data2, 0,
                                            (intptr_t)&data_small_keys[i]);
        if (keys)
          keys->data2 = MAX_SMALL_KEYS;
      }
  }
}

/* a menu for each dungeon: its map, compass and boss key, then its small keys */
static void build_dungeon_items(void)
{
  menu_init(&m_dungeon_items, "dungeon items", data_n_dungeons + 1);
  for (int d = 0; d < data_n_dungeons && d < MAX_DUNGEONS; d++) {
    menu_init(&m_dungeon[d], data_dungeons[d].label, 5);
    for (int k = 0; k < ARRAY_LEN(data_dungeons[d].items); k++)
      add_flag(&m_dungeon[d], &data_dungeons[d].items[k], Z64HD_VT_MAP_SCREEN);
    add_small_keys(&m_dungeon[d], data_dungeons[d].label);
    menu_add_return(&m_dungeon[d]);
    menu_add_submenu(&m_dungeon_items, data_dungeons[d].label, &m_dungeon[d]);
  }
  menu_add_return(&m_dungeon_items);
}

static void build_inventory(void)
{
  menu_init(&m_inventory, "inventory", 11);
  menu_item_t *items = menu_add_submenu(&m_inventory, "items", &m_items);
  if (items)
    items->hint = "the game's L/R quick menus need 2+ items";
  menu_add_submenu(&m_inventory, "ammo", &m_ammo);
  menu_add_submenu(&m_inventory, "equipment items", &m_equipment);
  menu_add_submenu(&m_inventory, "collection", &m_collection);
  menu_add_submenu(&m_inventory, "quest items", &m_quest);
  menu_add_submenu(&m_inventory, "songs", &m_songs);
  menu_add_submenu(&m_inventory, "dungeon items", &m_dungeon_items);
  menu_add_return(&m_inventory);

  trade_names[0][0] = "none";
  trade_names[1][0] = "none";
  for (int i = 0; i < data_n_trade_child; i++)
    trade_names[0][i + 1] = data_trade_child[i].label;
  for (int i = 0; i < data_n_trade_adult; i++)
    trade_names[1][i + 1] = data_trade_adult[i].label;

  /* the order matters: menu_init reserves each menu's items from one shared pool */
  build_items();
  build_ammo();
  build_equipment();
  build_collection();
  build_quest_and_songs();
  build_dungeon_items();
}

/* --- equips ------------------------------------------------------------------------------------- */

static menu_t m_equips;

/* The game's item names as the inventory menus call them. */
static const struct { const char *game, *shown; } worn_names[] = {
  { "Weapon_Sword_Kokiri", "kokiri sword" }, { "Weapon_Sword_Master", "master sword" },
  { "Weapon_Sword_Giant", "giant's knife" }, { "Weapon_Sword_BrokenGiant", "broken giant's knife" },
  { "Weapon_Sword_Biggoron", "biggoron's sword" }, { "Weapon_Shield_Deku", "kokiri shield" },
  { "Weapon_Shield_Hylian", "hylian shield" }, { "Weapon_Shield_Mirror", "mirror shield" },
  { "Armor_Adult_Goron", "goron tunic" }, { "Armor_Adult_Zora", "zora tunic" },
  { "Armor_Adult_HeavyBoot", "iron boots" }, { "Armor_Adult_HoverBoot", "hover boots" },
  { "Item_Ocarina_Fairy", "fairy ocarina" }, { "Item_Ocarina_Time", "ocarina of time" },
  { "Item_Arrow_Fire", "fire arrow" }, { "Item_Arrow_Ice", "ice arrow" }, { "Item_Arrow_Light", "light arrow" },
};

/* the lines of the equips menu: what Link wears in each slot (it->data indexes both) */
static const char *const worn_slot_names[] = { "sword", "shield", "tunic", "boots", "arrow", "ocarina" };
static const int worn_slot_fields[] = { GD_STR_SWORD, GD_STR_SHIELD, GD_STR_TUNIC, GD_STR_BOOTS, GD_STR_ARROW,
                                        GD_STR_OCARINA };

/* Anything else: drop the Weapon_/Armor_/Item_ prefix and its category, split the words, lower case. */
static void tidy_name(const char *game_name, char *buf, int len)
{
  static const char *const prefixes[] = { "Weapon_Sword_", "Weapon_Shield_", "Weapon_", "Armor_Adult_",
                                          "Armor_Child_", "Armor_", "Item_" };
  for (int i = 0; i < ARRAY_LEN(prefixes); i++) {
    const char *rest = after_prefix(game_name, prefixes[i]);
    if (rest) {
      game_name = rest;
      break;
    }
  }
  int n = 0;
  for (int i = 0; game_name[i] && n < len - 2; i++) { /* room for a space, the letter and the end */
    char c = game_name[i];
    if (c == '_') {
      c = ' ';
    } else if (c >= 'A' && c <= 'Z') {
      if (i && n && buf[n - 1] != ' ') /* a capital starts a new word */
        buf[n++] = ' ';
      c += 'a' - 'A';
    }
    buf[n++] = c;
  }
  buf[n] = 0;
}

static void text_worn(menu_item_t *it, char *buf, int len)
{
  const char *game_name = gd_get_str(worn_slot_fields[it->data]);
  char shown[48];
  if (!game_name || !game_name[0]) {
    snprintf(shown, sizeof(shown), "none");
  } else {
    shown[0] = 0;
    for (int i = 0; i < ARRAY_LEN(worn_names) && !shown[0]; i++)
      if (strings_equal(game_name, worn_names[i].game))
        snprintf(shown, sizeof(shown), "%s", worn_names[i].shown);
    if (!shown[0])
      tidy_name(game_name, shown, sizeof(shown));
  }
  snprintf(buf, len, "%-9s %s", worn_slot_names[it->data], shown);
}

/* what Link wears (owned gear is in inventory > equipment items) */
static void build_equips(void)
{
  menu_init(&m_equips, "equips", 7);
  for (int slot = 0; slot < ARRAY_LEN(worn_slot_names); slot++)
    menu_add_text(&m_equips, text_worn, slot);
  menu_add_return(&m_equips);
}

/* --- story ------------------------------------------------------------------------------------------ */

static menu_t m_story;

/* it->data: the quest (data_quests); a choice is a stage */
static int get_stage(menu_item_t *it)
{
  const quest_t *quest = &data_quests[it->data];
  uint32_t value;
  if (!gd_get_enum(quest->hash, &value))
    return -1;
  for (int stage = 0; stage < quest->n_stages; stage++)
    if (murmur3(quest->stages[stage]) == value)
      return stage;
  return -1;
}

static void set_stage(menu_item_t *it, int stage)
{
  const quest_t *quest = &data_quests[it->data];
  gd_set_enum(quest->hash, murmur3(quest->stages[stage]));
}

/* the stage's name, lower case */
static void stage_text(menu_item_t *it, int stage, char *buf, int len)
{
  const char *name = data_quests[it->data].stages[stage];
  int n = 0;
  for (; name[n] && n < len - 1; n++)
    buf[n] = (name[n] >= 'A' && name[n] <= 'Z') ? name[n] + ('a' - 'A') : name[n];
  buf[n] = 0;
}

static void act_tutorials(menu_item_t *it)
{
  int marked = 0;
  for (int i = 0; i < data_n_action_guides; i++)
    marked += gd_set_bool(data_action_guides[i], true);
  char message[48];
  snprintf(message, sizeof(message), "%d tutorials marked seen", marked);
  gfx_notice(message);
}

static void build_story(void)
{
  menu_init(&m_story, "story", data_n_quests + 4);
  menu_add_action(&m_story, "all tutorials seen", act_tutorials, 0);
  for (int i = 0; i < data_n_quests; i++) {
    menu_item_t *quest = menu_add_option(&m_story, data_quests[i].label, data_quests[i].n_stages, 0, get_stage,
                                         set_stage, i);
    if (quest) {
      quest->choice_text = stage_text;
      quest->hint = "d-left/right or L: change stage (applies after a loading zone)";
    }
  }
  menu_add_return(&m_story);
}

/* --- watches: the box ---------------------------------------------------------------------------- */

static menu_t m_watches, m_watch_add;
#define WATCH_ROWS 5 /* watches shown at once; gz's arrows scroll the rest */
static int watch_top;                /* the first watch shown */
static int watch_first_row;          /* the menu item showing it */
static uint32_t watches_built = ~0u; /* the watch_version the menu shows; ~0u: rebuild it */

static bool get_shown(menu_item_t *it)
{
  return watches_shown;
}

static void set_shown(menu_item_t *it, bool on)
{
  watches_shown = on;
}

static int get_corner(menu_item_t *it)
{
  return watches_corner;
}

static void set_corner(menu_item_t *it, int corner)
{
  watches_corner = corner;
}

/* move: the watches go to "custom" and the d-pad moves them, shown live; L leaves them there.
 * They glide while the d-pad is held (in thousandths of the picture per frame, faster the longer it's
 * held; a tap moves them one), rather than jumping on the d-pad's repeat. */
#define WATCHES_POS_MAX 1000 /* positions are thousandths of the picture */
#define WATCHES_ROUNDING 3   /* how far nvn.c's rounded copy of the position can drift from ours */
static int move_held;        /* frames the d-pad has been held */

/* how far the watches move this frame */
static int glide_step(int frames_held)
{
  if (frames_held == 0)
    return 0;
  if (frames_held < 10)
    return 1;
  if (frames_held < 30)
    return 3;
  if (frames_held < 60)
    return 6;
  return 10;
}

static bool move_watches(menu_item_t *it)
{
  watches_moving = true;
  /* nvn.c keeps them whole on screen: follow that only when it really pushed them back (its value
   * is rounded, so taking it every frame made them creep) */
  if (gfx_custom_x >= 0 &&
      (gfx_custom_x - watches_x > WATCHES_ROUNDING || watches_x - gfx_custom_x > WATCHES_ROUNDING))
    watches_x = gfx_custom_x;
  if (gfx_custom_y >= 0 &&
      (gfx_custom_y - watches_y > WATCHES_ROUNDING || watches_y - gfx_custom_y > WATCHES_ROUNDING))
    watches_y = gfx_custom_y;
  if (input_pressed() & BTN_L) {
    move_held = 0;
    gfx_logf("watches moved");
    return false;
  }
  uint32_t held = input_held();
  int dx = !!(held & BTN_DRIGHT) - !!(held & BTN_DLEFT);
  int dy = !!(held & BTN_DDOWN) - !!(held & BTN_DUP);
  move_held = dx || dy ? move_held + 1 : 0;
  int step = glide_step(move_held);
  watches_x += dx * step;
  watches_y += dy * step;
  watches_x = clamp_int(watches_x, 0, WATCHES_POS_MAX);
  watches_y = clamp_int(watches_y, 0, WATCHES_POS_MAX);
  return true;
}

static void act_move_watches(menu_item_t *it)
{
  if (watches_corner != GFX_CUSTOM) { /* start near the corner they're in */
    static const int16_t corner_start[GFX_NPLACES][2] = {
      { 0, 0 }, { 1000, 0 }, { 0, 1000 }, { 1000, 1000 }, { 400, 0 }, { 400, 1000 }, { 0, 0 },
    };
    watches_x = corner_start[watches_corner][0];
    watches_y = corner_start[watches_corner][1];
    watches_corner = GFX_CUSTOM;
    gfx_custom_x = -1;
  }
  watches_moving = true;
  move_held = 0;
  menu_edit(it);
}

static void act_timer_toggle(menu_item_t *it)
{
  timer_toggle();
}

static void act_timer_reset(menu_item_t *it)
{
  timer_reset();
}

static void act_lag_reset(menu_item_t *it)
{
  lag_reset();
}

static void act_js_height_reset(menu_item_t *it)
{
  gfx_notice(js_height_reset() ? "JS Height reset" : "JS Height: Link not found");
}

/* --- watches: the list ------------------------------------------------------------------------------ */

/* A row of the list, gz's buttons: the red cross (remove); the wrench (show the value in debug >
 * memory) when it has an address; the label (rename it with gz's keyboard); the type switch; then
 * the value and the arrows that move it up or down the list. d-left/right picks a button, L presses
 * it (the type: the next one). it->data = the row's index in watch_list.
 * The checkbox after the cross shows or hides the watch on screen without taking it off the list.
 * Columns in the line: cross 0, checkbox 2, wrench 5, label 7, type 20, value 28, arrows. */
enum { PART_REMOVE, PART_SHOW, PART_WRENCH, PART_LABEL, PART_TYPE, PART_UP, PART_DOWN };
#define MAX_ROW_PARTS 8
#define WATCH_LABEL_SIZE 24
#define WATCH_LABEL_W 12 /* the label's column ("%-12.12s" in watch_row) */
#define ROW_VALUE_W 20
#define COL_SHOW 2       /* the checkbox (two cells): shown in the on-screen box or not */
#define COL_WRENCH 5
#define COL_LABEL 7
#define COL_VALUE 28
#define COL_UP (COL_VALUE + ROW_VALUE_W + 1)
#define COL_DOWN (COL_UP + 2)

/* the buttons this watch's row has, in order; returns how many */
static int row_parts(const watch_entry_t *entry, int *parts)
{
  int n_types, current;
  int count = 0;
  watch_entry_types(entry, &n_types, &current);
  parts[count++] = PART_REMOVE;
  parts[count++] = PART_SHOW;
  if (watch_entry_has_address(entry))
    parts[count++] = PART_WRENCH;
  parts[count++] = PART_LABEL;
  if (n_types)
    parts[count++] = PART_TYPE;
  parts[count++] = PART_UP;
  parts[count++] = PART_DOWN;
  return count;
}

/* the button chosen in a row (a PART_*) */
static int row_part(const menu_item_t *it)
{
  int parts[MAX_ROW_PARTS];
  int count = row_parts(&watch_list[it->data], parts);
  return parts[it->part < count ? it->part : count - 1];
}

static void watch_row(menu_item_t *it, char *buf, int len)
{
  if (it->data >= watch_count) {
    snprintf(buf, len, "-");
    return;
  }
  const watch_entry_t *entry = &watch_list[it->data];
  char label[WATCH_LABEL_SIZE], value[48];
  int n_types, current;
  const char *const *types = watch_entry_types(entry, &n_types, &current);
  watch_entry_label(entry, label, sizeof(label));
  watch_entry_text(entry, value, sizeof(value));
  snprintf(buf, len, "%c%c %c %-12.12s %s%-3s%s %-*.*s %c %c", entry->hidden ? GFX_CHECK_OFF : GFX_CHECK_ON,
           GFX_WIDE_REST, watch_entry_has_address(entry) ? GFX_ICON_WRENCH : ' ', label, n_types ? "< " : "  ",
           n_types ? types[current] : "", n_types ? " >" : "  ", ROW_VALUE_W, ROW_VALUE_W, value, GFX_ICON_UP,
           GFX_ICON_DOWN);
}

/* where the chosen button is in the line (the line starts with the cross: "x " + the row text) */
static bool watch_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  switch (row_part(it)) {
    case PART_REMOVE:
      *start = 0;
      *len = 1;
      return true;
    case PART_SHOW:
      *start = COL_SHOW;
      *len = 2;
      return true;
    case PART_WRENCH:
      *start = COL_WRENCH;
      *len = 1;
      return true;
    case PART_LABEL: {
      char label[WATCH_LABEL_SIZE];
      watch_entry_label(&watch_list[it->data], label, sizeof(label));
      *start = COL_LABEL;
      *len = clamp_int((int)strlen(label), 1, WATCH_LABEL_W);
      return true;
    }
    case PART_UP:
      *start = COL_UP;
      *len = 1;
      return true;
    case PART_DOWN:
      *start = COL_DOWN;
      *len = 1;
      return true;
    default: { /* PART_TYPE: the "< ... >" after the label */
      int open = COL_LABEL + WATCH_LABEL_W;
      while (line[open] && line[open] != '<')
        open++;
      int close = open;
      while (line[close] && line[close] != '>')
        close++;
      *start = open;
      *len = line[close] ? close - open + 1 : 0;
      return true;
    }
  }
}

/* the on-screen keyboard's answer; an empty label gives the watch its own name back */
static void watch_renamed(const char *text, intptr_t index)
{
  char before[WATCH_LABEL_SIZE];
  watch_entry_label(&watch_list[index], before, sizeof(before));
  watch_set_label((int)index, text);
  if (text[0])
    gfx_logf("renamed %s to %s", before, text);
  else
    gfx_logf("%s: its own name again", before);
}

/* moves the watch at `index` up (-1) or down (1) the list; the window and cursor follow it */
static void watch_move_row(menu_item_t *it, int index, int dir)
{
  int new_index = index + dir;
  if (new_index < 0 || new_index >= watch_count)
    return;
  /* the window follows the row, and the cursor goes with it, on the same arrow */
  if (new_index < watch_top)
    watch_top = new_index;
  else if (new_index >= watch_top + WATCH_ROWS)
    watch_top = new_index - WATCH_ROWS + 1;
  int row = watch_first_row + (new_index - watch_top);
  if (row < m_watches.n_items) {
    m_watches.items[row].part = it->part;
    m_watches.selected = row;
  }
  watch_move(index, dir);
}

static void watch_press(menu_item_t *it, int part)
{
  int index = (int)it->data;
  if (index >= watch_count)
    return;
  watch_entry_t *entry = &watch_list[index];
  char label[WATCH_LABEL_SIZE];
  watch_entry_label(entry, label, sizeof(label));
  int button = row_part(it);
  switch (button) {
    case PART_REMOVE:
      gfx_logf("removed watch %s", label);
      watch_remove(index);
      break;
    case PART_SHOW:
      watch_set_hidden(index, !entry->hidden);
      gfx_logf("%s %s", label, entry->hidden ? "hidden" : "shown");
      break;
    case PART_WRENCH: {
      uintptr_t address;
      int type;
      if (watch_entry_address(entry, &address, &type))
        memview_open(address, type);
      else
        gfx_logf("%s: not in memory right now", label);
      break;
    }
    case PART_LABEL:
      osk_open("label", entry->label[0] ? entry->label : label, watch_renamed, index);
      break;
    case PART_UP:
    case PART_DOWN:
      watch_move_row(it, index, button == PART_UP ? -1 : 1);
      break;
    default: { /* PART_TYPE: the next type */
      int n_types, current;
      const char *const *types = watch_entry_types(entry, &n_types, &current);
      if (n_types) {
        watch_entry_set_type(index, current + 1);
        types = watch_entry_types(entry, &n_types, &current);
        gfx_logf("%s: %s", label, types[current]);
      }
      break;
    }
  }
}

/* gz's arrows over the list (as in debug > memory) when there are more watches than WATCH_ROWS */
static void watch_arrows(menu_item_t *it, char *buf, int len)
{
  int last = watch_top + WATCH_ROWS < watch_count ? watch_top + WATCH_ROWS : watch_count;
  snprintf(buf, len, "%c %c  %d-%d / %d", GFX_ICON_UP, GFX_ICON_DOWN, watch_top + 1, last, watch_count);
}

/* part 0 the up arrow, 1 the down arrow */
static bool arrows_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = it->part ? 2 : 0;
  *len = 1;
  return true;
}

static void watch_scroll(menu_item_t *it, int part)
{
  int top = clamp_int(watch_top + (part ? 1 : -1), 0, max_top(watch_count, WATCH_ROWS));
  if (top != watch_top) {
    watch_top = top;
    watches_built = ~0u; /* the rows show other watches: rebuilt */
  }
}

static const char *const watch_arrow_hints[] = { "L: scroll the list up (held: keeps going)",
                                                 "L: scroll the list down (held: keeps going)" };

/* --- watches: adding them -------------------------------------------------------------------------- */

/* the folder: gz's import list, here the predefined watches. Choosing one adds it and goes back. */
static void watch_add_state(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, "%s", watch_has((watch_t)it->data) ? "(in the list)" : "");
}

static void act_watch_add(menu_item_t *it)
{
  if (!watch_add((watch_t)it->data)) {
    gfx_notice("that watch is already in the list");
    return;
  }
  gfx_logf("added watch %s", watch_labels[it->data]);
  menu_back();
}

/* gz's +: a memory watch at an address typed in (hex digits, as gz's address field). It starts
 * from the last memory watch's address and type (as gz does), else main's start as a word. */
#define CUSTOM_DIGITS 10
static uintptr_t custom_addr;
static int custom_digit; /* the digit being changed, 0 = the leftmost */
static int custom_type = MW_X32;

/* The row under the list, gz's buttons with no names (the hint says what they are): the folder
 * (known watches) and the + (custom watch). While typing an address it follows the +. */
enum { TOOL_KNOWN, TOOL_CUSTOM };
#define COL_TOOL_CUSTOM 3  /* the +, after the folder (two cells) and a space */
#define COL_TOOL_DIGITS 6  /* the address, after "F+ +  " */
static const char *const tool_hints[] = { "known watches: the built-in ones (L: open)",
                                          "custom watch: a memory address of your choosing (L: type it)" };

static void tools_text(menu_item_t *it, char *buf, int len)
{
  if (menu_editing == it)
    snprintf(buf, len, "%c%c %c  %0*lx  %s", GFX_ICON_FOLDER, GFX_WIDE_REST, GFX_ICON_ADD, CUSTOM_DIGITS,
             (unsigned long)custom_addr, memory_type_names[custom_type]);
  else
    snprintf(buf, len, "%c%c %c", GFX_ICON_FOLDER, GFX_WIDE_REST, GFX_ICON_ADD);
}

static bool tools_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  if (menu_editing == it) {
    *start = COL_TOOL_DIGITS + custom_digit;
    *len = 1;
  } else if (it->part == TOOL_KNOWN) {
    *start = 0;
    *len = 2;
  } else {
    *start = COL_TOOL_CUSTOM;
    *len = 1;
  }
  return true;
}

/* typing the address: d-left/right picks a digit, d-up/down changes it, L adds the watch */
static bool custom_watch_edit(menu_item_t *it)
{
  if (input_repeat(BTN_DLEFT)) {
    custom_digit = custom_digit > 0 ? custom_digit - 1 : 0;
  } else if (input_repeat(BTN_DRIGHT)) {
    custom_digit = custom_digit < CUSTOM_DIGITS - 1 ? custom_digit + 1 : CUSTOM_DIGITS - 1;
  } else if (input_repeat(BTN_DUP) || input_repeat(BTN_DDOWN)) {
    int shift = (CUSTOM_DIGITS - 1 - custom_digit) * 4;
    int dir = input_repeat(BTN_DUP) ? 1 : -1;
    /* the digit goes round, f <-> 0, without carrying into the next */
    uintptr_t digit = ((custom_addr >> shift & 0xF) + (uintptr_t)(dir + 16)) & 0xF;
    custom_addr = (custom_addr & ~((uintptr_t)0xF << shift)) | digit << shift;
  } else if (input_pressed() & BTN_L) {
    if (watch_add_memory(custom_addr, custom_type))
      gfx_logf("added watch %0*lx", CUSTOM_DIGITS, (unsigned long)custom_addr);
    else
      gfx_notice("that address is already watched");
    return false;
  }
  return true;
}

static void act_custom_watch(menu_item_t *it)
{
  custom_addr = z64hd_main;
  custom_type = MW_X32;
  for (int i = watch_count - 1; i >= 0; i--)
    if (watch_list[i].id == W_MEMORY) {
      custom_addr = watch_list[i].addr;
      custom_type = watch_list[i].type;
      break;
    }
  custom_digit = CUSTOM_DIGITS - 1;
  menu_edit(it);
}

static void tools_press(menu_item_t *it, int part)
{
  if (part == TOOL_KNOWN)
    menu_goto(&m_watch_add);
  else
    act_custom_watch(it);
}

/* --- watches: the menu ----------------------------------------------------------------------------- */

static void add_watches_box_items(void)
{
  menu_add_check(&m_watches, "show on screen", get_shown, set_shown, 0);
  menu_add_heading(&m_watches, "watches box");
  menu_add_option(&m_watches, "position", GFX_NPLACES, watches_corner_names, get_corner, set_corner, 0);
  menu_item_t *move = menu_add_action(&m_watches, "move", act_move_watches, 0);
  if (move) {
    move->edit = move_watches;
    move->edit_alone = true;
    move->hint = "L: move the watches around the screen";
    move->edit_hint = "watches: d-pad moves them   L: done";
  }
}

/* the scroll arrows (if needed) and a row for each watch in the window */
static void add_watch_rows(void)
{
  watch_top = clamp_int(watch_top, 0, max_top(watch_count, WATCH_ROWS));
  if (watch_count > WATCH_ROWS) {
    menu_item_t *arrows = menu_add_action(&m_watches, "", 0, 0);
    if (arrows) {
      arrows->text = watch_arrows;
      arrows->cursor = arrows_cursor;
      arrows->press = watch_scroll;
      arrows->press_repeat = true;
      arrows->n_parts = 2;
      arrows->part_hints = watch_arrow_hints;
    }
  }
  watch_first_row = m_watches.n_items;
  for (int i = watch_top; i < watch_count && i < watch_top + WATCH_ROWS; i++) {
    menu_item_t *row = menu_add_action(&m_watches, "", 0, i);
    if (!row)
      continue;
    int parts[MAX_ROW_PARTS];
    row->text = watch_row;
    row->icon = GFX_ICON_REMOVE;
    row->cursor = watch_cursor;
    row->press = watch_press;
    row->n_parts = row_parts(&watch_list[i], parts);
    row->hint = "d-left/right: pick a button   L: press it";
  }
}

/* the folder and + row; `reselect` puts the cursor back on it, on button `part` */
static void add_watch_tools(bool reselect, int part)
{
  menu_item_t *tools = menu_add_action(&m_watches, "", 0, 0);
  if (!tools)
    return;
  tools->text = tools_text;
  tools->cursor = tools_cursor;
  tools->press = tools_press;
  tools->n_parts = 2;
  tools->part_hints = tool_hints;
  tools->edit = custom_watch_edit;
  tools->edit_hint = "d-left/right: digit   d-up/down: change   L: add";
  if (reselect) {
    m_watches.selected = (int)(tools - m_watches.items);
    tools->part = part;
  }
}

/* The watches menu, rebuilt whenever the list changes (watches_sync). */
static void fill_watches(void)
{
  /* the cursor stays on the folder when a watch is added from it */
  bool on_tools = m_watches.n_items && m_watches.items[m_watches.selected].text == tools_text;
  int tools_part = on_tools ? m_watches.items[m_watches.selected].part : 0;
  int part = m_watches.n_items ? m_watches.items[m_watches.selected].part : 0;
  menu_reset(&m_watches);
  add_watches_box_items();
  menu_add_heading(&m_watches, "watches");
  add_watch_rows();
  add_watch_tools(on_tools, tools_part);
  menu_add_heading(&m_watches, "timer");
  menu_add_action(&m_watches, "start/stop timer", act_timer_toggle, 0);
  menu_add_action(&m_watches, "reset timer", act_timer_reset, 0);
  menu_add_action(&m_watches, "reset lag counter", act_lag_reset, 0);
  menu_item_t *jh = menu_add_action(&m_watches, "reset JS Height (R + A + d-right)", act_js_height_reset, 0);
  if (jh)
    jh->hint = "L: back to FLT_MAX, as after a load (any height)";
  menu_add_return(&m_watches);
  menu_refresh(&m_watches);
  /* the cursor keeps to the same button of whatever row it's now on */
  menu_item_t *selected = &m_watches.items[m_watches.selected];
  if (selected->n_parts > 1)
    selected->part = part < selected->n_parts ? part : selected->n_parts - 1;
  watches_built = watch_version;
}

void watches_sync(void)
{
  if (watches_built != watch_version)
    fill_watches();
}

static void build_watches(void)
{
  menu_init(&m_watches, "watches", WATCH_ROWS + 16);
  fill_watches();
  menu_init(&m_watch_add, "known watches", W_COUNT + 1);
  m_watch_add.parent = &m_watches; /* opened from the folder button */
  for (int w = 0; w < W_COUNT; w++) {
    menu_item_t *known = menu_add_action(&m_watch_add, watch_labels[w], act_watch_add, w);
    if (known) {
      known->text = watch_add_state;
      known->hint = "L: add to the list";
    }
  }
  menu_add_return(&m_watch_add);
}

/* --- scene ------------------------------------------------------------------------------------ */

static menu_t m_scene, m_collision;

static bool get_air_walls_shown(menu_item_t *it)
{
  return airwalls_shown;
}

static void set_air_walls_shown(menu_item_t *it, bool on)
{
  airwalls_shown = on;
}

static bool get_air_walls_all(menu_item_t *it)
{
  return airwalls_all;
}

static void set_air_walls_all(menu_item_t *it, bool on)
{
  airwalls_all = on;
}

static void text_air_wall_summary(menu_item_t *it, char *buf, int len)
{
  char summary[64];
  airwall_summary(summary, sizeof(summary));
  snprintf(buf, len, TF("count  %s"), summary);
}

static void text_air_wall_link(menu_item_t *it, char *buf, int len)
{
  char text[64];
  airwall_link_text(text, sizeof(text));
  snprintf(buf, len, TF("link   %s"), text);
}

static void build_scene(void)
{
  menu_init(&m_scene, "scene", 3);
  menu_item_t *reload = menu_add_action(&m_scene, "reload map", act_reload_map, 0);
  if (reload)
    reload->hint = "L: reload this map (at the age set in the link menu)";
  menu_add_submenu(&m_scene, "collision", &m_collision);
  menu_add_return(&m_scene);

  menu_init(&m_collision, "collision", 6);
  menu_add_heading(&m_collision, "air walls");
  menu_item_t *show = menu_add_check(&m_collision, "show", get_air_walls_shown, set_air_walls_shown, 0);
  if (show) {
    show->hint = "cyan: stop Link   red: he's inside it   violet: other kinds";
    show->log_label = "air walls:";
  }
  menu_item_t *all_kinds = menu_add_check(&m_collision, "all kinds", get_air_walls_all, set_air_walls_all, 0);
  if (all_kinds) {
    all_kinds->hint = "also the walls for enemies, NPCs and objects";
    all_kinds->log_label = "all kinds:";
  }
  menu_add_text(&m_collision, text_air_wall_summary, 0);
  menu_add_text(&m_collision, text_air_wall_link, 0);
  menu_add_return(&m_collision);
}

/* --- settings --------------------------------------------------------------------------------- */

static menu_t m_settings, m_input_display, m_levitate, m_frame_advance;

/* menu settings */
#define OPACITY_STEP 10 /* percent per choice */
static const char *const opacity_names[] = { "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%",
                                             "90%", "100%" };
static const char *const size_names[] = { "auto", "small (2x)", "medium (2.5x)", "large (3x)" };

static int get_size(menu_item_t *it)
{
  return gfx_menu_scale;
}

static void set_size(menu_item_t *it, int size)
{
  gfx_menu_scale = size;
}

static int get_opacity(menu_item_t *it)
{
  return gfx_opacity / OPACITY_STEP;
}

static void set_opacity(menu_item_t *it, int choice)
{
  gfx_opacity = choice * OPACITY_STEP;
}

/* the languages are found on the SD card after the menus are built: the switch reads them each time */
static int get_lang(menu_item_t *it)
{
  it->n_choices = lang_count();
  it->choices = lang_names();
  return lang_current();
}

static void set_lang(menu_item_t *it, int choice)
{
  lang_select(choice);
}

/* input display */
static const char *const input_style_names[] = { "row", "controller" };
static const char *const input_size_names[] = { "small", "medium", "large" };

static bool get_input_display(menu_item_t *it)
{
  return input_display;
}

static void set_input_display(menu_item_t *it, bool on)
{
  input_display = on;
}

static bool get_input_bg(menu_item_t *it)
{
  return input_display_bg;
}

static void set_input_bg(menu_item_t *it, bool on)
{
  input_display_bg = on;
}

static int get_input_style(menu_item_t *it)
{
  return input_style;
}

static void set_input_style(menu_item_t *it, int style)
{
  input_style = style;
}

static int get_input_size(menu_item_t *it)
{
  return input_size;
}

static void set_input_size(menu_item_t *it, int size)
{
  input_size = size;
}

static bool get_input_left(menu_item_t *it)
{
  return input_show_left;
}

static void set_input_left(menu_item_t *it, bool on)
{
  input_show_left = on;
}

static bool get_input_right(menu_item_t *it)
{
  return input_show_right;
}

static void set_input_right(menu_item_t *it, bool on)
{
  input_show_right = on;
}

static bool get_input_buttons(menu_item_t *it)
{
  return input_show_buttons;
}

static void set_input_buttons(menu_item_t *it, bool on)
{
  input_show_buttons = on;
}

/* levitate */
static const char *const lev_speed_names[] = { "2", "4", "6", "8", "12", "20" };
static const int lev_speeds[] = { 2, 4, 6, 8, 12, 20 };
static const char *const lev_move_names[] = { "2", "4", "6", "8", "12", "20", "30" };
static const int lev_moves[] = { 2, 4, 6, 8, 12, 20, 30 };

static bool get_levitate(menu_item_t *it)
{
  return levitate_enabled;
}

static void set_levitate(menu_item_t *it, bool on)
{
  levitate_enabled = on;
}

static bool get_walls(menu_item_t *it)
{
  return levitate_through_walls;
}

static void set_walls(menu_item_t *it, bool on)
{
  levitate_through_walls = on;
}

static int get_lev_speed(menu_item_t *it)
{
  for (int i = 0; i < ARRAY_LEN(lev_speeds); i++)
    if (lev_speeds[i] == levitate_speed)
      return i;
  return -1;
}

static void set_lev_speed(menu_item_t *it, int choice)
{
  levitate_speed = lev_speeds[choice];
}

static int get_lev_move(menu_item_t *it)
{
  for (int i = 0; i < ARRAY_LEN(lev_moves); i++)
    if (lev_moves[i] == levitate_move_speed)
      return i;
  return -1;
}

static void set_lev_move(menu_item_t *it, int choice)
{
  levitate_move_speed = lev_moves[choice];
}

static void text_levitate(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, TF("status  %s"), levitate_state());
}

/* frame advance */
static const char *const fa_button_names[] = { "L", "R" };

static bool get_fa(menu_item_t *it)
{
  return frame_advance;
}

static void set_fa(menu_item_t *it, bool on)
{
  frame_advance = on;
}

static bool get_pause_display(menu_item_t *it)
{
  return pause_display;
}

static void set_pause_display(menu_item_t *it, bool on)
{
  pause_display = on;
}

static int get_fa_button(menu_item_t *it)
{
  return frame_advance_button;
}

static void set_fa_button(menu_item_t *it, int button)
{
  frame_advance_button = button;
}

static void build_frame_advance(void)
{
  menu_init(&m_frame_advance, "frame advance", 5);
  menu_add_heading(&m_frame_advance, "frame advance options");
  menu_item_t *enabled = menu_add_check(&m_frame_advance, "enabled", get_fa, set_fa, 0);
  if (enabled) {
    enabled->hint = "in game, tap the button: pause; tap: a frame; button + L3: run";
    enabled->log_label = "frame advance";
  }
  menu_item_t *pause_icon = menu_add_check(&m_frame_advance, "pause icon", get_pause_display, set_pause_display, 0);
  if (pause_icon)
    pause_icon->hint = "gz's pause display at the top left while paused";
  menu_item_t *button = menu_add_option(&m_frame_advance, "button", ARRAY_LEN(fa_button_names), fa_button_names,
                                        get_fa_button, set_fa_button, 0);
  if (button) {
    button->hint = "the button for frame advance (the game no longer sees it while it's on)";
    button->log_label = "frame advance button";
  }
  menu_end_group(&m_frame_advance);
  menu_add_return(&m_frame_advance);
}

static void build_input_display(void)
{
  menu_init(&m_input_display, "input display", 10);
  menu_add_heading(&m_input_display, "display options");
  menu_item_t *show = menu_add_check(&m_input_display, "show", get_input_display, set_input_display, 0);
  if (show) {
    show->hint = "the sticks and the buttons held, at the bottom left (hidden while the menu is open)";
    show->log_label = "input display";
  }
  menu_item_t *background = menu_add_check(&m_input_display, "background", get_input_bg, set_input_bg, 0);
  if (background)
    background->hint = "behind the whole input display, as the watches have";
  menu_add_check(&m_input_display, "left stick", get_input_left, set_input_left, 0);
  menu_add_check(&m_input_display, "right stick", get_input_right, set_input_right, 0);
  menu_add_check(&m_input_display, "buttons", get_input_buttons, set_input_buttons, 0);
  menu_item_t *style = menu_add_option(&m_input_display, "style", ARRAY_LEN(input_style_names), input_style_names,
                                       get_input_style, set_input_style, 0);
  if (style) {
    style->hint = "row: gz's line   controller: a Switch 2 Pro Controller, the sticks' values under it";
    style->log_label = "input display style";
  }
  menu_item_t *size = menu_add_option(&m_input_display, "size", ARRAY_LEN(input_size_names), input_size_names,
                                      get_input_size, set_input_size, 0);
  if (size) {
    size->hint = "small, medium, large: half, three quarters or all of the menu's text size";
    size->log_label = "input display size";
  }
  menu_end_group(&m_input_display);
  menu_add_return(&m_input_display);
}

static void build_levitate(void)
{
  menu_init(&m_levitate, "levitate", 9);
  menu_add_heading(&m_levitate, "levitation options");
  menu_item_t *enabled = menu_add_check(&m_levitate, "enabled", get_levitate, set_levitate, 0);
  if (enabled) {
    enabled->hint = "L3: start/stop floating   R3: hold height";
    enabled->log_label = "levitate";
  }
  menu_add_check(&m_levitate, "through walls", get_walls, set_walls, 0);
  menu_add_option(&m_levitate, "rise speed", ARRAY_LEN(lev_speed_names), lev_speed_names, get_lev_speed,
                  set_lev_speed, 0);
  menu_add_option(&m_levitate, "move speed", ARRAY_LEN(lev_move_names), lev_move_names, get_lev_move, set_lev_move,
                  0);
  menu_end_group(&m_levitate);
  menu_add_return(&m_levitate);
  /* what can only be read, under return after a blank line */
  add_blank_line(&m_levitate);
  menu_add_text(&m_levitate, text_levitate, 0);
}

static void build_settings(void)
{
  menu_init(&m_settings, "settings", 10);
  menu_add_heading(&m_settings, "menu settings");
  menu_item_t *language = menu_add_option(&m_settings, "language", 1, lang_names(), get_lang, set_lang, 0);
  if (language)
    language->hint = "files in sd:/ootgz/lang (see lang/template.json)";
  menu_item_t *size = menu_add_option(&m_settings, "size", ARRAY_LEN(size_names), size_names, get_size, set_size, 0);
  if (size)
    size->hint = "text size of the menu, watches and log (auto: for the screen)";
  menu_item_t *opacity = menu_add_option(&m_settings, "background opacity", ARRAY_LEN(opacity_names), opacity_names,
                                         get_opacity, set_opacity, 0);
  if (opacity)
    opacity->hint = "below 100%: costs some speed, and what's behind lags 2 frames";
  menu_end_group(&m_settings);
  menu_add_heading(&m_settings, "other settings");
  menu_add_submenu(&m_settings, "input display", &m_input_display);
  menu_add_submenu(&m_settings, "levitate", &m_levitate);
  menu_add_submenu(&m_settings, "frame advance", &m_frame_advance);
  menu_end_group(&m_settings);
  menu_add_return(&m_settings);

  build_frame_advance();
  build_input_display();
  build_levitate();
}

/* --- debug (gz's debug menu) ----------------------------------------------------------------------- */

static menu_t m_debug;

/* debug > heap: gz shows its game's arena; here, what the console says of the game's memory
 * (svcGetInfo) and how much of the heap region is mapped (svcQueryMemory, refreshed every second). */
static menu_t m_heap;

/* svcGetInfo's info types, and svcQueryMemory's MemoryType for heap */
#define INFO_HEAP_REGION_ADDRESS 4
#define INFO_HEAP_REGION_SIZE    5
#define INFO_TOTAL_MEMORY_SIZE   6
#define INFO_USED_MEMORY_SIZE    7
#define MEMORY_TYPE_HEAP         5
#define HEAP_REFRESH_FRAMES      30

static struct {
  uint64_t start, size;     /* the heap region */
  uint64_t mapped, blocks;  /* how much of it is mapped, in how many blocks */
  uint64_t total, used;     /* the game's memory */
  uint32_t updated_at;      /* frame_counter() */
  bool ok;                  /* the region is known */
} heap;

static void heap_update(void)
{
  uint32_t now = frame_counter();
  if (heap.ok && now - heap.updated_at < HEAP_REFRESH_FRAMES)
    return;
  heap.updated_at = now;
  heap.ok = sys_get_info(INFO_HEAP_REGION_ADDRESS, 0, &heap.start) &&
            sys_get_info(INFO_HEAP_REGION_SIZE, 0, &heap.size);
  if (!sys_get_info(INFO_TOTAL_MEMORY_SIZE, 0, &heap.total))
    heap.total = 0;
  if (!sys_get_info(INFO_USED_MEMORY_SIZE, 0, &heap.used))
    heap.used = 0;
  heap.mapped = 0;
  heap.blocks = 0;
  for (uint64_t addr = heap.start; heap.ok && addr < heap.start + heap.size;) {
    sys_meminfo_t info;
    if (sys_query_memory(&info, addr) != 0 || info.size == 0)
      break;
    if ((info.type & 0xFF) == MEMORY_TYPE_HEAP) {
      heap.mapped += info.size;
      heap.blocks++;
    }
    addr = info.addr + info.size;
  }
}

/* bytes as MiB with one decimal */
static void format_mib(uint64_t bytes, char *buf, int len)
{
  snprintf(buf, len, "%llu.%01llu MiB", (unsigned long long)(bytes >> 20),
           (unsigned long long)((bytes & 0xFFFFF) * 10 >> 20));
}

/* it->data: which line */
enum { HEAP_LINE_REGION, HEAP_LINE_MAPPED, HEAP_LINE_MEMORY, HEAP_LINE_FREE, N_HEAP_LINES };

static void text_heap(menu_item_t *it, char *buf, int len)
{
  heap_update();
  char first[24], second[24];
  switch (it->data) {
    case HEAP_LINE_REGION:
      if (!heap.ok) {
        snprintf(buf, len, "%s", T("heap region   unknown"));
      } else {
        format_mib(heap.size, first, sizeof(first));
        snprintf(buf, len, TF("heap region   %010llx  %s"), (unsigned long long)heap.start, first);
      }
      break;
    case HEAP_LINE_MAPPED:
      format_mib(heap.mapped, first, sizeof(first));
      snprintf(buf, len, TF("heap in use   %s in %d blocks"), first, (int)heap.blocks);
      break;
    case HEAP_LINE_MEMORY:
      format_mib(heap.total, first, sizeof(first));
      format_mib(heap.used, second, sizeof(second));
      snprintf(buf, len, TF("memory        %s of %s used"), second, first);
      break;
    default: /* HEAP_LINE_FREE */
      format_mib(heap.total > heap.used ? heap.total - heap.used : 0, first, sizeof(first));
      snprintf(buf, len, TF("memory free   %s"), first);
      break;
  }
}

static void act_heap_memory(menu_item_t *it)
{
  heap_update();
  if (heap.ok)
    memview_open((uintptr_t)heap.start, MW_X64);
}

/* debug > pause research: the engine's own pause state (Sequence_* GameData), to find how the game
 * stops its world while still drawing (its pause screen), for frame advance. Shows the values, lets them
 * be changed, and can log every change (also with the menu closed: menus_research_frame). */
static menu_t m_pause_research;
static const struct { const char *name; uint32_t hash; bool is_int; } pause_fields[] = {
  { "IsPause", 0x604AEBE1u, false }, { "GamePauseRequestCount", 0x64A61098u, true },
  { "PreGamePauseRequestCount", 0xEF935C2Du, true }, { "PostGamePauseRequestCount", 0xE8D033EAu, true },
  { "IsPreGamePause", 0xBF5BF25Fu, false }, { "IsPostGamePause", 0xC2F987DBu, false },
  { "IsSceneTransitionPause", 0x9A9506E7u, false }, { "IsEventBancChangePause", 0xF96989B6u, false },
  { "IsChangingBancFromRequestToPause", 0xF8633C7Fu, false },
};
#define N_PAUSE_FIELDS ARRAY_LEN(pause_fields)
static bool pause_log;
static int32_t pause_last[N_PAUSE_FIELDS];
static bool pause_seen; /* pause_last holds values (logging is on and a frame has passed) */

static int32_t pause_value(int i)
{
  if (pause_fields[i].is_int) {
    int32_t value;
    return gd_get_int(pause_fields[i].hash, &value) ? value : NUM_UNKNOWN;
  }
  bool flag;
  return gd_get_bool(pause_fields[i].hash, &flag) ? flag : NUM_UNKNOWN;
}

void menus_research_frame(void)
{
  if (!pause_log)
    return;
  for (int i = 0; i < N_PAUSE_FIELDS; i++) {
    int32_t value = pause_value(i);
    if (pause_seen && value != pause_last[i])
      log_printf("pause research: frame %u: %s %d -> %d", (unsigned)frame_counter(), pause_fields[i].name,
                 (int)pause_last[i], (int)value);
    pause_last[i] = value;
  }
  pause_seen = true;
}

static bool get_pause_log(menu_item_t *it)
{
  return pause_log;
}

static void set_pause_log(menu_item_t *it, bool on)
{
  pause_log = on;
  pause_seen = false;
}

/* it->data: the field (pause_fields) */
static int32_t get_pause_int(menu_item_t *it)
{
  return pause_value((int)it->data);
}

static void set_pause_int(menu_item_t *it, int32_t value)
{
  gd_set_int(pause_fields[it->data].hash, value);
}

static bool get_pause_bool(menu_item_t *it)
{
  return pause_value((int)it->data) == 1;
}

static void set_pause_bool(menu_item_t *it, bool on)
{
  gd_set_bool(pause_fields[it->data].hash, on);
}

static void build_pause_research(void)
{
  menu_init(&m_pause_research, "pause research", N_PAUSE_FIELDS + 4);
  menu_item_t *log_changes = menu_add_check(&m_pause_research, "log changes", get_pause_log, set_pause_log, 0);
  if (log_changes)
    log_changes->hint = "every change of these to the log, also with the menu closed (open the game's pause screen)";
  menu_add_heading(&m_pause_research, "Sequence_");
  for (int i = 0; i < N_PAUSE_FIELDS; i++) {
    const char *name = pause_fields[i].name;
    menu_item_t *field;
    if (pause_fields[i].is_int)
      field = menu_add_number(&m_pause_research, name, get_pause_int, set_pause_int, 0, 0, i);
    else
      field = menu_add_check(&m_pause_research, name, get_pause_bool, set_pause_bool, i);
    if (field)
      field->hint = "the game's own value; changing it is an experiment";
  }
  menu_end_group(&m_pause_research);
  menu_add_return(&m_pause_research);
}

static void build_debug(void)
{
  build_pause_research();
  menu_init(&m_debug, "debug", 4);
  menu_add_submenu(&m_debug, "heap", &m_heap);
  menu_add_submenu(&m_debug, "memory", &memview_menu);
  menu_add_submenu(&m_debug, "pause research", &m_pause_research);
  menu_add_return(&m_debug);
  memview_build(&m_watches);

  menu_init(&m_heap, "heap", 7);
  menu_item_t *view = menu_add_action(&m_heap, "view the heap in memory", act_heap_memory, 0);
  if (view)
    view->hint = "L: debug > memory at the start of the heap";
  menu_add_return(&m_heap);
  add_blank_line(&m_heap);
  for (int line = 0; line < N_HEAP_LINES; line++)
    menu_add_text(&m_heap, text_heap, line);
}

/* --- log ------------------------------------------------------------------------------------------ */

static menu_t m_log;

/* The log menu: the module's last LOG_LINES messages, newest first, filtered by type (the word(s)
 * before a message's ':', e.g. "warp", "settings"; or "problems") and scrolled with gz's arrows. */
#define LOG_ROWS 12
#define MAX_LOG_TYPES 24
#define LOG_TYPE_SIZE 20
#define LOG_TYPE_MAX_LEN 18 /* a ':' further in isn't after a type */

/* the filter's choices: all, problems, then each type seen */
enum { LOG_FILTER_ALL, LOG_FILTER_PROBLEMS, LOG_FILTER_FIRST_TYPE };

static char log_types[MAX_LOG_TYPES][LOG_TYPE_SIZE];
static const char *log_type_names[MAX_LOG_TYPES + LOG_FILTER_FIRST_TYPE];
static int n_log_types;
static int log_filter;
static int log_top; /* how many matching messages are scrolled past */

/* a message's type: what's before its ':' (letters and spaces, a short way in), or "other" */
static void log_type(const char *message, char *out, int len)
{
  int n = 0;
  while (message[n] && n < LOG_TYPE_MAX_LEN && ((message[n] >= 'a' && message[n] <= 'z') || message[n] == ' '))
    n++;
  if (message[n] == ':' && n > 0)
    snprintf(out, len, "%.*s", n, message);
  else
    snprintf(out, len, "other");
}

/* a message saying something couldn't be done */
static bool log_problem(const char *message)
{
  static const char *const words[] = { "couldn't", "can't", "fail", "refused", "not found", "error", "abandoned" };
  for (int w = 0; w < ARRAY_LEN(words); w++)
    for (const char *p = message; *p; p++)
      if (after_prefix(p, words[w]))
        return true;
  return false;
}

/* the types present, for the filter switch */
static void log_scan_types(void)
{
  n_log_types = 0;
  for (int i = 0; i < LOG_LINES; i++) {
    const char *message = log_line(i);
    if (!message || !message[0])
      continue;
    char type[LOG_TYPE_SIZE];
    log_type(message, type, sizeof(type));
    int known = 0;
    while (known < n_log_types && !strings_equal(log_types[known], type))
      known++;
    if (known == n_log_types && n_log_types < MAX_LOG_TYPES)
      snprintf(log_types[n_log_types++], sizeof(log_types[0]), "%s", type);
  }
  log_type_names[LOG_FILTER_ALL] = "all";
  log_type_names[LOG_FILTER_PROBLEMS] = "problems";
  for (int k = 0; k < n_log_types; k++)
    log_type_names[LOG_FILTER_FIRST_TYPE + k] = log_types[k];
}

static bool log_matches(const char *message)
{
  if (log_filter == LOG_FILTER_ALL)
    return true;
  if (log_filter == LOG_FILTER_PROBLEMS)
    return log_problem(message);
  char type[LOG_TYPE_SIZE];
  log_type(message, type, sizeof(type));
  int type_index = log_filter - LOG_FILTER_FIRST_TYPE;
  return type_index < n_log_types && strings_equal(type, log_types[type_index]);
}

/* the n-th message (newest first) that passes the filter, or NULL; *count: how many pass */
static const char *log_nth(int n, int *count)
{
  const char *found = 0;
  int matching = 0;
  for (int i = 0; i < LOG_LINES; i++) {
    const char *message = log_line(i);
    if (!message || !message[0] || !log_matches(message))
      continue;
    if (matching == n)
      found = message;
    matching++;
  }
  if (count)
    *count = matching;
  return found;
}

static int log_count(void)
{
  int count;
  log_nth(0, &count);
  return count;
}

static int get_log_filter(menu_item_t *it)
{
  log_scan_types();
  it->n_choices = n_log_types + LOG_FILTER_FIRST_TYPE;
  it->choices = log_type_names;
  if (log_filter >= it->n_choices)
    log_filter = LOG_FILTER_ALL;
  /* the hint: "which messages:", then what this filter shows, indented under it */
  static char hint[160];
  char what[100];
  if (log_filter == LOG_FILTER_ALL)
    snprintf(what, sizeof(what), "%s", T("every message, newest first (all also in sd:/sloperinaGZ/log.txt)"));
  else if (log_filter == LOG_FILTER_PROBLEMS)
    snprintf(what, sizeof(what), "%s", T("anything that couldn't be done"));
  else
    snprintf(what, sizeof(what), TF("messages about %s"), log_type_names[log_filter]);
  snprintf(hint, sizeof(hint), "%s\n  %s: %s", T("which messages:"), T(log_type_names[log_filter]), what);
  it->hint = hint;
  return log_filter;
}

static void set_log_filter(menu_item_t *it, int filter)
{
  log_filter = filter;
  log_top = 0;
}

static void log_arrows(menu_item_t *it, char *buf, int len)
{
  int count = log_count();
  int max = max_top(count, LOG_ROWS);
  if (log_top > max)
    log_top = max;
  int last = log_top + LOG_ROWS < count ? log_top + LOG_ROWS : count;
  snprintf(buf, len, "%c %c  %d-%d / %d", GFX_ICON_UP, GFX_ICON_DOWN, count ? log_top + 1 : 0, last, count);
}

static void log_scroll(menu_item_t *it, int part)
{
  int max = max_top(log_count(), LOG_ROWS);
  log_top = clamp_int(log_top + (part ? 1 : -1), 0, max);
}

static const char *const log_arrow_hints[] = { "L: scroll to newer (held: keeps going)",
                                               "L: scroll to older (held: keeps going)" };

/* it->data: the row */
static void text_log(menu_item_t *it, char *buf, int len)
{
  const char *message = log_nth(log_top + (int)it->data, 0);
  snprintf(buf, len, "%s", message ? message : "");
}

static void build_log(void)
{
  menu_init(&m_log, "log", LOG_ROWS + 5);
  /* the filter on the heading's line: "log history  < all >" */
  menu_item_t *filter =
    menu_add_option(&m_log, "log history", 1, log_type_names, get_log_filter, set_log_filter, 0);
  if (filter)
    filter->log_label = ""; /* the list shows the filter: no log message for it */
  menu_item_t *arrows = menu_add_action(&m_log, "", 0, 0);
  if (arrows) {
    arrows->text = log_arrows;
    arrows->cursor = arrows_cursor;
    arrows->press = log_scroll;
    arrows->press_repeat = true;
    arrows->n_parts = 2;
    arrows->part_hints = log_arrow_hints;
  }
  for (int row = 0; row < LOG_ROWS; row++)
    menu_add_text(&m_log, text_log, row); /* grey: they can't be selected */
  menu_add_return(&m_log);
}

/* --- about, splash, root ------------------------------------------------------------------------- */

static menu_t m_about;

#define SPLASH_FRAMES 240 /* about 8 seconds, as gz's */

/* gz's start-up splash (bottom right): name and version, the build date under it, the website, how to
 * open the menu */
void menus_splash(void)
{
  static char title[64], date[64];
  snprintf(title, sizeof(title), "sloperinaGZ - %s", BUILD_VERSION);
  snprintf(date, sizeof(date), TF("Build Date: %s"), BUILD_WHEN);
  const char *lines[] = { title, date, "practicerom.com/oothd", 0, T("R + right stick down: open the menu") };
  static const uint8_t styles[] = { GFX_DIM, GFX_DIM, GFX_LOG, GFX_LOG, GFX_NORMAL };
  static const uint8_t indents[] = { 0, 0, 0, 0, 0 };
  gfx_splash(lines, styles, indents, ARRAY_LEN(lines), SPLASH_FRAMES);
}

/* the about page: the name, version, build date and description, then the website */
static void build_about(void)
{
  menu_init(&m_about, "about", 10);
  static char title[64];
  snprintf(title, sizeof(title), "sloperinaGZ - %s", BUILD_VERSION);
  menu_add_heading(&m_about, title);
  menu_add_text(&m_about, text_static, (intptr_t)BUILD_WHEN);
  menu_add_text(&m_about, text_static, (intptr_t) "gz-style practice menu, loaded by the game");
  menu_add_text(&m_about, text_static, (intptr_t) "font: Press Start 2P (codeman38, OFL)");
  menu_add_text(&m_about, text_static, (intptr_t) "based on the work of OoT N64's Practice ROM by glankk");
  menu_end_group(&m_about);
  add_blank_line(&m_about);
  menu_add_text(&m_about, text_static, (intptr_t) "practicerom.com/oothd");
  add_blank_line(&m_about);
  menu_add_return(&m_about);
}

static void build_root(void)
{
  menu_init(&menu_root, "sloperinaGZ", 14);
  menu_add_submenu(&menu_root, "link", &m_link);
  menu_add_submenu(&menu_root, "warps", &m_warps);
  menu_add_submenu(&menu_root, "scene", &m_scene);
  menu_add_submenu(&menu_root, "cheats", &m_cheats);
  menu_add_submenu(&menu_root, "inventory", &m_inventory);
  menu_add_submenu(&menu_root, "equips", &m_equips);
  menu_add_submenu(&menu_root, "story", &m_story);
  menu_add_submenu(&menu_root, "watches", &m_watches);
  menu_add_submenu(&menu_root, "debug", &m_debug);
  menu_add_submenu(&menu_root, "settings", &m_settings);
  menu_add_submenu(&menu_root, "log", &m_log);
  menu_add_submenu(&menu_root, "about", &m_about);
  menu_add(&menu_root, MI_BACK, "close");
}

/* the order matters: menu_init reserves each menu's items from one shared pool */
void menus_build(void)
{
  build_link();
  build_warps();
  build_cheats();
  build_inventory();
  build_equips();
  build_story();
  build_watches();
  build_scene();
  build_settings();
  build_debug();
  osk_build();
  build_log();
  build_about();
  build_root();
}
