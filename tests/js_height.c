/* Host regression tests: real watches/settings/HUD, mocked game memory and SD I/O. */
#include <assert.h>
#include "menu.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "watches.h"
#include "settings.h"
#include "sys.h"
#include "sdfs.h"
#include "link.h"
#include "gfx.h"
#include "font.h"
#include "cheats.h"
#include "z64hd.h"

_Static_assert(sizeof(uintptr_t) == 8 && sizeof(float) == 4, "requires a 64-bit host with f32");
_Static_assert(W_X == 0 && W_AIRWALL == 16 && W_JS_HEIGHT == 17, "keep saved watch IDs stable");

uintptr_t z64hd_main;
static const struct z64hd_build test_build; /* no build: the version-specific paths stay off */
const struct z64hd_build *z64hd_game = &test_build;
bool enemy_hp_shown;
bool cheats[CHEAT_COUNT], frame_advance, pause_display, warp_arrival_cutscenes;
bool levitate_enabled, levitate_through_walls, airwalls_shown, airwalls_all;
int frame_advance_button, levitate_speed = 5, levitate_move_speed = 5;

static uintptr_t actor, first, player, base;
static uint32_t value;
static int reads, unavailable;
static char settings[2048];
static bool sd_available = true;
static float *live_target;
static bool writable;

/* Synthetic addresses are deliberately above 4 GiB. Only the expected slots are readable. */
uintptr_t link_actor(void) { return actor; }
bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm)
{
  assert(perm == SYS_PERM_R || perm == (SYS_PERM_R | SYS_PERM_W));
  if (size == 8)
    return (addr == base + 0x270 && unavailable != 1) ||
           (addr == base + 0x1000 + 0x358 && unavailable != 2);
  if (live_target)
    return size == 4 && addr == (uintptr_t)live_target && unavailable != 3 &&
           (!(perm & SYS_PERM_W) || writable);
  if (perm & SYS_PERM_W)
    return false;
  return size == 4 && addr == base + 0x2000 + 0x18E8 && unavailable != 3;
}
uintptr_t sys_ptr(uintptr_t addr)
{
  reads++;
  if ((addr & 7) || !sys_mapped(addr, 8, SYS_PERM_R))
    return 0;
  return addr == base + 0x270 ? first : player;
}
bool sys_read32(uintptr_t addr, uint32_t *out)
{
  reads++;
  if ((addr & 3) || !sys_mapped(addr, 4, SYS_PERM_R))
    return false;
  if (live_target)
    memcpy(out, live_target, 4);
  else
    *out = value;
  return true;
}
static void chain(uintptr_t b, float f)
{
  base = b, actor = b, first = b + 0x1000, player = b + 0x2000;
  memcpy(&value, &f, 4);
  unavailable = reads = 0;
  live_target = 0;
  writable = false;
}
static void expect(const char *s, int nreads)
{
  char text[48];
  reads = 0;
  watch_text(W_JS_HEIGHT, text, sizeof(text));
  /* The Windows CRT uses three exponent digits; Horizon/libc may use two. */
  char *exp = strstr(text, "e+0");
  if (exp) memmove(exp + 2, exp + 3, strlen(exp + 3) + 1);
  if (strcmp(text, s)) fprintf(stderr, "expected '%s', got '%s'\n", s, text);
  assert(strcmp(text, s) == 0);
  assert(reads == nreads);
}

bool link_drawn_pos(vec3f_t *p) { *p = (vec3f_t){ 1, 2, 3 }; return true; }
uintptr_t camera(void) { return 0; }
uint32_t frame_counter(void) { return 0; }
uint64_t sys_tick(void) { return 1; }
uint64_t sys_tick_freq(void) { return 1000; }
uint32_t input_held(void) { return 0; }
uint32_t input_pressed(void) { return 0; }
bool input_repeat(uint32_t button) { (void)button; return false; }
bool input_block_game;
bool input_combo(uint32_t mask) { (void)mask; return false; }
void input_sticks(int32_t *a, int32_t *b, int32_t *c, int32_t *d) { *a = *b = *c = *d = 0; }
const char *gd_get_str(int field) { (void)field; return "test"; }
void airwall_link_text(char *buf, int len) { snprintf(buf, len, "outside"); }
const char *T(const char *s) { return s; }
const char *TF(const char *s) { return s; }
void lang_init(void) {}
const char *lang_code(void) { return "en"; }
bool lang_select_code(const char *s) { (void)s; return true; }
void log_printf(const char *fmt, ...) { (void)fmt; }
size_t log_take_pending(char *out, size_t n) { (void)out; (void)n; return 0; }

