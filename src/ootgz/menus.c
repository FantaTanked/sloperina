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

#define F_RUPEES     0xA77921D7u
#define F_WALLET_CAP 0xE0010A50u
#define F_MAGIC      0x76B18200u
#define F_MAGIC_CAP  0x12D63A97u

menu_t menu_root;

/* --- field helpers -------------------------------------------------------------------------- */

static bool fget(field_t f, int32_t *v)
{
  bool b;
  uint32_t e;
  switch (f.type) {
    case FT_INT: return gd_get_int(f.hash, v);
    case FT_BOOL: if (!gd_get_bool(f.hash, &b)) return false; *v = b; return true;
    case FT_ENUM: if (!gd_get_enum(f.hash, &e)) return false; *v = (int32_t)e; return true;
    default: return false;
  }
}

static void fset(field_t f, int32_t v)
{
  switch (f.type) {
    case FT_INT: gd_set_int(f.hash, v); break;
    case FT_BOOL: gd_set_bool(f.hash, v != 0); break;
    case FT_ENUM: gd_set_enum(f.hash, (uint32_t)v); break;
    default: break;
  }
}

static void mark_acquired(uint32_t h)
{
  if (h)
    gd_set_bool(h, true);
}

static bool same_label(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static int32_t int_or_unknown(uint32_t hash)
{
  int32_t v;
  return gd_get_int(hash, &v) ? v : NUM_UNKNOWN;
}

static uint32_t stock_hash(const char *item)
{
  char buf[128];
  snprintf(buf, sizeof(buf), "PouchData.%s.StockNum", item);
  return murmur3(buf);
}

static uint32_t isget_hash(const char *item)
{
  char buf[128];
  snprintf(buf, sizeof(buf), "IsGet.%s", item);
  return murmur3(buf);
}

/* --- link ------------------------------------------------------------------------------------ */

static menu_t m_link;

#define F_MAX_HEARTS 0xFBE01DA1u /* quarter hearts (game.py INT_HASH) */

static void hearts_text(int32_t q, char *buf, int len)
{
  static const char *const frac[] = { "", ".25", ".5", ".75" };
  snprintf(buf, len, "%d%s", (int)(q / 4), frac[q & 3]);
}

static int32_t get_health(menu_item_t *it)
{
  int32_t *cur, *max;
  return link_health(&cur, &max) ? *cur : NUM_UNKNOWN;
}
static int32_t max_health(menu_item_t *it)
{
  int32_t *cur, *max;
  return link_health(&cur, &max) ? *max : NUM_UNKNOWN;
}
static void set_health(menu_item_t *it, int32_t v)
{
  int32_t *cur, *max;
  if (link_health(&cur, &max))
    *cur = v;
}
static void fill_health(menu_item_t *it)
{
  int32_t *cur, *max;
  if (link_health(&cur, &max)) {
    *cur = *max;
    gfx_notice("health refilled");
  }
}
static void health_text(menu_item_t *it, char *buf, int len) { hearts_text(get_health(it), buf, len); }

/* heart containers: Link's maximum and the save field, together */
#define K_CONTAINERS (0x20u << 24)
static int pending_containers;
static int32_t get_containers(menu_item_t *it)
{
  int32_t *cur, *max;
  if (job_pending(K_CONTAINERS))
    return pending_containers;
  return link_health(&cur, &max) ? *max / 4 : NUM_UNKNOWN;
}
static int32_t max_containers(menu_item_t *it) { return 20; }
/* Heart containers: Link's max health (his status object), the save fields (max, and current
 * health when it's above the new max), then the HUD redrawn. Held for a second in case the game
 * puts its own values back, which is logged (so we learn which copy it reads). */
#define F_LIFE 0x31AB5580u /* PlayerStatus.Life, quarter hearts */
#define CONTAINER_HOLD_FRAMES 30

static void log_param(const char *what, const int32_t *v)
{
  const uint32_t *o = (const uint32_t *)((const uint8_t *)v - 8);
  log_printf("hearts: %s object %p: %08x %08x | %08x %08x %08x %08x", what, (void *)o, o[0], o[1], o[2], o[3],
             o[4], o[5]);
}

static bool job_containers(job_t *j)
{
  int32_t q = (int32_t)j->arg[0], *cur, *max, field, life;
  if (!link_health(&cur, &max)) {
    if (j->step == 0)
      log_printf("hearts: Link's health not found; will keep trying for a second");
    return j->step > CONTAINER_HOLD_FRAMES;
  }
  bool have_field = gd_get_int(F_MAX_HEARTS, &field), have_life = gd_get_int(F_LIFE, &life);
  if (j->i == 0) { /* first frame Link is found */
    log_param("current", cur);
    log_param("max", max);
    log_printf("hearts: containers %d -> %d (save field %d), health %d (save field %d)", (int)(*max / 4), (int)(q / 4),
               have_field ? (int)field : -1, (int)*cur, have_life ? (int)life : -1);
  } else if ((*max != q || *cur > q || (have_field && field != q) || (have_life && life > q)) && j->i < 4) {
    log_printf("hearts: frame %d: the game has max %d, field %d, health %d, field %d (we set %d)", j->step, (int)*max,
               have_field ? (int)field : -1, (int)*cur, have_life ? (int)life : -1, (int)q);
  }
  j->i++;
  /* written directly (value and its copy), not through the game's SetInt queue: queued sets
   * while scrolling left an earlier value behind */
  *max = q;
  if (*cur > q)
    *cur = q;
  if (!have_field || field != q)
    gd_poke_int(F_MAX_HEARTS, q);
  if (have_life && life > q)
    gd_poke_int(F_LIFE, q);
  if (j->i == 2 || j->step == CONTAINER_HOLD_FRAMES)
    hud_refresh_now();
  return j->step >= CONTAINER_HOLD_FRAMES;
}

static void set_containers(menu_item_t *it, int32_t n)
{
  if (n < 1)
    n = 1;
  pending_containers = n;
  job_submit(K_CONTAINERS, job_containers, n * 4, 0, 0, 0);
}

static int32_t get_rupees(menu_item_t *it) { return int_or_unknown(F_RUPEES); }
static int32_t max_rupees(menu_item_t *it) { return int_or_unknown(F_WALLET_CAP); }
static void set_rupees(menu_item_t *it, int32_t v) { gd_set_int(F_RUPEES, v); }
static void fill_rupees(menu_item_t *it)
{
  int32_t w = max_rupees(it);
  if (w != NUM_UNKNOWN) {
    gd_set_int(F_RUPEES, w);
    gfx_notice("wallet filled");
  }
}

static const char *const age_names[] = { "child", "adult" };
static int get_age(menu_item_t *it) { return age_wanted(); }
static void set_age(menu_item_t *it, int age) { age_want(age); }
static void act_reload_map(menu_item_t *it)
{
  gfx_logf("reload map (%s)", age_wanted() == AGE_ADULT ? "adult" : "child");
  age_reload_map();
  menu_close(); /* so the reload can be watched */
}

/* read-only lines */
static void text_static(menu_item_t *it, char *buf, int len) { snprintf(buf, len, "%s", (const char *)it->data); }

static void text_axis(menu_item_t *it, char *buf, int len)
{
  vec3f_t p;
  static const char axis[] = "xyz";
  if (link_drawn_pos(&p))
    snprintf(buf, len, "%c: %.2f", axis[it->data], (double)(it->data == 0 ? p.x : it->data == 1 ? p.y : p.z));
  else
    snprintf(buf, len, "%c: -", axis[it->data]);
}

static void text_age(menu_item_t *it, char *buf, int len)
{
  int a = age_current();
  snprintf(buf, len, TF("current age: %s"), T(a == AGE_ADULT ? "adult" : a == AGE_CHILD ? "child" : "-"));
}


/* the map as the warps menu names it, with the age */
static void text_map(menu_item_t *it, char *buf, int len)
{
  const char *m = gd_get_str(GD_STR_MAP);
  if (!m || !m[0]) {
    snprintf(buf, len, TF("map: -"));
    return;
  }
  for (int i = 0; i < data_n_warp_maps; i++)
    if (same_label(data_warp_maps[i].name, m)) {
      const warp_map_t *w = &data_warp_maps[i];
      snprintf(buf, len, TF("map: %s%s"), T(w->label),
               w->category == WC_CHILD ? " (child)" : w->category == WC_ADULT ? " (adult)" : "");
      return;
    }
  snprintf(buf, len, TF("map: %s"), m);
}

static void build_link(void)
{
  menu_init(&m_link, "link", 14);
  /* what can be changed first */
  menu_item_t *h = menu_add_number(&m_link, "health", get_health, set_health, max_health, fill_health, 0);
  if (h)
    h->text = health_text, h->hint = "d-left/right: change by a quarter heart   L: fill";
  menu_item_t *c = menu_add_number(&m_link, "heart containers", get_containers, set_containers, max_containers, 0, 0);
  if (c)
    c->num_min = 1;
  menu_add_number(&m_link, "rupees", get_rupees, set_rupees, max_rupees, fill_rupees, 0);
  menu_item_t *ag = menu_add_option(&m_link, "age", 2, age_names, get_age, set_age, 0);
  if (ag)
    ag->hint = "takes effect when the map reloads (reload map, below)";
  menu_item_t *rl = menu_add_action(&m_link, "reload map", act_reload_map, 0);
  if (rl)
    rl->hint = "L: reload this map at the age above";
  menu_add_return(&m_link);
  /* what can only be read, under return after a blank line */
  menu_add_text(&m_link, text_static, (intptr_t) "");
  menu_add_heading(&m_link, "position");
  for (int a = 0; a < 3; a++)
    menu_add_text(&m_link, text_axis, a);
  menu_end_group(&m_link);
  menu_add_text(&m_link, text_age, 0);
  menu_add_text(&m_link, text_map, 0);
}

/* --- warps ------------------------------------------------------------------------------------- */

#define MAX_WARP_MAPS 96
static menu_t m_warps, m_places, m_warp_cat[WC_COUNT], m_warp_map[MAX_WARP_MAPS];

static void text_warp_current(menu_item_t *it, char *buf, int len)
{
  const char *m = gd_get_str(GD_STR_MAP);
  snprintf(buf, len, TF("current  %s"), m ? m : "-");
}
static void text_warp_state(menu_item_t *it, char *buf, int len) { snprintf(buf, len, TF("warp     %s"), warp_state()); }

static void act_warp(menu_item_t *it)
{
  gfx_logf("warp to %s", data_warp_maps[it->data2].label);
  warp_arm(&data_warp_maps[it->data2], &data_spawns[it->data]);
  menu_close();
}
static void act_cancel_warp(menu_item_t *it) { warp_cancel(); }
static bool get_arrival(menu_item_t *it) { return warp_arrival_cutscenes; }
static void set_arrival(menu_item_t *it, bool on) { warp_arrival_cutscenes = on; }

static void build_warps(void)
{
  menu_init(&m_warps, "warps", 7);
  menu_add_submenu(&m_warps, "places", &m_places);
  menu_add_action(&m_warps, "cancel warp", act_cancel_warp, 0);
  menu_add_check(&m_warps, "arrival cutscenes", get_arrival, set_arrival, 0);
  menu_add_return(&m_warps);
  /* what can only be read, under return after a blank line */
  menu_add_text(&m_warps, text_static, (intptr_t) "");
  menu_add_text(&m_warps, text_warp_current, 0);
  menu_add_text(&m_warps, text_warp_state, 0);

  menu_init(&m_places, "places", WC_COUNT + 1);
  int per_cat[WC_COUNT] = { 0 };
  for (int i = 0; i < data_n_warp_maps; i++)
    per_cat[data_warp_maps[i].category]++;
  for (int c = 0; c < WC_COUNT; c++) {
    menu_init(&m_warp_cat[c], data_warp_categories[c], per_cat[c] + 1);
    if (per_cat[c])
      menu_add_submenu(&m_places, data_warp_categories[c], &m_warp_cat[c]);
  }
  menu_add_return(&m_places);
  for (int i = 0; i < data_n_warp_maps && i < MAX_WARP_MAPS; i++) {
    const warp_map_t *wm = &data_warp_maps[i];
    menu_init(&m_warp_map[i], wm->label, wm->n + 1);
    for (int s = wm->first; s < wm->first + wm->n; s++) {
      menu_item_t *it = menu_add_action(&m_warp_map[i], data_spawns[s].label, act_warp, s);
      if (it)
        it->data2 = i;
    }
    menu_add_return(&m_warp_map[i]);
    menu_add_submenu(&m_warp_cat[wm->category], wm->label, &m_warp_map[i]);
  }
  for (int c = 0; c < WC_COUNT; c++)
    menu_add_return(&m_warp_cat[c]);
}

/* --- cheats ------------------------------------------------------------------------------------ */

static menu_t m_cheats;
static bool get_cheat(menu_item_t *it) { return cheats[it->data]; }
static void set_cheat(menu_item_t *it, bool on) { cheats[it->data] = on; }

static void build_cheats(void)
{
  menu_init(&m_cheats, "cheats", CHEAT_COUNT + 3);
  menu_add_heading(&m_cheats, "infinite");
  for (int c = 0; c < CHEAT_COUNT; c++) {
    if (c == CHEAT_SPEED)
      menu_add_heading(&m_cheats, "player");
    menu_add_check(&m_cheats, cheat_names[c], get_cheat, set_cheat, c);
  }
  menu_add_return(&m_cheats);
}

/* --- inventory: owned items and gear ------------------------------------------------------------- */

static bool get_owned(menu_item_t *it)
{
  const inv_item_t *d = (const inv_item_t *)it->data;
  int32_t v;
  return fget(d->owned, &v) && v > 0;
}

/* Ticked: owned + acquired (the item screen needs both). Unticked: also taken off Link if he
 * wears it, and off the item buttons (FINDINGS §5.5, §5.6). */
static void set_owned(menu_item_t *it, bool on)
{
  const inv_item_t *d = (const inv_item_t *)it->data;
  fset(d->owned, on ? 1 : 0);
  if (on) {
    mark_acquired(d->acquired);
    if (d->button)
      pouch_clear_buttons_cancel(d->button);
  } else {
    if (d->worn_slot >= 0)
      pouch_unequip(d->worn_slot, d->worn_item);
    if (d->button)
      pouch_clear_buttons(d->button);
  }
}

static bool get_acquired(menu_item_t *it)
{
  bool b;
  return gd_get_bool(((const inv_item_t *)it->data)->acquired, &b) && b;
}
static void set_acquired(menu_item_t *it, bool on) { gd_set_bool(((const inv_item_t *)it->data)->acquired, on); }

/* trade slots and masks: one item at a time; trading switches an item button holding the old one */
static const char *trade_names[2][16];
static const char *mask_names[16];

static int get_trade(menu_item_t *it)
{
  const pouch_one_t *g = (const pouch_one_t *)it->data;
  int n = (int)it->data2;
  for (int i = 0; i < n; i++) {
    int32_t v;
    if (gd_get_int(stock_hash(g[i].item), &v) && v > 0)
      return i + 1;
  }
  return 0;
}

static const uint32_t button_slots[] = { 0x6BEE602D, 0x76F85E6D, 0xA0029837, 0xBC32CE24 };

static void set_trade(menu_item_t *it, int choice)
{
  const pouch_one_t *g = (const pouch_one_t *)it->data;
  int n = (int)it->data2;
  uint32_t olds[16];
  int n_old = 0;
  for (int i = 0; i < n; i++) {
    int32_t v;
    if (i + 1 != choice && gd_get_int(stock_hash(g[i].item), &v) && v > 0) {
      gd_set_int(stock_hash(g[i].item), 0);
      gd_set_bool(isget_hash(g[i].item), false);
      olds[n_old++] = murmur3(g[i].button);
    }
  }
  const pouch_one_t *now = choice > 0 ? &g[choice - 1] : 0;
  if (now) {
    gd_set_int(stock_hash(now->item), 1);
    gd_set_bool(isget_hash(now->item), true);
  }
  uint32_t target = murmur3(now ? now->button : "Empty");
  bool changed = false;
  for (unsigned s = 0; s < sizeof(button_slots) / sizeof(*button_slots); s++) {
    uint32_t v;
    for (int k = 0; k < n_old; k++)
      if (gd_get_enum(button_slots[s], &v) && v == olds[k]) {
        gd_set_enum(button_slots[s], target);
        changed = true;
      }
  }
  if (changed)
    pouch_refresh_screen(Z64HD_VT_HUD);
}

/* Owned items that are one choice (hookshot / longshot, the big swords): it->data = the group,
 * it->data2 = its size. One that goes is also taken off Link if he wears it. */
static int get_owned_group(menu_item_t *it)
{
  const inv_item_t *const *g = (const inv_item_t *const *)it->data;
  for (int i = (int)it->data2 - 1; i >= 0; i--) { /* the best one held */
    int32_t v;
    if (fget(g[i]->owned, &v) && v > 0)
      return i + 1;
  }
  return 0;
}

static void set_owned_group(menu_item_t *it, int choice)
{
  const inv_item_t *const *g = (const inv_item_t *const *)it->data;
  int n = (int)it->data2;
  const inv_item_t *now = choice > 0 && choice <= n ? g[choice - 1] : 0;
  uint32_t target = murmur3(now && now->button ? now->button : "Empty");
  bool changed = false;
  for (int i = 0; i < n; i++) {
    const inv_item_t *d = g[i];
    int32_t v;
    bool had = fget(d->owned, &v) && v > 0;
    fset(d->owned, d == now);
    if (d == now) {
      mark_acquired(d->acquired);
      if (d->button)
        pouch_clear_buttons_cancel(d->button);
      continue;
    }
    if (had && d->worn_slot >= 0)
      pouch_unequip(d->worn_slot, d->worn_item);
    if (had && d->button) {
      /* an item button holding the old one gets the new one (or nothing), as with trade items */
      pouch_clear_buttons_cancel(d->button);
      uint32_t old = murmur3(d->button);
      for (unsigned s = 0; s < sizeof(button_slots) / sizeof(*button_slots); s++) {
        uint32_t b;
        if (gd_get_enum(button_slots[s], &b) && b == old) {
          gd_set_enum(button_slots[s], target);
          changed = true;
        }
      }
    }
  }
  if (changed)
    pouch_refresh_screen(Z64HD_VT_HUD);
}

static const inv_item_t *hookshots[2];
static const char *const hookshot_names[] = { "none", "hookshot", "longshot" };
static const inv_item_t *big_swords[3];
/* The Stone of Agony is a pouch item here, Item_AgonyStone, worn in the head row of the equipment
 * screen beside the Lens of Truth (PouchTable: Pa_HeadEquip_00); it detects Gold Skulltulas. Held
 * like the lens: its pouch stock and IsGet. (The "Stone of Agony equipped" flag did nothing.) */
static const inv_item_t agony_stone = {
  "echo gem", IK_OWNED, { 0xF3B14CE2u /* PouchData.Item_AgonyStone.StockNum */, FT_INT },
  0xDD587ED8u /* IsGet.Item_AgonyStone */, NULL, -1, NULL,
};
static const char *const big_sword_names[] = { "none", "giant's knife", "broken giant's knife", "biggoron's sword" };

static int get_ocarina(menu_item_t *it) { return pouch_ocarina(); }
static void set_ocarina(menu_item_t *it, int choice)
{
  /* the D-pad (where the ocarina goes) only exists once the Navi tutorial has been seen */
  if (choice > 0)
    gd_set_bool(data_action_guide_navi, true);
  pouch_set_ocarina(choice);
}

/* --- inventory: amounts ------------------------------------------------------------------------- */

static int32_t get_ammo(menu_item_t *it) { return int_or_unknown(((const ammo_t *)it->data)->count); }
static int32_t max_ammo(menu_item_t *it)
{
  const ammo_t *a = (const ammo_t *)it->data;
  return a->cap ? int_or_unknown(a->cap) : a->fixed;
}
static void set_ammo(menu_item_t *it, int32_t v)
{
  const ammo_t *a = (const ammo_t *)it->data;
  gd_set_int(a->count, v);
  if (v > 0)
    mark_acquired(a->acquired);
}
static void fill_ammo(menu_item_t *it)
{
  int32_t m = max_ammo(it);
  if (m != NUM_UNKNOWN)
    set_ammo(it, m);
}

static int32_t get_number(menu_item_t *it) { return int_or_unknown(((const number_t *)it->data)->hash); }
static void set_number(menu_item_t *it, int32_t v) { gd_set_int(((const number_t *)it->data)->hash, v); }
static int32_t max_from_data2(menu_item_t *it) { return (int32_t)it->data2; }

static int32_t get_magic(menu_item_t *it) { return int_or_unknown(F_MAGIC); }
static int32_t max_magic(menu_item_t *it) { return int_or_unknown(F_MAGIC_CAP); }
static void set_magic(menu_item_t *it, int32_t v) { magic_set(v); }
static void fill_magic(menu_item_t *it)
{
  int32_t m = max_magic(it);
  if (m != NUM_UNKNOWN && m > 0)
    magic_set(m);
}

/* --- inventory: upgrades, bottles, flags ---------------------------------------------------------- */

static int get_upgrade(menu_item_t *it)
{
  int p = upgrade_pending_level((upgrade_t)it->data);
  return p >= 0 ? p : upgrade_level((upgrade_t)it->data);
}
static int upgrade_now(upgrade_t u)
{
  int p = upgrade_pending_level(u);
  return p >= 0 ? p : upgrade_level(u);
}

/* Link/Abilities/Strength (None, Low, Middle, High): the best of the bracelet and the gauntlets */
static void write_strength(int bracelet, int gauntlets)
{
  int s = gauntlets > 0 ? 1 + gauntlets : bracelet > 0 ? 1 : 0;
  gd_set_enum(data_strength.hash, data_strength_values[s]);
}

static void set_upgrade(menu_item_t *it, int level)
{
  upgrade_t u = (upgrade_t)it->data;
  upgrade_set(u, level);
  /* the ability fields game.py also wrote */
  if (u == UPG_GAUNTLETS && level < 3)
    write_strength(upgrade_now(UPG_BRACELET), level);
  else if (u == UPG_SCALE && level < 3)
    gd_set_float(data_diving_depth.hash, data_scale_depths[level]);
}

static bool get_bracelet(menu_item_t *it) { return upgrade_now(UPG_BRACELET) > 0; }
static void set_bracelet(menu_item_t *it, bool on)
{
  upgrade_set(UPG_BRACELET, on);
  write_strength(on, upgrade_now(UPG_GAUNTLETS));
}

static bool get_defense(menu_item_t *it)
{
  int p = upgrade_pending_level(UPG_DEFENSE);
  return (p >= 0 ? p : upgrade_level(UPG_DEFENSE)) > 0;
}
static void set_defense(menu_item_t *it, bool on)
{
  fset(data_double_defense.f[0], on);
  upgrade_set(UPG_DEFENSE, on);
}

static const char *bottle_choices[16];

static int get_bottle(menu_item_t *it)
{
  const bottle_t *b = &data_bottles[it->data];
  int32_t own;
  uint32_t v;
  if (!fget(b->own, &own))
    return -1;
  if (!own)
    return 0;
  if (!gd_get_enum(b->contents, &v))
    return -1;
  for (int i = 0; i < data_n_bottle_contents; i++)
    if (data_bottle_values[i] == v)
      return i + 1;
  return -1;
}
static void set_bottle(menu_item_t *it, int choice)
{
  const bottle_t *b = &data_bottles[it->data];
  fset(b->own, choice ? 1 : 0);
  gd_set_bool(b->available, choice != 0);
  if (choice) {
    gd_set_enum(b->contents, data_bottle_values[choice - 1]);
    mark_acquired(data_acquired_bottle);
    mark_acquired(data_bottle_acquired[choice - 1]);
  }
}

/* a toggle over one or two fields; it->data2 = a screen to refresh afterwards */
static bool get_flag(menu_item_t *it)
{
  int32_t v;
  return fget(((const flag_t *)it->data)->f[0], &v) && v;
}
static void set_flag(menu_item_t *it, bool on)
{
  const flag_t *f = (const flag_t *)it->data;
  for (int i = 0; i < 2; i++)
    if (f->f[i].type != FT_NONE)
      fset(f->f[i], on);
  if (on)
    mark_acquired(f->acquired);
  if (it->data2)
    pouch_refresh_screen((uintptr_t)it->data2);
}

static menu_item_t *add_flag(menu_t *m, const flag_t *f, uintptr_t refresh)
{
  menu_item_t *it = menu_add_check(m, f->label, get_flag, set_flag, (intptr_t)f);
  if (it)
    it->data2 = (intptr_t)refresh;
  return it;
}

static menu_t m_inventory, m_items, m_child_items, m_adult_items, m_universal, m_ammo, m_equipment, m_collection,
  m_capacity, m_magic, m_quest, m_songs, m_dungeon_items, m_dungeon[16];

/* An item by its data label (data_items, then data_gear). */
static const inv_item_t *item_by(const char *label)
{
  for (int i = 0; i < data_n_items; i++)
    if (same_label(data_items[i].label, label))
      return &data_items[i];
  for (int i = 0; i < data_n_gear; i++)
    if (same_label(data_gear[i].label, label))
      return &data_gear[i];
  log_printf("menus: no item \"%s\"", label);
  return 0;
}

/* Add the item with data label `label`, shown as `shown`: a checkbox (acquired / owned), or a switch
 * (trade slot, ocarina). */
static void add_item(menu_t *m, const char *shown, const char *label)
{
  const inv_item_t *d = item_by(label);
  if (!d)
    return;
  menu_item_t *it;
  switch (d->kind) {
    case IK_ACQUIRED:
      menu_add_check(m, shown, get_acquired, set_acquired, (intptr_t)d);
      break;
    case IK_OWNED:
      menu_add_check(m, shown, get_owned, set_owned, (intptr_t)d);
      break;
    case IK_TRADE_CHILD:
    case IK_TRADE_ADULT: {
      int adult = d->kind == IK_TRADE_ADULT;
      int n = adult ? data_n_trade_adult : data_n_trade_child;
      it = menu_add_option(m, shown, n + 1, trade_names[adult], get_trade, set_trade,
                           (intptr_t)(adult ? data_trade_adult : data_trade_child));
      if (it)
        it->data2 = n;
      break;
    }
    case IK_OCARINA:
      menu_add_option(m, shown, 3, ocarina_names, get_ocarina, set_ocarina, 0);
      break;
  }
}

static void build_inventory(void)
{
  menu_init(&m_inventory, "inventory", 11);
  menu_item_t *im = menu_add_submenu(&m_inventory, "items", &m_items);
  if (im)
    im->hint = "the game's L/R quick menus need 2+ items";
  menu_add_submenu(&m_inventory, "ammo", &m_ammo);
  menu_add_submenu(&m_inventory, "equipment items", &m_equipment);
  menu_add_submenu(&m_inventory, "collection", &m_collection);
  menu_add_submenu(&m_inventory, "quest items", &m_quest);
  menu_add_submenu(&m_inventory, "songs", &m_songs);
  menu_add_submenu(&m_inventory, "dungeon items", &m_dungeon_items);
  menu_add_return(&m_inventory);

  trade_names[0][0] = trade_names[1][0] = "none";
  for (int i = 0; i < data_n_trade_child; i++)
    trade_names[0][i + 1] = data_trade_child[i].label;
  for (int i = 0; i < data_n_trade_adult; i++)
    trade_names[1][i + 1] = data_trade_adult[i].label;

  /* items: child, adult, universal */
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
  hookshots[0] = item_by("hookshot"), hookshots[1] = item_by("longshot");
  menu_item_t *hs = menu_add_option(&m_adult_items, "hookshot", 3, hookshot_names, get_owned_group,
                                    set_owned_group, (intptr_t)hookshots);
  if (hs)
    hs->data2 = hookshots[1] ? 2 : 1;
  add_item(&m_adult_items, "adult trade item", "adult trade");
  menu_add_return(&m_adult_items);

  bottle_choices[0] = "none";
  for (int i = 0; i < data_n_bottle_contents && i < 15; i++)
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
  for (int b = 0; b < 5; b++)
    menu_add_option(&m_universal, bottle_labels[b], data_n_bottle_contents + 1, bottle_choices, get_bottle,
                    set_bottle, b);
  menu_add_return(&m_universal);

  menu_init(&m_ammo, "ammo", data_n_ammo + 2);
  for (int i = 0; i < data_n_ammo; i++)
    menu_add_number(&m_ammo, data_ammo[i].label, get_ammo, set_ammo, max_ammo, fill_ammo,
                    (intptr_t)&data_ammo[i]);
  menu_add_return(&m_ammo);

  /* equipment items */
  mask_names[0] = "none";
  for (int i = 0; i < data_n_masks && i < 15; i++)
    mask_names[i + 1] = data_masks[i].label;
  menu_init(&m_equipment, "equipment items", 21);
  menu_add_heading(&m_equipment, "swords");
  add_item(&m_equipment, "kokiri sword", "kokiri sword");
  add_item(&m_equipment, "master sword", "master sword");
  big_swords[0] = item_by("giant's knife"), big_swords[1] = item_by("broken giant's knife");
  big_swords[2] = item_by("biggoron's sword");
  if (big_swords[0] && big_swords[1] && big_swords[2]) {
    menu_item_t *bs = menu_add_option(&m_equipment, "big sword", 4, big_sword_names, get_owned_group,
                                      set_owned_group, (intptr_t)big_swords);
    if (bs)
      bs->data2 = 3;
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
  menu_item_t *mk = menu_add_option(&m_equipment, "mask slot", data_n_masks + 1, mask_names, get_trade, set_trade,
                                    (intptr_t)data_masks);
  if (mk)
    mk->data2 = data_n_masks; /* one mask at a time, like the trade slots */
  menu_add_return(&m_equipment);

  /* collection */
  menu_init(&m_collection, "collection", 15);
  /* checkboxes, then strength, then the switches and numbers */
  add_item(&m_collection, "fairy ocarina", "fairy ocarina");   /* owned flag: the collection screen */
  add_item(&m_collection, "ocarina of time", "ocarina of time");
  add_item(&m_collection, "gerudo card", "gerudo membership card");
  menu_add_check(&m_collection, "double defense", get_defense, set_defense, 0);
  menu_add_heading(&m_collection, "strength");
  menu_add_check(&m_collection, "goron's bracelet", get_bracelet, set_bracelet, 0);
  menu_add_option(&m_collection, "gauntlets", upgrades[UPG_GAUNTLETS].n_levels, upgrades[UPG_GAUNTLETS].level_name,
                  get_upgrade, set_upgrade, UPG_GAUNTLETS);
  menu_add_heading(&m_collection, "other");
  menu_add_option(&m_collection, "scale", upgrades[UPG_SCALE].n_levels, upgrades[UPG_SCALE].level_name,
                  get_upgrade, set_upgrade, UPG_SCALE);
  menu_item_t *sk = menu_add_number(&m_collection, "skulltulas", get_number, set_number, max_from_data2, 0,
                                    (intptr_t)&data_skulltulas);
  if (sk)
    sk->data2 = 100;
  menu_add_submenu(&m_collection, "ammo capacity", &m_capacity);
  menu_add_submenu(&m_collection, "magic", &m_magic);
  menu_end_group(&m_collection);
  menu_add_return(&m_collection);

  menu_init(&m_capacity, "ammo capacity", 7);
  for (int u = UPG_STICK; u <= UPG_WALLET; u++)
    menu_add_option(&m_capacity, upgrades[u].name, upgrades[u].n_levels, upgrades[u].level_name,
                    get_upgrade, set_upgrade, u);
  menu_add_return(&m_capacity);

  menu_init(&m_magic, "magic", 4);
  menu_item_t *mg = menu_add_number(&m_magic, "magic", get_magic, set_magic, max_magic, fill_magic, 0);
  if (mg)
    mg->hint = "d-left/right: change   L: fill (set the meter first)";
  menu_add_option(&m_magic, upgrades[UPG_MAGIC].name, upgrades[UPG_MAGIC].n_levels,
                  upgrades[UPG_MAGIC].level_name, get_upgrade, set_upgrade, UPG_MAGIC);
  menu_add_return(&m_magic);

  menu_init(&m_quest, "quest items", data_n_quest_items + 3);
  for (int i = 0; i < data_n_quest_items; i++) {
    if (i == 0)
      menu_add_heading(&m_quest, "spiritual stones"); /* data.c: the three stones, then the medallions */
    else if (i == 3)
      menu_add_heading(&m_quest, "medallions");
    add_flag(&m_quest, &data_quest_items[i], 0);
  }
  menu_add_return(&m_quest);

  menu_init(&m_songs, "songs", data_n_songs + 3);
  menu_add_heading(&m_songs, "normal songs");
  for (int i = 0; i < data_n_songs; i++) {
    if (same_label(data_songs[i].label, "minuet of forest")) /* data.c: the warp songs follow */
      menu_add_heading(&m_songs, "warp songs");
    add_flag(&m_songs, &data_songs[i], 0);
  }
  menu_add_return(&m_songs);

  /* each dungeon's small keys go in its own menu, after the map, compass and boss key (the key fields
   * are named a little differently from the dungeons) */
  static const struct { const char *keys, *dungeon; } key_dungeon[] = {
    { "forest temple", "forest temple" }, { "fire temple", "fire temple" }, { "water temple", "water temple" },
    { "shadow temple", "shadow temple" }, { "spirit temple", "spirit temple" },
    { "bottom of the well", "bottom of well" }, { "gerudo training ground", "gerudo training" },
    { "inside ganon castle", "ganon's castle" },
  };
  menu_init(&m_dungeon_items, "dungeon items", data_n_dungeons + 1);
  for (int d = 0; d < data_n_dungeons && d < 16; d++) {
    menu_init(&m_dungeon[d], data_dungeons[d].label, 5);
    for (int k = 0; k < 3; k++)
      add_flag(&m_dungeon[d], &data_dungeons[d].items[k], Z64HD_VT_MAP_SCREEN);
    for (unsigned j = 0; j < sizeof(key_dungeon) / sizeof(*key_dungeon); j++) {
      if (!same_label(key_dungeon[j].dungeon, data_dungeons[d].label))
        continue;
      for (int i = 0; i < data_n_small_keys; i++)
        if (same_label(data_small_keys[i].label, key_dungeon[j].keys)) {
          menu_item_t *it = menu_add_number(&m_dungeon[d], "small keys", get_number, set_number, max_from_data2, 0,
                                            (intptr_t)&data_small_keys[i]);
          if (it)
            it->data2 = 9;
        }
    }
    menu_add_return(&m_dungeon[d]);
    menu_add_submenu(&m_dungeon_items, data_dungeons[d].label, &m_dungeon[d]);
  }
  menu_add_return(&m_dungeon_items);

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

/* Anything else: drop the Weapon_/Armor_/Item_ prefix and its category, split the words, lower case. */
static void tidy_name(const char *v, char *buf, int len)
{
  static const char *const prefixes[] = { "Weapon_Sword_", "Weapon_Shield_", "Weapon_", "Armor_Adult_",
                                          "Armor_Child_", "Armor_", "Item_" };
  for (unsigned i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++) {
    const char *p = prefixes[i], *q = v;
    while (*p && *p == *q)
      p++, q++;
    if (!*p) {
      v = q;
      break;
    }
  }
  int n = 0;
  for (int i = 0; v[i] && n < len - 2; i++) {
    char c = v[i];
    if (c == '_')
      c = ' ';
    else if (c >= 'A' && c <= 'Z') {
      if (i && n && buf[n - 1] != ' ')
        buf[n++] = ' ';
      c += 32;
    }
    buf[n++] = c;
  }
  buf[n] = 0;
}

static void text_worn(menu_item_t *it, char *buf, int len)
{
  static const char *const names[] = { "sword", "shield", "tunic", "boots", "arrow", "ocarina" };
  static const int slots[] = { GD_STR_SWORD, GD_STR_SHIELD, GD_STR_TUNIC, GD_STR_BOOTS, GD_STR_ARROW,
                               GD_STR_OCARINA };
  const char *v = gd_get_str(slots[it->data]);
  char shown[48];
  if (!v || !v[0])
    snprintf(shown, sizeof(shown), "none");
  else {
    shown[0] = 0;
    for (unsigned i = 0; i < sizeof(worn_names) / sizeof(*worn_names) && !shown[0]; i++)
      if (same_label(v, worn_names[i].game))
        snprintf(shown, sizeof(shown), "%s", worn_names[i].shown);
    if (!shown[0])
      tidy_name(v, shown, sizeof(shown));
  }
  snprintf(buf, len, "%-9s %s", names[it->data], shown);
}

/* what Link wears (owned gear is in inventory > equipment items) */
static void build_equips(void)
{
  menu_init(&m_equips, "equips", 7);
  for (int i = 0; i < 6; i++)
    menu_add_text(&m_equips, text_worn, i);
  menu_add_return(&m_equips);
}

/* --- story ------------------------------------------------------------------------------------------ */

static menu_t m_story;

static int get_stage(menu_item_t *it)
{
  const quest_t *q = &data_quests[it->data];
  uint32_t v;
  if (!gd_get_enum(q->hash, &v))
    return -1;
  for (int i = 0; i < q->n_stages; i++)
    if (murmur3(q->stages[i]) == v)
      return i;
  return -1;
}
static void set_stage(menu_item_t *it, int i)
{
  const quest_t *q = &data_quests[it->data];
  gd_set_enum(q->hash, murmur3(q->stages[i]));
}
static void stage_text(menu_item_t *it, int i, char *buf, int len)
{
  const char *s = data_quests[it->data].stages[i];
  int n = 0;
  for (; s[n] && n < len - 1; n++)
    buf[n] = (s[n] >= 'A' && s[n] <= 'Z') ? s[n] + 32 : s[n];
  buf[n] = 0;
}

static void act_tutorials(menu_item_t *it)
{
  int n = 0;
  for (int i = 0; i < data_n_action_guides; i++)
    n += gd_set_bool(data_action_guides[i], true);
  char msg[48];
  snprintf(msg, sizeof(msg), "%d tutorials marked seen", n);
  gfx_notice(msg);
}

static void build_story(void)
{
  menu_init(&m_story, "story", data_n_quests + 4);
  menu_add_action(&m_story, "all tutorials seen", act_tutorials, 0);
  for (int i = 0; i < data_n_quests; i++) {
    menu_item_t *it = menu_add_option(&m_story, data_quests[i].label, data_quests[i].n_stages, 0, get_stage,
                                      set_stage, i);
    if (it)
      it->choice_text = stage_text, it->hint = "d-left/right or L: change stage (applies after a loading zone)";
  }
  menu_add_return(&m_story);
}

/* --- watches ------------------------------------------------------------------------------------ */

static menu_t m_watches, m_watch_add;
#define WATCH_ROWS 5 /* watches shown at once; gz's arrows scroll the rest */
static int watch_top, watch_first_row;
static uint32_t watches_built = ~0u;

static bool get_shown(menu_item_t *it) { return watches_shown; }
static void set_shown(menu_item_t *it, bool on) { watches_shown = on; }
static int get_corner(menu_item_t *it) { return watches_corner; }

/* move: the watches go to "custom" and the d-pad moves them, shown live; L leaves them there.
 * They glide while the d-pad is held (in thousandths of the picture per frame, faster the longer it's
 * held; a tap moves them one), rather than jumping on the d-pad's repeat. */
static int move_held;
static bool move_watches(menu_item_t *it)
{
  watches_moving = true;
  /* nvn.c keeps them whole on screen: follow that only when it really pushed them back (its value
   * is rounded, so taking it every frame made them creep) */
  if (gfx_custom_x >= 0 && (gfx_custom_x - watches_x > 3 || watches_x - gfx_custom_x > 3))
    watches_x = gfx_custom_x;
  if (gfx_custom_y >= 0 && (gfx_custom_y - watches_y > 3 || watches_y - gfx_custom_y > 3))
    watches_y = gfx_custom_y;
  if (input_pressed() & BTN_L) {
    move_held = 0;
    gfx_logf("watches moved");
    return false;
  }
  uint32_t h = input_held();
  int dx = !!(h & BTN_DRIGHT) - !!(h & BTN_DLEFT), dy = !!(h & BTN_DDOWN) - !!(h & BTN_DUP);
  move_held = dx || dy ? move_held + 1 : 0;
  int step = move_held == 0 ? 0 : move_held < 10 ? 1 : move_held < 30 ? 3 : move_held < 60 ? 6 : 10;
  watches_x += dx * step, watches_y += dy * step;
  watches_x = watches_x < 0 ? 0 : watches_x > 1000 ? 1000 : watches_x;
  watches_y = watches_y < 0 ? 0 : watches_y > 1000 ? 1000 : watches_y;
  return true;
}
static void act_move_watches(menu_item_t *it)
{
  if (watches_corner != GFX_CUSTOM) { /* start near the corner they're in */
    static const int16_t from[GFX_NPLACES][2] = {
      { 0, 0 }, { 1000, 0 }, { 0, 1000 }, { 1000, 1000 }, { 400, 0 }, { 400, 1000 }, { 0, 0 },
    };
    watches_x = from[watches_corner][0], watches_y = from[watches_corner][1];
    watches_corner = GFX_CUSTOM;
    gfx_custom_x = -1;
  }
  watches_moving = true;
  move_held = 0;
  menu_edit(it);
}
static void set_corner(menu_item_t *it, int c) { watches_corner = c; }
static void act_timer_toggle(menu_item_t *it) { timer_toggle(); }
static void act_timer_reset(menu_item_t *it) { timer_reset(); }
static void act_lag_reset(menu_item_t *it) { lag_reset(); }
static void act_jump_height_reset(menu_item_t *it)
{
  gfx_notice(jump_height_reset() ? "jump height reset" : "jump height: Link not found");
}

/* A row of the list, gz's buttons: the red cross (remove); the wrench (show the value in debug >
 * memory) when it has an address; the label (rename it with gz's keyboard); the type switch; then
 * the value and the arrows that move it up or down the list. d-left/right picks a button, L presses
 * it (the type: the next one). it->data = the row's index in watch_list.
 * The checkbox after the cross shows or hides the watch on screen without taking it off the list.
 * Columns in the line: cross 0, checkbox 2, wrench 5, label 7, type 20, value 28, arrows. */
enum { PART_REMOVE, PART_SHOW, PART_WRENCH, PART_LABEL, PART_TYPE, PART_UP, PART_DOWN };
#define ROW_VALUE_W 20
#define COL_SHOW 2   /* the checkbox (two cells): shown in the on-screen box or not */
#define COL_WRENCH 5
#define COL_LABEL 7
#define COL_UP (28 + ROW_VALUE_W + 1)

static int row_parts(const watch_entry_t *e, int *parts)
{
  int n, cur, k = 0;
  watch_entry_types(e, &n, &cur);
  parts[k++] = PART_REMOVE;
  parts[k++] = PART_SHOW;
  if (watch_entry_has_address(e))
    parts[k++] = PART_WRENCH;
  parts[k++] = PART_LABEL;
  if (n)
    parts[k++] = PART_TYPE;
  parts[k++] = PART_UP;
  parts[k++] = PART_DOWN;
  return k;
}

static int row_part(const menu_item_t *it)
{
  int parts[8], k = row_parts(&watch_list[it->data], parts);
  return parts[it->part < k ? it->part : k - 1];
}

static void watch_row(menu_item_t *it, char *buf, int len)
{
  if (it->data >= watch_count) {
    snprintf(buf, len, "-");
    return;
  }
  const watch_entry_t *e = &watch_list[it->data];
  char label[24], v[48];
  int n, cur;
  const char *const *types = watch_entry_types(e, &n, &cur);
  watch_entry_label(e, label, sizeof(label));
  watch_entry_text(e, v, sizeof(v));
  snprintf(buf, len, "%c%c %c %-12.12s %s%-3s%s %-*.*s %c %c", e->hidden ? GFX_CHECK_OFF : GFX_CHECK_ON, GFX_WIDE_REST,
           watch_entry_has_address(e) ? GFX_ICON_WRENCH : ' ', label,
           n ? "< " : "  ", n ? types[cur] : "", n ? " >" : "  ", ROW_VALUE_W, ROW_VALUE_W, v, GFX_ICON_UP,
           GFX_ICON_DOWN);
}

/* where the chosen button is in the line (the line starts with the cross: "x " + the row text) */
static bool watch_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  switch (row_part(it)) {
    case PART_REMOVE: *start = 0, *len = 1; return true;
    case PART_SHOW: *start = COL_SHOW, *len = 2; return true;
    case PART_WRENCH: *start = COL_WRENCH, *len = 1; return true;
    case PART_LABEL: {
      char label[24];
      watch_entry_label(&watch_list[it->data], label, sizeof(label));
      int l = (int)strlen(label);
      *start = COL_LABEL, *len = l < 1 ? 1 : l > 12 ? 12 : l;
      return true;
    }
    case PART_UP: *start = COL_UP, *len = 1; return true;
    case PART_DOWN: *start = COL_UP + 2, *len = 1; return true;
    default: {
      int a = COL_LABEL + 12;
      while (line[a] && line[a] != '<')
        a++;
      int b = a;
      while (line[b] && line[b] != '>')
        b++;
      *start = a, *len = line[b] ? b - a + 1 : 0;
      return true;
    }
  }
}

static void watch_renamed(const char *text, intptr_t index)
{
  char before[24];
  watch_entry_label(&watch_list[index], before, sizeof(before));
  watch_set_label((int)index, text);
  if (text[0])
    gfx_logf("renamed %s to %s", before, text);
  else
    gfx_logf("%s: its own name again", before);
}

static void watch_press(menu_item_t *it, int part)
{
  int i = (int)it->data;
  if (i >= watch_count)
    return;
  watch_entry_t *e = &watch_list[i];
  char label[24];
  watch_entry_label(e, label, sizeof(label));
  switch (row_part(it)) {
    case PART_REMOVE:
      gfx_logf("removed watch %s", label);
      watch_remove(i);
      break;
    case PART_SHOW:
      watch_set_hidden(i, !e->hidden);
      gfx_logf("%s %s", label, e->hidden ? "hidden" : "shown");
      break;
    case PART_WRENCH: {
      uintptr_t a;
      int t;
      if (watch_entry_address(e, &a, &t))
        memview_open(a, t);
      else
        gfx_logf("%s: not in memory right now", label);
      break;
    }
    case PART_LABEL:
      osk_open("label", e->label[0] ? e->label : label, watch_renamed, i);
      break;
    case PART_UP:
    case PART_DOWN: {
      int dir = row_part(it) == PART_UP ? -1 : 1, j = i + dir;
      if (j < 0 || j >= watch_count)
        break;
      /* the window follows the row, and the cursor goes with it, on the same arrow */
      if (j < watch_top)
        watch_top = j;
      else if (j >= watch_top + WATCH_ROWS)
        watch_top = j - WATCH_ROWS + 1;
      int sel = watch_first_row + (j - watch_top);
      if (sel < m_watches.n_items)
        m_watches.items[sel].part = it->part, m_watches.selected = sel;
      watch_move(i, dir);
      break;
    }
    default: {
      int n, cur;
      const char *const *types = watch_entry_types(e, &n, &cur);
      if (n) {
        watch_entry_set_type(i, cur + 1);
        types = watch_entry_types(e, &n, &cur);
        gfx_logf("%s: %s", label, types[cur]);
      }
      break;
    }
  }
}

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
#define CW_DIGITS 10
static uintptr_t cw_addr;
static int cw_pos, cw_type = MW_X32;

/* The row under the list, gz's buttons with no names (the hint says what they are): the folder
 * (known watches) and the + (custom watch). While typing an address it follows the +. */
enum { TOOL_KNOWN, TOOL_CUSTOM };
static const char *const tool_hints[] = { "known watches: the built-in ones (L: open)",
                                          "custom watch: a memory address of your choosing (L: type it)" };

static void tools_text(menu_item_t *it, char *buf, int len)
{
  if (menu_editing == it)
    snprintf(buf, len, "%c%c %c  %0*lx  %s", GFX_ICON_FOLDER, GFX_WIDE_REST, GFX_ICON_ADD, CW_DIGITS,
             (unsigned long)cw_addr, memory_type_names[cw_type]);
  else
    snprintf(buf, len, "%c%c %c", GFX_ICON_FOLDER, GFX_WIDE_REST, GFX_ICON_ADD);
}

static bool tools_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  if (menu_editing == it)
    *start = 6 + cw_pos, *len = 1; /* the digit, after "F+ +  " */
  else if (it->part == TOOL_KNOWN)
    *start = 0, *len = 2;
  else
    *start = 3, *len = 1;
  return true;
}

static bool custom_watch_edit(menu_item_t *it)
{
  if (input_repeat(BTN_DLEFT))
    cw_pos = cw_pos > 0 ? cw_pos - 1 : 0;
  else if (input_repeat(BTN_DRIGHT))
    cw_pos = cw_pos < CW_DIGITS - 1 ? cw_pos + 1 : CW_DIGITS - 1;
  else if (input_repeat(BTN_DUP) || input_repeat(BTN_DDOWN)) {
    int shift = (CW_DIGITS - 1 - cw_pos) * 4, dir = input_repeat(BTN_DUP) ? 1 : -1;
    uintptr_t nib = ((cw_addr >> shift & 0xF) + (uintptr_t)(dir + 16)) & 0xF;
    cw_addr = (cw_addr & ~((uintptr_t)0xF << shift)) | nib << shift;
  } else if (input_pressed() & BTN_L) {
    if (watch_add_memory(cw_addr, cw_type))
      gfx_logf("added watch %0*lx", CW_DIGITS, (unsigned long)cw_addr);
    else
      gfx_notice("that address is already watched");
    return false;
  }
  return true;
}

static void act_custom_watch(menu_item_t *it);

static void tools_press(menu_item_t *it, int part)
{
  if (part == TOOL_KNOWN)
    menu_goto(&m_watch_add);
  else
    act_custom_watch(it);
}

static void act_custom_watch(menu_item_t *it)
{
  cw_addr = z64hd_main, cw_type = MW_X32;
  for (int i = watch_count - 1; i >= 0; i--)
    if (watch_list[i].id == W_MEMORY) {
      cw_addr = watch_list[i].addr, cw_type = watch_list[i].type;
      break;
    }
  cw_pos = CW_DIGITS - 1;
  menu_edit(it);
}

/* The watches menu, rebuilt whenever the list changes (watches_sync). */
/* gz's arrows over the list (as in debug > memory) when there are more watches than WATCH_ROWS */
static void watch_arrows(menu_item_t *it, char *buf, int len)
{
  int last = watch_top + WATCH_ROWS < watch_count ? watch_top + WATCH_ROWS : watch_count;
  snprintf(buf, len, "%c %c  %d-%d / %d", GFX_ICON_UP, GFX_ICON_DOWN, watch_top + 1, last, watch_count);
}

static bool watch_arrows_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = it->part ? 2 : 0, *len = 1;
  return true;
}

