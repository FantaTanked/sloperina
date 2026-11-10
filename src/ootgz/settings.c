/* Settings on the SD card, through the game's own nn::fs (exported by sdk). The file is plain
 * text, one "key=value" per line, so it can be edited by hand, for example:
 *
 *   language=<code> (lang/<code>.json; en: English)   watch_label_<watch_t>=<name> (a renamed watch)
 *   cheats=<bit per cheat_t>   watches_shown=0|1   watch_position=<gfx_corner_t>
 *   watch_list=<watch_t>:<display type>:<hidden>,...
 *   opacity=0..100             arrival_cutscenes=0|1      levitate=0|1      levitate_speed=<units/s>
 *   menu_style=0|1 (0: the game's type, 1: gz's pixel font)
 *
 * (save_settings_file writes every key; parse_line reads them.)
 *
 * The SD card is mounted with nn::fs::MountSdCard (MountSdCardForDebug if that's refused). On a
 * console this needs SD card access in the game's permissions (main.npdm); Lichii allows it. If the
 * mount fails the menu works as before, without saving. */
#include "settings.h"

#include "cheats.h"
#include "enemyhp.h"
#include "gfx.h"
#include "sys.h"
#include "warp.h"
#include "watches.h"
#include "levitate.h"
#include "airwall.h"

#include "sdfs.h"
#include "lang.h"
#include "menus.h"


#define MOUNT SD_MOUNT
#define DIR SD_DIR
#define SETTINGS_FILE DIR "/settings.ini"
#define LOG_FILE DIR "/log.txt" /* everything the module logs, from this start of the game */
#define LOG_FILE_GAP GAME_FPS   /* frames between writes */
#define OLD_SETTINGS_FILE SD_OLD_DIR "/settings.ini"
#define SAVE_GAP_FRAMES (2 * GAME_FPS) /* between settings writes */
#define SETTINGS_TEXT_MAX 2048 /* the settings file's size limit, both ways */
#define LOG_CHUNK 8192         /* the most log text written at once */

/* the ranges a loaded value must be in to be used (otherwise the current setting stays) */
#define WATCH_TYPE_MAX     15   /* a watch's display type; out of range: 0 */
#define WATCH_POS_MAX      1000 /* watch_x, watch_y: thousandths of the picture */
#define INPUT_SIZE_COUNT   3    /* small, medium, large */
#define MENU_SCALE_MAX     3
#define OPACITY_MAX        100  /* percent, kept in steps of 10 */
#define LEVITATE_SPEED_MAX 50   /* units per second */

/* input_display_parts bits */
#define INPUT_PART_LEFT    1
#define INPUT_PART_RIGHT   2
#define INPUT_PART_BUTTONS 4

#define WATCH_LABEL_KEY "watch_label_" /* + <watch_t>=<name> */

/* Everything kept in the file. Compared byte by byte to tell when to save, so it's always
 * zeroed (padding too) before being filled. */
typedef struct {
  uint32_t cheats; /* bit per cheat_t */
  int32_t n_watches;
  uint8_t watches[W_COUNT]; /* the watch list, in order */
  uint8_t watch_types[W_COUNT];
  uint8_t watch_hidden[W_COUNT];
  char watch_labels[W_COUNT][WATCH_LABEL_MAX + 1]; /* by watch_t; "" = its own name */
  char lang[8];
  int32_t frame_advance;
  int32_t pause_display;
  int32_t frame_advance_button;
  int32_t menu_scale;
  int32_t input_display;
  int32_t input_display_bg;
  int32_t input_parts; /* INPUT_PART_* */
  int32_t input_style;
  int32_t input_size;
  int32_t watches_shown;
  int32_t watch_position;
  int32_t watch_x;
  int32_t watch_y;
  int32_t opacity;
  int32_t arrival_cutscenes;
  int32_t levitate;
  int32_t levitate_speed;
  int32_t levitate_move;
  int32_t levitate_walls;
  int32_t airwalls_shown;
  int32_t airwalls_all;
  int32_t airwalls_xray;
  int32_t enemy_hp;
  int32_t enemy_hp_xray;
  int32_t menu_style;
} snapshot_t;

static bool mounted;
static snapshot_t last_saved; /* what the file holds */
static int frames_since_save;