/* Only settings.ini is kept, in memory; never opens the user's real files. */
nn_result_t fs_mount_sd(const char *s) { (void)s; return !sd_available; }
nn_result_t fs_mount_sd_debug(const char *s) { return fs_mount_sd(s); }
nn_result_t fs_create_dir(const char *s) { (void)s; return 0; }
nn_result_t fs_create_file(const char *s, int64_t n) { (void)s; (void)n; return 0; }
nn_result_t fs_delete_file(const char *s) { (void)s; return 0; }
nn_result_t fs_open(fs_file_t *out, const char *s, int mode)
{
  if (strcmp(s, SD_DIR "/settings.ini") || (mode == FS_READ && !settings[0]))
    return 1;
  out->handle = settings;
  return 0;
}
nn_result_t fs_size(int64_t *out, fs_file_t f) { *out = (int64_t)strlen(f.handle); return 0; }
nn_result_t fs_read(size_t *got, fs_file_t f, int64_t offset, void *out, size_t n)
{
  assert(offset == 0 && n <= strlen(f.handle));
  memcpy(out, f.handle, n);
  *got = n;
  return 0;
}
nn_result_t fs_write(fs_file_t f, int64_t offset, const void *in, size_t n, const fs_write_option_t *opt)
{
  (void)opt;
  assert(f.handle == settings && offset == 0 && n < sizeof(settings));
  memcpy(settings, in, n);
  settings[n] = 0;
  return 0;
}
void fs_close(fs_file_t f) { (void)f; }

static void test_values(void)
{
  chain(UINT64_C(0x2300000000), FLT_MAX);
  expect("Max", 3);
  chain(base, 0x1.fffffcp127f); /* next f32 below FLT_MAX is still numeric */
  expect("3.40282326e+38", 3);
  chain(base, 100.7f);
  expect("100.699997", 3);
  chain(base, 100.7001f);
  expect("100.700104", 3);
  chain(UINT64_C(0x2400000000), -12.125f); /* relocated actor and both pointers */
  expect("-12.125", 3);
  chain(base, 0.0f);
  expect("0", 3);

  for (int i = 0; i < 3; i++) {
    chain(base, 1.0f);
    uintptr_t *p = i == 0 ? &actor : i == 1 ? &first : &player;
    *p = 0;
    expect("N/A", i);
    *p = UINTPTR_MAX - 7; /* addition overflow */
    expect("N/A", i);
    *p = 0xDEAD0000; /* unmapped */
    expect("N/A", i + 1);
    *p = base + (uintptr_t)i * 0x1000 + 1; /* misaligned */
    expect("N/A", i + 1);
  }
  for (int i = 1; i <= 3; i++) {
    chain(base, 1.0f);
    unavailable = i; /* pointer slot or final f32 temporarily unreadable */
    expect("N/A", i);
    unavailable = 0;
    expect("1", 3); /* recovers on the very next read */
  }
  chain(base, FLT_MAX);
  watch_set_type(W_JS_HEIGHT, 1);
  expect("Max", 3);
  char small[4] = { 'x', 'x', 'x', 'x' };
  watch_text(W_JS_HEIGHT, small, 3);
  assert(small[2] == 0 && small[3] == 'x');
  puts("PASS: f32 precision, FLT_MAX, relocation, null/unmapped/unaligned/overflow, recovery");
}