static void watch_scroll(menu_item_t *it, int part)
{
  int max = watch_count > WATCH_ROWS ? watch_count - WATCH_ROWS : 0, top = watch_top + (part ? 1 : -1);
  top = top < 0 ? 0 : top > max ? max : top;
  if (top != watch_top)
    watch_top = top, watches_built = ~0u; /* the rows show other watches: rebuilt */
}

static const char *const watch_arrow_hints[] = { "L: scroll the list up (held: keeps going)",
                                                 "L: scroll the list down (held: keeps going)" };

static void fill_watches(void)
{
  /* the cursor stays on the folder when a watch is added from it */
  bool on_tools = m_watches.n_items && m_watches.items[m_watches.selected].text == tools_text;
  int tools_part = on_tools ? m_watches.items[m_watches.selected].part : 0;
  int part = m_watches.n_items ? m_watches.items[m_watches.selected].part : 0;
  menu_reset(&m_watches);
  menu_add_check(&m_watches, "show on screen", get_shown, set_shown, 0);
  menu_add_heading(&m_watches, "watches box");
  menu_add_option(&m_watches, "position", GFX_NPLACES, watches_corner_names, get_corner, set_corner, 0);
  menu_item_t *mv = menu_add_action(&m_watches, "move", act_move_watches, 0);
  if (mv)
    mv->edit = move_watches, mv->edit_alone = true, mv->hint = "L: move the watches around the screen",
    mv->edit_hint = "watches: d-pad moves them   L: done";
  menu_add_heading(&m_watches, "watches");
  int max_top = watch_count > WATCH_ROWS ? watch_count - WATCH_ROWS : 0;
  watch_top = watch_top < 0 ? 0 : watch_top > max_top ? max_top : watch_top;
  if (watch_count > WATCH_ROWS) {
    menu_item_t *ar = menu_add_action(&m_watches, "", 0, 0);
    if (ar)
      ar->text = watch_arrows, ar->cursor = watch_arrows_cursor, ar->press = watch_scroll, ar->press_repeat = true,
      ar->n_parts = 2, ar->part_hints = watch_arrow_hints;
  }
  watch_first_row = m_watches.n_items;
  for (int i = watch_top; i < watch_count && i < watch_top + WATCH_ROWS; i++) {
    menu_item_t *it = menu_add_action(&m_watches, "", 0, i);
    if (!it)
      continue;
    int n, cur;
    watch_entry_types(&watch_list[i], &n, &cur);
    int parts[8];
    it->text = watch_row, it->icon = GFX_ICON_REMOVE, it->cursor = watch_cursor, it->press = watch_press;
    it->n_parts = row_parts(&watch_list[i], parts);
    it->hint = "d-left/right: pick a button   L: press it";
  }
  menu_item_t *tools = menu_add_action(&m_watches, "", 0, 0);
  if (tools) {
    tools->text = tools_text, tools->cursor = tools_cursor, tools->press = tools_press, tools->n_parts = 2;
    tools->part_hints = tool_hints, tools->edit = custom_watch_edit;
    tools->edit_hint = "d-left/right: digit   d-up/down: change   L: add";
    if (on_tools)
      m_watches.selected = (int)(tools - m_watches.items), tools->part = tools_part;
  }
  menu_add_heading(&m_watches, "timer");
  menu_add_action(&m_watches, "start/stop timer", act_timer_toggle, 0);
  menu_add_action(&m_watches, "reset timer", act_timer_reset, 0);
  menu_add_action(&m_watches, "reset lag counter", act_lag_reset, 0);
  menu_item_t *jh = menu_add_action(&m_watches, "reset jump height (R + A + d-right)", act_jump_height_reset, 0);
  if (jh)
    jh->hint = "L: back to none (any height)";
  menu_add_return(&m_watches);
  menu_refresh(&m_watches);
  menu_item_t *sel = &m_watches.items[m_watches.selected];
  if (sel->n_parts > 1)
    sel->part = part < sel->n_parts ? part : sel->n_parts - 1;
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
    menu_item_t *it = menu_add_action(&m_watch_add, watch_labels[w], act_watch_add, w);
    if (it)
      it->text = watch_add_state, it->hint = "L: add to the list";
  }
  menu_add_return(&m_watch_add);
}