static bool same_text(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* --- snapshots --------------------------------------------------------------------------------- */

/* The settings as they are now. */
static snapshot_t take_snapshot(void)
{
  snapshot_t s;
  memset(&s, 0, sizeof(s)); /* padding too: snapshots are compared byte by byte */
  for (int c = 0; c < CHEAT_COUNT; c++)
    s.cheats |= (uint32_t)cheats[c] << c;
  for (int i = 0; i < watch_count && s.n_watches < W_COUNT; i++) {
    if (watch_list[i].id >= W_COUNT) /* memory watches aren't kept */
      continue;
    s.watches[s.n_watches++] = (uint8_t)watch_list[i].id;
    memcpy(s.watch_labels[watch_list[i].id], watch_list[i].label, sizeof(s.watch_labels[0]));
    s.watch_hidden[watch_list[i].id] = watch_list[i].hidden;
  }
  memcpy(s.watch_types, watch_types, sizeof(s.watch_types));
  s.watches_shown = watches_shown;
  s.watch_position = watches_corner;
  s.watch_x = watches_x;
  s.watch_y = watches_y;
  s.opacity = gfx_opacity;
  s.menu_scale = gfx_menu_scale;
  s.frame_advance = frame_advance;
  s.pause_display = pause_display;
  s.frame_advance_button = frame_advance_button;
  snprintf(s.lang, sizeof(s.lang), "%s", lang_code());
  s.input_display = input_display;
  s.input_display_bg = input_display_bg;
  s.input_style = input_style;
  s.input_size = input_size;
  s.input_parts = input_show_left | input_show_right << 1 | input_show_buttons << 2;
  s.arrival_cutscenes = warp_arrival_cutscenes;
  s.levitate = levitate_enabled;
  s.levitate_speed = levitate_speed;
  s.levitate_move = levitate_move_speed;
  s.levitate_walls = levitate_through_walls;
  s.airwalls_shown = airwalls_shown;
  s.airwalls_all = airwalls_all;
  s.airwalls_xray = airwalls_xray;
  s.enemy_hp = enemy_hp_shown;
  s.enemy_hp_xray = enemy_hp_xray;
  s.menu_style = gfx_style;
  return s;
}

/* Make the settings what `s` says, skipping values out of range. */
static void apply_snapshot(const snapshot_t *s)
{
  for (int c = 0; c < CHEAT_COUNT; c++)
    cheats[c] = s->cheats >> c & 1;

  watch_t list[W_COUNT];
  int n_watches = s->n_watches;
  if (n_watches < 0)
    n_watches = 0;
  else if (n_watches > W_COUNT)
    n_watches = W_COUNT;
  for (int i = 0; i < n_watches; i++)
    list[i] = (watch_t)s->watches[i];
  watch_set_list(list, n_watches);
  for (int i = 0; i < watch_count; i++) {
    if (watch_list[i].id >= W_COUNT)
      continue;
    watch_set_label(i, s->watch_labels[watch_list[i].id]);
    watch_set_hidden(i, s->watch_hidden[watch_list[i].id] != 0);
  }
  for (int w = 0; w < W_COUNT; w++)
    watch_set_type((watch_t)w, s->watch_types[w]);
  watches_shown = s->watches_shown != 0;
  if (s->watch_position >= 0 && s->watch_position < GFX_NPLACES)
    watches_corner = s->watch_position;
  if (s->watch_x >= 0 && s->watch_x <= WATCH_POS_MAX && s->watch_y >= 0 && s->watch_y <= WATCH_POS_MAX) {
    watches_x = s->watch_x;
    watches_y = s->watch_y;
  }

  if (!same_text(s->lang, lang_code()))
    lang_select_code(s->lang);
  frame_advance = s->frame_advance != 0;
  pause_display = s->pause_display != 0;
  frame_advance_button = s->frame_advance_button == 1;

  input_display = s->input_display != 0;
  input_display_bg = s->input_display_bg != 0;
  input_style = s->input_style == 1;
  if (s->input_size >= 0 && s->input_size < INPUT_SIZE_COUNT)
    input_size = s->input_size;
  input_show_left = s->input_parts & INPUT_PART_LEFT;
  input_show_right = s->input_parts >> 1 & 1;
  input_show_buttons = s->input_parts >> 2 & 1;

  if (s->menu_scale >= 0 && s->menu_scale <= MENU_SCALE_MAX)
    gfx_menu_scale = s->menu_scale;
  if (s->opacity >= 0 && s->opacity <= OPACITY_MAX)
    gfx_opacity = s->opacity / 10 * 10;
  if (s->menu_style >= 0 && s->menu_style < GFX_STYLES)
    gfx_style = s->menu_style;
  warp_arrival_cutscenes = s->arrival_cutscenes != 0;
  levitate_enabled = s->levitate != 0;
  if (s->levitate_speed >= 0 && s->levitate_speed <= LEVITATE_SPEED_MAX)
    levitate_speed = s->levitate_speed;
  if (s->levitate_move > 0 && s->levitate_move <= LEVITATE_SPEED_MAX)
    levitate_move_speed = s->levitate_move;
  levitate_through_walls = s->levitate_walls != 0;
  airwalls_shown = s->airwalls_shown != 0;
  airwalls_all = s->airwalls_all != 0;
  airwalls_xray = s->airwalls_xray != 0;
  enemy_hp_shown = s->enemy_hp != 0;
  enemy_hp_xray = s->enemy_hp_xray != 0;
}

/* --- reading the file ------------------------------------------------------------------------- */

/* A decimal integer, with an optional '-'; stops at the first non-digit. */
static int64_t parse_int(const char *p)
{
  int64_t value = 0;
  bool negative = *p == '-';
  if (negative)
    p++;
  for (; *p >= '0' && *p <= '9'; p++)
    if (value < INT64_MAX / 100) /* a longer number stops growing */
      value = value * 10 + (*p - '0');
  return negative ? -value : value;
}

static const char *skip_digits(const char *p)
{
  while (*p >= '0' && *p <= '9')
    p++;
  return p;
}

static bool starts_with(const char *line, const char *prefix)
{
  while (*prefix && *line == *prefix) {
    line++;
    prefix++;
  }
  return !*prefix;
}

/* True if the line is "<key>=...", with *value pointing after the '='. */
static bool key_is(const char *line, const char *key, const char **value)
{
  while (*key && *line == *key) {
    line++;
    key++;
  }
  if (*key || *line != '=')
    return false;
  *value = line + 1;
  return true;
}

/* The keys whose value is a plain integer, and the snapshot field each one sets. */
#define INT_KEY(key, field) { key, offsetof(snapshot_t, field) }
static const struct { const char *key; size_t field_offset; } int_keys[] = {
  INT_KEY("watches_shown", watches_shown),
  INT_KEY("watch_position", watch_position),
  INT_KEY("watch_x", watch_x),
  INT_KEY("watch_y", watch_y),
  INT_KEY("opacity", opacity),
  INT_KEY("menu_size", menu_scale),
  INT_KEY("frame_advance", frame_advance),
  INT_KEY("pause_display", pause_display),
  INT_KEY("frame_advance_button", frame_advance_button),
  INT_KEY("input_display", input_display),
  INT_KEY("input_display_background", input_display_bg),
  INT_KEY("input_display_style", input_style),
  INT_KEY("input_display_size", input_size),
  INT_KEY("input_display_parts", input_parts),
  INT_KEY("arrival_cutscenes", arrival_cutscenes),
  INT_KEY("levitate", levitate),
  INT_KEY("levitate_speed", levitate_speed),
  INT_KEY("levitate_move_speed", levitate_move),
  INT_KEY("levitate_through_walls", levitate_walls),
  INT_KEY("airwalls_shown", airwalls_shown),
  INT_KEY("airwalls_all", airwalls_all),
  INT_KEY("airwalls_xray", airwalls_xray),
  INT_KEY("enemy_health", enemy_hp),
  INT_KEY("enemy_health_xray", enemy_hp_xray),
  INT_KEY("menu_style", menu_style),
};
#define N_INT_KEYS (sizeof(int_keys) / sizeof(*int_keys))

/* "watches=<bits>": the old format, before watch_list: a bit per watch_t, in watch_t order. */
static void parse_watch_bits(const char *value, snapshot_t *s)
{
  uint32_t bits = (uint32_t)parse_int(value);
  s->n_watches = 0;
  for (int w = 0; w < W_COUNT; w++)
    if (bits >> w & 1)
      s->watches[s->n_watches++] = (uint8_t)w;
}

/* "watch_list=<watch_t>[:<display type>[:<hidden>]],..." */
static void parse_watch_list(const char *value, snapshot_t *s)
{
  const char *p = value;
  s->n_watches = 0;
  while (*p >= '0' && *p <= '9' && s->n_watches < W_COUNT) {
    int64_t watch = parse_int(p);
    p = skip_digits(p);
    int64_t type = 0;
    int64_t hidden = 0;
    if (*p == ':') { /* its display type */
      type = parse_int(++p);
      p = skip_digits(p);
    }
    if (*p == ':') { /* 1: hidden */
      hidden = parse_int(++p);
      p = skip_digits(p);
    }
    if (watch < W_COUNT) {
      s->watches[s->n_watches++] = (uint8_t)watch;
      s->watch_types[watch] = (uint8_t)(type < 0 || type > WATCH_TYPE_MAX ? 0 : type);
      s->watch_hidden[watch] = hidden != 0;
    }
    if (*p == ',')
      p++;
  }
}

/* "watch_label_<watch_t>=<name>"; `p` is just after the prefix */
static void parse_watch_label(const char *p, snapshot_t *s)
{
  int64_t watch = parse_int(p);
  p = skip_digits(p);
  if (*p == '=' && watch >= 0 && watch < W_COUNT)
    snprintf(s->watch_labels[watch], sizeof(s->watch_labels[watch]), "%s", p + 1);
}

/* One line of the file into `s`; unknown keys are ignored. *have_list is set once a watch_list
 * line is seen, after which the old "watches" key is ignored. */
static void parse_line(const char *line, snapshot_t *s, bool *have_list)
{
  const char *value;
  if (key_is(line, "cheats", &value)) {
    s->cheats = (uint32_t)parse_int(value);
  } else if (key_is(line, "watches", &value)) {
    if (!*have_list)
      parse_watch_bits(value, s);
  } else if (key_is(line, "watch_list", &value)) {
    *have_list = true;
    parse_watch_list(value, s);
  } else if (key_is(line, "language", &value)) {
    snprintf(s->lang, sizeof(s->lang), "%s", value);
  } else if (starts_with(line, WATCH_LABEL_KEY)) {
    parse_watch_label(line + sizeof(WATCH_LABEL_KEY) - 1, s);
  } else {
    for (size_t k = 0; k < N_INT_KEYS; k++) {
      if (key_is(line, int_keys[k].key, &value)) {
        *(int32_t *)((char *)s + int_keys[k].field_offset) = (int32_t)parse_int(value);
        break;
      }
    }
  }
}

/* Reads the settings file (or the one in the old folder) into buf, null-terminated. Returns the
 * bytes read; *from is the file read (0 if none), *old whether it was the old folder's. */
static size_t read_settings_text(char *buf, size_t buf_size, const char **from, bool *old)
{
  fs_file_t file;
  size_t got = 0;
  *from = 0;
  *old = false;
  if (fs_open(&file, SETTINGS_FILE, FS_READ) == 0) {
    *from = SETTINGS_FILE;
  } else if (fs_open(&file, OLD_SETTINGS_FILE, FS_READ) == 0) {
    *from = OLD_SETTINGS_FILE;
    *old = true;
  }
  if (*from) {
    int64_t size = 0;
    fs_size(&size, file);
    if (size < 0)
      size = 0;
    if (size > (int64_t)buf_size - 1)
      size = buf_size - 1;
    fs_read(&got, file, 0, buf, (size_t)size);
    fs_close(file);
  }
  buf[got] = 0;
  return got;
}

void settings_load(void)
{
  if (fs_mount_sd(MOUNT) != 0 && fs_mount_sd_debug(MOUNT) != 0) {
    log_printf("settings: no SD card access; settings won't be kept");
    gfx_notice("sloperinaGZ: no SD card access, settings won't be kept");
    return;
  }
  mounted = true;
  fs_create_dir(DIR);
  fs_delete_file(LOG_FILE); /* a fresh one each time the game starts */
  fs_create_file(LOG_FILE, 0);
  lang_init(); /* the language files on the SD card, for the language setting */

  snapshot_t s = take_snapshot(); /* defaults for anything the file lacks */
  s.menu_style = -1;               /* ... except the style: chosen below */
  bool have_list = false;
  static char text[SETTINGS_TEXT_MAX];
  const char *from;
  bool old; /* saved before the rename: saved again in the new folder */
  size_t got = read_settings_text(text, sizeof(text), &from, &old);

  /* line by line, cutting each line off in place */
  for (char *line = text; *line;) {
    char *end = line;
    while (*end && *end != '\n' && *end != '\r')
      end++;
    char line_break = *end;
    *end = 0;
    parse_line(line, &s, &have_list);
    line = line_break ? end + 1 : end;
  }
  /* a file from before the game's style: the game's, as for everyone */
  if (s.menu_style < 0)
    s.menu_style = GFX_STYLE_GAME;

  apply_snapshot(&s);
  last_saved = take_snapshot();
  if (got)
    log_printf("settings: loaded %s", from);
  else
    log_printf("settings: none yet (" SETTINGS_FILE ")");
  if (old)
    last_saved.cheats ^= 1; /* differs from now: written to the new folder at the next chance */
}

/* --- writing the file ------------------------------------------------------------------------- */

/* "<watch_t>:<display type>:<hidden>,..." for watch_list= */
static void format_watch_list(const snapshot_t *s, char *out, size_t out_size)
{
  for (int i = 0, len = 0; i < s->n_watches && i < W_COUNT; i++) {
    int watch = s->watches[i];
    len += snprintf(out + len, out_size - len, "%s%d:%d:%d", i ? "," : "", watch,
                    s->watch_types[watch % W_COUNT], s->watch_hidden[watch % W_COUNT]);
  }
}

static void save_settings_file(const snapshot_t *s)
{
  char list[W_COUNT * 9 + 1] = ""; /* up to 9 characters per watch ("nn:nn:n,") */
  format_watch_list(s, list, sizeof(list));
  static char buf[SETTINGS_TEXT_MAX];
  int n = snprintf(buf, sizeof(buf),
                   "cheats=%u\nwatch_list=%s\nwatches_shown=%d\nwatch_position=%d\nwatch_x=%d\nwatch_y=%d\nopacity=%d\n"
                   "arrival_cutscenes=%d\nlevitate=%d\nlevitate_speed=%d\nlevitate_move_speed=%d\n"
                   "levitate_through_walls=%d\nairwalls_shown=%d\nairwalls_all=%d\nenemy_health=%d\n",
                   (unsigned)s->cheats, list, (int)s->watches_shown, (int)s->watch_position,
                   (int)s->watch_x, (int)s->watch_y,
                   (int)s->opacity, (int)s->arrival_cutscenes, (int)s->levitate, (int)s->levitate_speed,
                   (int)s->levitate_move, (int)s->levitate_walls, (int)s->airwalls_shown, (int)s->airwalls_all,
                   (int)s->enemy_hp);
  if (n < (int)sizeof(buf))
    n += snprintf(buf + n, sizeof(buf) - n,
                  "language=%s\nframe_advance=%d\nframe_advance_button=%d\npause_display=%d\nmenu_size=%d\n"
                  "input_display=%d\ninput_display_background=%d\ninput_display_parts=%d\n"
                  "input_display_style=%d\ninput_display_size=%d\nmenu_style=%d\n",
                  s->lang, (int)s->frame_advance, (int)s->frame_advance_button, (int)s->pause_display,
                  (int)s->menu_scale, (int)s->input_display, (int)s->input_display_bg, (int)s->input_parts,
                  (int)s->input_style, (int)s->input_size, (int)s->menu_style);
  for (int w = 0; w < W_COUNT && n < (int)sizeof(buf); w++)
    if (s->watch_labels[w][0])
      n += snprintf(buf + n, sizeof(buf) - n, WATCH_LABEL_KEY "%d=%s\n", w, s->watch_labels[w]);
  if (n > (int)sizeof(buf) - 1)
    n = (int)sizeof(buf) - 1;

  fs_create_dir(DIR); /* fails harmlessly if it exists */
  fs_delete_file(SETTINGS_FILE);
  fs_file_t file;
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  if (fs_create_file(SETTINGS_FILE, n) != 0 || fs_open(&file, SETTINGS_FILE, FS_WRITE) != 0) {
    log_printf("settings: couldn't write " SETTINGS_FILE);
    return;
  }
  nn_result_t result = fs_write(file, 0, buf, (size_t)n, &flush);
  fs_close(file);
  log_printf(result == 0 ? "settings: saved" : "settings: write failed");
}

/* (memcmp isn't one of the functions we import) */
static bool same_bytes(const void *a, const void *b, size_t n)
{
  const uint8_t *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return false;
  return true;
}

/* --- per frame ---------------------------------------------------------------------------------- */

/* appends what's been logged to the log file, about once a second (the game's thread) */
void settings_log_frame(void)
{
  static int frames_since_write;
  static char chunk[LOG_CHUNK];
  if (!mounted || ++frames_since_write < LOG_FILE_GAP)
    return;
  frames_since_write = 0;
  size_t len = log_take_pending(chunk, sizeof(chunk));
  if (!len)
    return;
  fs_file_t file;
  if (fs_open(&file, LOG_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0) {
    fs_create_file(LOG_FILE, 0);
    if (fs_open(&file, LOG_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0)
      return;
  }
  int64_t size = 0;
  fs_size(&size, file);
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  fs_write(file, size, chunk, len, &flush);
  fs_close(file);
}

void settings_frame(bool menu_open)
{
  if (!mounted)
    return;
  frames_since_save++;
  if (menu_open || frames_since_save < SAVE_GAP_FRAMES)
    return;
  snapshot_t now = take_snapshot();
  if (!same_bytes(&now, &last_saved, sizeof(now))) {
    save_settings_file(&now);
    last_saved = now;
    frames_since_save = 0;
  }
}