static void test_settings(void)
{
  snprintf(settings, sizeof(settings), "watch_list=16:0:1,2:2:0,0:1:1\nwatch_label_2=east\n"
           "watches_shown=0\nwatch_position=3\n");
  settings_load();
  assert(watch_count == 4 && watch_list[3].id == W_JS_HEIGHT && !watch_list[3].hidden);
  assert(watch_list[0].id == W_AIRWALL && watch_list[0].hidden);
  assert(watch_list[1].id == W_Z && !strcmp(watch_list[1].label, "east") && watch_types[W_Z] == 2);
  assert(watch_list[2].id == W_X && watch_list[2].hidden && watch_types[W_X] == 1);
  assert(!watches_shown && watches_corner == GFX_BOTTOM_RIGHT);
  watch_set_hidden(3, true);
  watch_set_label(3, "custom JS");
  watch_move(3, -1);
  for (int i = 0; i < 60; i++) settings_frame(false);
  assert(strstr(settings, "17:0:1") && strstr(settings, "watch_label_17=custom JS"));
  watch_set_list(NULL, 0);
  settings_load();
  assert(watch_count == 4 && watch_list[2].id == W_JS_HEIGHT && watch_list[2].hidden);
  assert(!strcmp(watch_list[2].label, "custom JS"));
  assert(!watch_add(W_JS_HEIGHT));
  watch_remove(2);
  for (int i = 0; i < 60; i++) settings_frame(false);
  settings_load();
  assert(watch_count == 4 && watch_list[3].id == W_JS_HEIGHT); /* permanent after restart */
  assert(!watch_list[3].hidden);

  snprintf(settings, sizeof(settings), "watches=5\n"); /* legacy bitmask */
  settings_load();
  assert(watch_count == 3 && watch_list[0].id == W_X && watch_list[1].id == W_Z);
  assert(watch_list[2].id == W_JS_HEIGHT);
  snprintf(settings, sizeof(settings), "watch_list=\n");
  settings_load();
  assert(watch_count == 1 && watch_list[0].id == W_JS_HEIGHT);
  assert(watch_add_memory(0x10000, MW_F32) && watch_list[1].id == W_MEMORY);
  assert(!watch_add_memory(0x10000, MW_X32));
  snprintf(settings, sizeof(settings), "levitate_speed=0\n");
  levitate_speed = 4;
  settings_load();
  assert(levitate_speed == 0);
  watch_set_label(0, "save hover"); /* trigger settings save */
  for (int i = 0; i < 60; i++) settings_frame(false);
  assert(strstr(settings, "levitate_speed=0\n"));
  levitate_speed = 4;
  settings_load();
  assert(levitate_speed == 0);
  snprintf(settings, sizeof(settings), "levitate_speed=-1\n");
  settings_load();
  assert(levitate_speed == 0); /* invalid negative speed is ignored */
  snprintf(settings, sizeof(settings), "levitate_speed=4\n");
  settings_load();
  assert(levitate_speed == 4); /* existing positive settings still work */
  sd_available = false;
  settings_load();
  assert(watch_has(W_JS_HEIGHT));
  puts("PASS: old/current settings, IDs, order, labels, visibility, restart, memory watches, no SD");
}

static void render_panel(const gfx_panel_t *p, const char *image)
{
  if (image) { /* rasterise the real HUD text with the project's font, headlessly */
    FILE *f = fopen(image, "wb");
    assert(f);
    int w = (p->cols + 2) * 8, h = (p->rows + 2) * 8;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        unsigned char ch = x >= 8 && x < w - 8 ? (unsigned char)p->ch[y >= 8 && y / 8 - 1 < p->rows ? y / 8 - 1 : 0][x / 8 - 1] : 0;
        bool on = y >= 8 && y < (p->rows + 1) * 8 && ch >= FONT_FIRST && ch < FONT_FIRST + FONT_COUNT &&
                  (font_8x8[ch - FONT_FIRST][y % 8] & (0x80 >> (x % 8)));
        unsigned char rgb[3] = { on ? 255 : 24, on ? 255 : 24, on ? 255 : 24 };
        fwrite(rgb, 1, 3, f);
      }
    fclose(f);
  }
}

static void test_hud(const char *image)
{
  watch_set_list(NULL, 0);
  watches_shown = true;
  chain(base, FLT_MAX);
  watches_frame(false);
  gfx_frame();
  const gfx_screen_t *s = gfx_published();
  assert(s && s->panel[1].rows == 1);
  const gfx_panel_t *p = &s->panel[1];
  assert(!strncmp(p->ch[0], "js height    Max", 16));
  js_height_lock = true;
  watches_frame(false);
  gfx_frame();
  p = &gfx_published()->panel[1];
  assert(!strncmp(p->ch[0], "js height \x80  Max", 16));
  char label[24];
  watch_entry_label(&watch_list[0], label, sizeof(label));
  assert(!strcmp(label, "js height")); /* status must not enter the rename field */
  watch_set_label(0, "abcdefghijkl");
  watch_entry_display_label(&watch_list[0], label, sizeof(label));
  assert(!strcmp(label, "abcdefghij \x80"));
  assert(!strcmp(watch_list[0].label, "abcdefghijkl"));
  watch_set_label(0, "");
  render_panel(p, image);
  js_height_lock = false;
  watches_frame(false);
  gfx_frame();
  assert(!strncmp(gfx_published()->panel[1].ch[0], "js height    Max", 16));
  watch_set_hidden(0, true);
  watches_frame(false);
  gfx_frame();
  s = gfx_published();
  assert(!s || !s->panel[1].rows);
  watch_set_hidden(0, false);
  watches_shown = false;
  watches_frame(false);
  gfx_frame();
  s = gfx_published();
  assert(!s || !s->panel[1].rows);
  puts("PASS: real HUD label/value rendering and individual/global visibility");
}