/* --- scene ------------------------------------------------------------------------------------ */

static menu_t m_scene, m_collision;
static bool get_aw_shown(menu_item_t *it) { return airwalls_shown; }
static void set_aw_shown(menu_item_t *it, bool on) { airwalls_shown = on; }
static bool get_aw_all(menu_item_t *it) { return airwalls_all; }
static void set_aw_all(menu_item_t *it, bool on) { airwalls_all = on; }
static void text_aw_summary(menu_item_t *it, char *buf, int len)
{
  char v[64];
  airwall_summary(v, sizeof(v));
  snprintf(buf, len, TF("count  %s"), v);
}
static void text_aw_link(menu_item_t *it, char *buf, int len)
{
  char v[64];
  airwall_link_text(v, sizeof(v));
  snprintf(buf, len, TF("link   %s"), v);
}

static void build_scene(void)
{
  menu_init(&m_scene, "scene", 3);
  menu_item_t *rl = menu_add_action(&m_scene, "reload map", act_reload_map, 0);
  if (rl)
    rl->hint = "L: reload this map (at the age set in the link menu)";
  menu_add_submenu(&m_scene, "collision", &m_collision);
  menu_add_return(&m_scene);

  menu_init(&m_collision, "collision", 6);
  menu_add_heading(&m_collision, "air walls");
  menu_item_t *sh = menu_add_check(&m_collision, "show", get_aw_shown, set_aw_shown, 0);
  if (sh)
    sh->hint = "cyan: stop Link   red: he's inside it   violet: other kinds", sh->log_label = "air walls:";
  menu_item_t *al = menu_add_check(&m_collision, "all kinds", get_aw_all, set_aw_all, 0);
  if (al)
    al->hint = "also the walls for enemies, NPCs and objects", al->log_label = "all kinds:";
  menu_add_text(&m_collision, text_aw_summary, 0);
  menu_add_text(&m_collision, text_aw_link, 0);
  menu_add_return(&m_collision);
}

/* --- settings --------------------------------------------------------------------------------- */

static menu_t m_settings;
static const char *const opacity_names[] = { "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%",
                                             "90%", "100%" };
static bool get_input_display(menu_item_t *it) { return input_display; }
static bool get_input_bg(menu_item_t *it) { return input_display_bg; }
static void set_input_bg(menu_item_t *it, bool on) { input_display_bg = on; }
static void set_input_display(menu_item_t *it, bool on) { input_display = on; }
static const char *const size_names[] = { "auto", "small (2x)", "medium (2.5x)", "large (3x)" };
static int get_size(menu_item_t *it) { return gfx_menu_scale; }
static void set_size(menu_item_t *it, int s) { gfx_menu_scale = s; }
static int get_opacity(menu_item_t *it) { return gfx_opacity / 10; }
static void set_opacity(menu_item_t *it, int i) { gfx_opacity = i * 10; }

static bool get_levitate(menu_item_t *it) { return levitate_enabled; }
static void set_levitate(menu_item_t *it, bool on) { levitate_enabled = on; }
static const char *const lev_speed_names[] = { "2", "4", "6", "8", "12", "20" };
static const int lev_speeds[] = { 2, 4, 6, 8, 12, 20 };
static int get_lev_speed(menu_item_t *it)
{
  for (int i = 0; i < 6; i++)
    if (lev_speeds[i] == levitate_speed)
      return i;
  return -1;
}
static void set_lev_speed(menu_item_t *it, int i) { levitate_speed = lev_speeds[i]; }
static void text_levitate(menu_item_t *it, char *buf, int len) { snprintf(buf, len, TF("status  %s"), levitate_state()); }
static bool get_walls(menu_item_t *it) { return levitate_through_walls; }
static void set_walls(menu_item_t *it, bool on) { levitate_through_walls = on; }
static const char *const lev_move_names[] = { "2", "4", "6", "8", "12", "20", "30" };
static const int lev_moves[] = { 2, 4, 6, 8, 12, 20, 30 };
static int get_lev_move(menu_item_t *it)
{
  for (int i = 0; i < 7; i++)
    if (lev_moves[i] == levitate_move_speed)
      return i;
  return -1;
}
static void set_lev_move(menu_item_t *it, int i) { levitate_move_speed = lev_moves[i]; }