static void test_menu_filler(const char *image)
{
  /* Exercise real menu layout: its invisible sizing cells must remain blank. */
  menu_item_t items[] = {
    { .kind = MI_ACTION, .label = "watches" },
    { .kind = MI_ACTION, .label = "settings", .hint = "A longer hint reserves room around the menu" },
  };
  menu_t m = { .title = "sloperinaGZ", .items = items, .n_items = 2 };
  menu_current = &m;
  menu_draw();
  gfx_frame();
  const gfx_panel_t *p = &gfx_published()->panel[0];
  int fillers = 0;
  for (int r = 0; r < p->rows; r++)
    for (int c = 0; c < p->cols; c++) {
      unsigned char ch = (unsigned char)p->ch[r][c];
      assert(ch != (unsigned char)GFX_ICON_LOCK);
      if (ch == 0x7F) {
        fillers++;
        for (int y = 0; y < FONT_H; y++)
          assert(font_8x8[ch - FONT_FIRST][y] == 0);
      }
    }
  assert(fillers > 0);
  render_panel(p, image);
  menu_current = NULL;
  gfx_clear();
  puts("PASS: real menu sizing fillers remain invisible; no unintended lock icons");
}

static void test_reset_lock(void)
{
  assert(!js_height_lock); /* off by default */
  float live[3] = { 123.0f, 42.5f, -123.0f };
  float next[3] = { 321.0f, 11.5f, -321.0f };
  chain(UINT64_C(0x2500000000), 0);
  live_target = &live[1];
  player = (uintptr_t)live_target - 0x18E8;
  writable = true;
  expect("42.5", 3);
  assert(live[1] == 42.5f); /* viewing a watch never writes */
  assert(js_height_reset());
  assert(live[0] == 123.0f && live[1] == FLT_MAX && live[2] == -123.0f);
  live[1] = 43.2f; /* the game changes it after a one-shot reset */
  watches_frame(false);
  assert(live[1] == 43.2f);

  js_height_lock = true;
  watches_shown = false;
  watches_frame(false);
  assert(live[1] == FLT_MAX); /* enforced with HUD hidden */
  live[1] = 44.0f;
  watches_frame(true);
  assert(live[1] == FLT_MAX); /* enforced with the menu open */
  watch_set_hidden(0, true);
  live[1] = 45.0f;
  watches_frame(false);
  assert(live[1] == FLT_MAX); /* enforced with the individual watch hidden */
  watch_remove(0);
  live[1] = 46.0f;
  watches_frame(false);
  assert(live[1] == FLT_MAX); /* removing the readout does not silently unlock it */

  live[1] = 47.0f;
  writable = false;
  assert(!js_height_reset());
  watches_frame(false);
  assert(live[1] == 47.0f); /* read-only mappings are never written */
  writable = true;
  for (int i = 1; i <= 3; i++) {
    unavailable = i;
    assert(!js_height_reset());
    watches_frame(false);
    assert(live[1] == 47.0f);
  }
  unavailable = 0;
  uintptr_t saved_player = player;
  player = 0;
  assert(!js_height_reset());
  player = UINTPTR_MAX - 7;
  assert(!js_height_reset());
  player = saved_player + 1;
  assert(!js_height_reset());
  assert(live[1] == 47.0f);

  /* Replace the final object: only the newly resolved target may be written. */
  live_target = &next[1];
  player = (uintptr_t)live_target - 0x18E8;
  watches_frame(false);
  assert(next[1] == FLT_MAX && live[1] == 47.0f);
  assert(live[0] == 123.0f && live[2] == -123.0f && next[0] == 321.0f && next[2] == -321.0f);
  js_height_lock = false;
  next[1] = 12.2f;
  watches_frame(false);
  assert(next[1] == 12.2f); /* disabling stops all writes */
  puts("PASS: reset/lock, exact four-byte writes, read-only/invalid targets, relocation, hidden HUD, unlock");
}

int main(int argc, char **argv)
{
  assert(watches_shown && watch_count == 7 && watch_list[6].id == W_JS_HEIGHT && !watch_list[6].hidden);
  int n, current;
  const char *const *types = watch_entry_types(&watch_list[6], &n, &current);
  assert(n == 1 && current == 0 && !strcmp(types[0], "f32"));
  assert(watch_entry_has_address(&watch_list[6])); /* the wrench (debug > memory), added with save/load position */
  test_values();
  test_settings();
  test_hud(argc > 1 ? argv[1] : NULL);
  test_menu_filler(argc > 2 ? argv[2] : NULL);
  test_reset_lock();
  return 0;
}