static menu_t m_input_display, m_levitate, m_frame_advance;
static bool get_fa(menu_item_t *it) { return frame_advance; }
static void set_fa(menu_item_t *it, bool on) { frame_advance = on; }
static bool get_pause_display(menu_item_t *it) { return pause_display; }
static const char *const fa_button_names[] = { "L", "R" };
static int get_fa_button(menu_item_t *it) { return frame_advance_button; }
static void set_fa_button(menu_item_t *it, int b) { frame_advance_button = b; }
static void set_pause_display(menu_item_t *it, bool on) { pause_display = on; }
/* the languages are found on the SD card after the menus are built: the switch reads them each time */
static int get_lang(menu_item_t *it)
{
  it->n_choices = lang_count(), it->choices = lang_names();
  return lang_current();
}
static void set_lang(menu_item_t *it, int i) { lang_select(i); }
static const char *const input_style_names[] = { "row", "controller" };
static int get_input_style(menu_item_t *it) { return input_style; }
static const char *const input_size_names[] = { "small", "medium", "large" };
static int get_input_size(menu_item_t *it) { return input_size; }
static void set_input_size(menu_item_t *it, int s) { input_size = s; }
static void set_input_style(menu_item_t *it, int s) { input_style = s; }
static bool get_input_left(menu_item_t *it) { return input_show_left; }
static void set_input_left(menu_item_t *it, bool on) { input_show_left = on; }
static bool get_input_right(menu_item_t *it) { return input_show_right; }
static void set_input_right(menu_item_t *it, bool on) { input_show_right = on; }
static bool get_input_buttons(menu_item_t *it) { return input_show_buttons; }
static void set_input_buttons(menu_item_t *it, bool on) { input_show_buttons = on; }

static void build_settings(void)
{
  menu_init(&m_settings, "settings", 10);
  menu_add_heading(&m_settings, "menu settings");
  menu_item_t *lg = menu_add_option(&m_settings, "language", 1, lang_names(), get_lang, set_lang, 0);
  if (lg)
    lg->hint = "files in sd:/ootgz/lang (see lang/template.json)";
  menu_item_t *sz = menu_add_option(&m_settings, "size", 4, size_names, get_size, set_size, 0);
  if (sz)
    sz->hint = "text size of the menu, watches and log (auto: for the screen)";
  menu_item_t *op = menu_add_option(&m_settings, "background opacity", 11, opacity_names, get_opacity, set_opacity, 0);
  if (op)
    op->hint = "below 100%: costs some speed, and what's behind lags 2 frames";
  menu_end_group(&m_settings);
  menu_add_heading(&m_settings, "other settings");
  menu_add_submenu(&m_settings, "input display", &m_input_display);
  menu_add_submenu(&m_settings, "levitate", &m_levitate);
  menu_add_submenu(&m_settings, "frame advance", &m_frame_advance);
  menu_end_group(&m_settings);
  menu_add_return(&m_settings);

  menu_init(&m_frame_advance, "frame advance", 5);
  menu_add_heading(&m_frame_advance, "frame advance options");
  menu_item_t *fa = menu_add_check(&m_frame_advance, "enabled", get_fa, set_fa, 0);
  if (fa)
    fa->hint = "in game, tap the button: pause; tap: a frame; button + L3: run", fa->log_label = "frame advance";
  menu_item_t *pd = menu_add_check(&m_frame_advance, "pause icon", get_pause_display, set_pause_display, 0);
  if (pd)
    pd->hint = "gz's pause display at the top left while paused";
  menu_item_t *fb = menu_add_option(&m_frame_advance, "button", 2, fa_button_names, get_fa_button, set_fa_button, 0);
  if (fb)
    fb->hint = "the button for frame advance (the game no longer sees it while it's on)",
    fb->log_label = "frame advance button";
  menu_end_group(&m_frame_advance);
  menu_add_return(&m_frame_advance);

  menu_init(&m_input_display, "input display", 10);
  menu_add_heading(&m_input_display, "display options");
  menu_item_t *id = menu_add_check(&m_input_display, "show", get_input_display, set_input_display, 0);
  if (id)
    id->hint = "the sticks and the buttons held, at the bottom left (hidden while the menu is open)",
    id->log_label = "input display";
  menu_item_t *ib = menu_add_check(&m_input_display, "background", get_input_bg, set_input_bg, 0);
  if (ib)
    ib->hint = "behind the whole input display, as the watches have";
  menu_add_check(&m_input_display, "left stick", get_input_left, set_input_left, 0);
  menu_add_check(&m_input_display, "right stick", get_input_right, set_input_right, 0);
  menu_add_check(&m_input_display, "buttons", get_input_buttons, set_input_buttons, 0);
  menu_item_t *st = menu_add_option(&m_input_display, "style", 2, input_style_names, get_input_style, set_input_style, 0);
  if (st)
    st->hint = "row: gz's line   controller: a Switch 2 Pro Controller, the sticks' values under it",
    st->log_label = "input display style";
  menu_item_t *is = menu_add_option(&m_input_display, "size", 3, input_size_names, get_input_size, set_input_size, 0);
  if (is)
    is->hint = "small, medium, large: half, three quarters or all of the menu's text size",
    is->log_label = "input display size";
  menu_end_group(&m_input_display);
  menu_add_return(&m_input_display);

  menu_init(&m_levitate, "levitate", 9);
  menu_add_heading(&m_levitate, "levitation options");
  menu_item_t *lv = menu_add_check(&m_levitate, "enabled", get_levitate, set_levitate, 0);
  if (lv)
    lv->hint = "L3: start/stop floating   R3: hold height", lv->log_label = "levitate";
  menu_add_check(&m_levitate, "through walls", get_walls, set_walls, 0);
  menu_add_option(&m_levitate, "rise speed", 6, lev_speed_names, get_lev_speed, set_lev_speed, 0);
  menu_add_option(&m_levitate, "move speed", 7, lev_move_names, get_lev_move, set_lev_move, 0);
  menu_end_group(&m_levitate);
  menu_add_return(&m_levitate);
  /* what can only be read, under return after a blank line */
  menu_add_text(&m_levitate, text_static, (intptr_t) "");
  menu_add_text(&m_levitate, text_levitate, 0);
}

/* --- debug (gz's debug menu) ----------------------------------------------------------------------- */

static menu_t m_debug;

/* debug > heap: gz shows its game's arena; here, what the console says of the game's memory
 * (svcGetInfo) and how much of the heap region is mapped (svcQueryMemory, refreshed every second). */
static menu_t m_heap;
static struct { uint64_t start, size, mapped, blocks, total, used; uint32_t at; bool ok; } heap;

static void heap_update(void)
{
  uint32_t now = frame_counter();
  if (heap.ok && now - heap.at < 30)
    return;
  heap.at = now;
  heap.ok = sys_get_info(4, 0, &heap.start) && sys_get_info(5, 0, &heap.size);
  if (!sys_get_info(6, 0, &heap.total))
    heap.total = 0;
  if (!sys_get_info(7, 0, &heap.used))
    heap.used = 0;
  heap.mapped = heap.blocks = 0;
  for (uint64_t a = heap.start; heap.ok && a < heap.start + heap.size;) {
    sys_meminfo_t mi;
    if (sys_query_memory(&mi, a) != 0 || mi.size == 0)
      break;
    if ((mi.type & 0xFF) == 5) /* MemoryType Heap */
      heap.mapped += mi.size, heap.blocks++;
    a = mi.addr + mi.size;
  }
}

static void mib(uint64_t v, char *buf, int len)
{
  snprintf(buf, len, "%llu.%01llu MiB", (unsigned long long)(v >> 20), (unsigned long long)((v & 0xFFFFF) * 10 >> 20));
}

static void text_heap(menu_item_t *it, char *buf, int len)
{
  heap_update();
  char a[24], b[24];
  switch (it->data) {
    case 0:
      if (!heap.ok)
        snprintf(buf, len, "%s", T("heap region   unknown"));
      else
        mib(heap.size, a, sizeof(a)), snprintf(buf, len, TF("heap region   %010llx  %s"),
                                               (unsigned long long)heap.start, a);
      break;
    case 1:
      mib(heap.mapped, a, sizeof(a));
      snprintf(buf, len, TF("heap in use   %s in %d blocks"), a, (int)heap.blocks);
      break;
    case 2:
      mib(heap.total, a, sizeof(a)), mib(heap.used, b, sizeof(b));
      snprintf(buf, len, TF("memory        %s of %s used"), b, a);
      break;
    default:
      mib(heap.total > heap.used ? heap.total - heap.used : 0, a, sizeof(a));
      snprintf(buf, len, TF("memory free   %s"), a);
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
#define N_PAUSE_FIELDS ((int)(sizeof(pause_fields) / sizeof(*pause_fields)))
static bool pause_log;
static int32_t pause_last[N_PAUSE_FIELDS];
static bool pause_seen;

static int32_t pause_value(int i)
{
  if (pause_fields[i].is_int) {
    int32_t v;
    return gd_get_int(pause_fields[i].hash, &v) ? v : NUM_UNKNOWN;
  }
  bool b;
  return gd_get_bool(pause_fields[i].hash, &b) ? b : NUM_UNKNOWN;
}

void menus_research_frame(void)
{
  if (!pause_log)
    return;
  for (int i = 0; i < N_PAUSE_FIELDS; i++) {
    int32_t v = pause_value(i);
    if (pause_seen && v != pause_last[i])
      log_printf("pause research: frame %u: %s %d -> %d", (unsigned)frame_counter(), pause_fields[i].name,
                 (int)pause_last[i], (int)v);
    pause_last[i] = v;
  }
  pause_seen = true;
}

static bool get_pause_log(menu_item_t *it) { return pause_log; }
static void set_pause_log(menu_item_t *it, bool on) { pause_log = on, pause_seen = false; }
static int32_t get_pause_int(menu_item_t *it) { return pause_value((int)it->data); }
static void set_pause_int(menu_item_t *it, int32_t v) { gd_set_int(pause_fields[it->data].hash, v); }
static bool get_pause_bool(menu_item_t *it) { return pause_value((int)it->data) == 1; }
static void set_pause_bool(menu_item_t *it, bool on) { gd_set_bool(pause_fields[it->data].hash, on); }

static void build_pause_research(void)
{
  menu_init(&m_pause_research, "pause research", N_PAUSE_FIELDS + 4);
  menu_item_t *lg = menu_add_check(&m_pause_research, "log changes", get_pause_log, set_pause_log, 0);
  if (lg)
    lg->hint = "every change of these to the log, also with the menu closed (open the game's pause screen)";
  menu_add_heading(&m_pause_research, "Sequence_");
  for (int i = 0; i < N_PAUSE_FIELDS; i++) {
    menu_item_t *it = pause_fields[i].is_int
                        ? menu_add_number(&m_pause_research, pause_fields[i].name, get_pause_int, set_pause_int, 0, 0, i)
                        : menu_add_check(&m_pause_research, pause_fields[i].name, get_pause_bool, set_pause_bool, i);
    if (it)
      it->hint = "the game's own value; changing it is an experiment";
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
  menu_item_t *hm = menu_add_action(&m_heap, "view the heap in memory", act_heap_memory, 0);
  if (hm)
    hm->hint = "L: debug > memory at the start of the heap";
  menu_add_return(&m_heap);
  menu_add_text(&m_heap, text_static, (intptr_t) "");
  for (int i = 0; i < 4; i++)
    menu_add_text(&m_heap, text_heap, i);
}

/* --- log, about, root ------------------------------------------------------------------------------ */

static menu_t m_log, m_about;


/* The log menu: the module's last LOG_LINES messages, newest first, filtered by type (the word(s)
 * before a message's ':', e.g. "warp", "settings"; or "problems") and scrolled with gz's arrows. */
#define LOG_ROWS 12
#define LOG_TYPES 24
static char log_types[LOG_TYPES][20];
static const char *log_type_names[LOG_TYPES + 2];
static int n_log_types, log_filter, log_top;

/* a message's type: what's before its ':' (letters and spaces, a short way in), or "other" */
static void log_type(const char *l, char *out, int len)
{
  int n = 0;
  while (l[n] && n < 18 && ((l[n] >= 'a' && l[n] <= 'z') || l[n] == ' '))
    n++;
  if (l[n] == ':' && n > 0)
    snprintf(out, len, "%.*s", n, l);
  else
    snprintf(out, len, "other");
}

static bool log_problem(const char *l)
{
  static const char *const words[] = { "couldn't", "can't", "fail", "refused", "not found", "error", "abandoned" };
  for (unsigned w = 0; w < sizeof(words) / sizeof(*words); w++)
    for (const char *p = l; *p; p++) {
      const char *a = p, *b = words[w];
      while (*b && *a == *b)
        a++, b++;
      if (!*b)
        return true;
    }
  return false;
}

/* the types present, for the filter switch: all, problems, then each one seen */
static void log_scan_types(void)
{
  n_log_types = 0;
  for (int i = 0; i < LOG_LINES; i++) {
    const char *l = log_line(i);
    if (!l || !l[0])
      continue;
    char t[20];
    log_type(l, t, sizeof(t));
    int k = 0;
    while (k < n_log_types && !same_label(log_types[k], t))
      k++;
    if (k == n_log_types && n_log_types < LOG_TYPES)
      snprintf(log_types[n_log_types++], sizeof(log_types[0]), "%s", t);
  }
  log_type_names[0] = "all", log_type_names[1] = "problems";
  for (int k = 0; k < n_log_types; k++)
    log_type_names[k + 2] = log_types[k];
}

static bool log_matches(const char *l)
{
  if (log_filter == 0)
    return true;
  if (log_filter == 1)
    return log_problem(l);
  char t[20];
  log_type(l, t, sizeof(t));
  return log_filter - 2 < n_log_types && same_label(t, log_types[log_filter - 2]);
}

/* the n-th message (newest first) that passes the filter, or NULL; *count: how many pass */
static const char *log_nth(int n, int *count)
{
  const char *found = 0;
  int c = 0;
  for (int i = 0; i < LOG_LINES; i++) {
    const char *l = log_line(i);
    if (!l || !l[0] || !log_matches(l))
      continue;
    if (c == n)
      found = l;
    c++;
  }
  if (count)
    *count = c;
  return found;
}

static int get_log_filter(menu_item_t *it)
{
  log_scan_types();
  it->n_choices = n_log_types + 2, it->choices = log_type_names;
  if (log_filter >= it->n_choices)
    log_filter = 0;
  /* the hint: "which messages:", then what this filter shows, indented under it */
  static char hint[160];
  char what[100];
  if (log_filter == 0)
    snprintf(what, sizeof(what), "%s", T("every message, newest first (all also in sd:/sloperinaGZ/log.txt)"));
  else if (log_filter == 1)
    snprintf(what, sizeof(what), "%s", T("anything that couldn't be done"));
  else
    snprintf(what, sizeof(what), TF("messages about %s"), log_type_names[log_filter]);
  snprintf(hint, sizeof(hint), "%s\n  %s: %s", T("which messages:"), T(log_type_names[log_filter]), what);
  it->hint = hint;
  return log_filter;
}
static void set_log_filter(menu_item_t *it, int f) { log_filter = f, log_top = 0; }

static void log_arrows(menu_item_t *it, char *buf, int len)
{
  int count;
  log_nth(0, &count);
  int max = count > LOG_ROWS ? count - LOG_ROWS : 0;
  log_top = log_top > max ? max : log_top;
  int last = log_top + LOG_ROWS < count ? log_top + LOG_ROWS : count;
  snprintf(buf, len, "%c %c  %d-%d / %d", GFX_ICON_UP, GFX_ICON_DOWN, count ? log_top + 1 : 0, last, count);
}

static bool log_arrows_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = it->part ? 2 : 0, *len = 1;
  return true;
}

static void log_scroll(menu_item_t *it, int part)
{
  int count;
  log_nth(0, &count);
  int max = count > LOG_ROWS ? count - LOG_ROWS : 0;
  log_top += part ? 1 : -1;
  log_top = log_top < 0 ? 0 : log_top > max ? max : log_top;
}

static const char *const log_arrow_hints[] = { "L: scroll to newer (held: keeps going)",
                                               "L: scroll to older (held: keeps going)" };

static void text_log(menu_item_t *it, char *buf, int len)
{
  const char *l = log_nth(log_top + (int)it->data, 0);
  snprintf(buf, len, "%s", l ? l : "");
}

/* gz's start-up splash (bottom right): name and version, the build date under it, the website, how to
 * open the menu; about 8 seconds, as gz's */
void menus_splash(void)
{
  static char title[64], date[64];
  snprintf(title, sizeof(title), "sloperinaGZ - %s", BUILD_VERSION);
  snprintf(date, sizeof(date), TF("Build Date: %s"), BUILD_WHEN);
  const char *lines[] = { title, date, "practicerom.com/oothd", 0, T("R + right stick down: open the menu") };
  static const uint8_t styles[] = { GFX_DIM, GFX_DIM, GFX_LOG, GFX_LOG, GFX_NORMAL };
  static const uint8_t indents[] = { 0, 0, 0, 0, 0 };
  gfx_splash(lines, styles, indents, 5, 240);
}

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

  menu_init(&m_log, "log", LOG_ROWS + 5);
  /* the filter on the heading's line: "log history  < all >" */
  menu_item_t *lf = menu_add_option(&m_log, "log history", 1, log_type_names, get_log_filter, set_log_filter, 0);
  if (lf)
    lf->log_label = ""; /* the list shows the filter: no log message for it */
  menu_item_t *la = menu_add_action(&m_log, "", 0, 0);
  if (la)
    la->text = log_arrows, la->cursor = log_arrows_cursor, la->press = log_scroll, la->press_repeat = true,
    la->n_parts = 2, la->part_hints = log_arrow_hints;
  for (int i = 0; i < LOG_ROWS; i++) {
    menu_add_text(&m_log, text_log, i); /* grey: they can't be selected */
  }
  menu_add_return(&m_log);

  /* the about page: the name, version, build date and description, then the website */
  menu_init(&m_about, "about", 10);
  static char title[64];
  snprintf(title, sizeof(title), "sloperinaGZ - %s", BUILD_VERSION);
  menu_add_heading(&m_about, title);
  menu_add_text(&m_about, text_static, (intptr_t)BUILD_WHEN);
  menu_add_text(&m_about, text_static, (intptr_t) "gz-style practice menu, loaded by the game");
  menu_add_text(&m_about, text_static, (intptr_t) "font: Press Start 2P (codeman38, OFL)");
  menu_add_text(&m_about, text_static, (intptr_t) "based on the work of OoT N64's Practice ROM by glankk");
  menu_end_group(&m_about);
  menu_add_text(&m_about, text_static, (intptr_t) "");
  menu_add_text(&m_about, text_static, (intptr_t) "practicerom.com/oothd");
  menu_add_text(&m_about, text_static, (intptr_t) "");
  menu_add_return(&m_about);

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
