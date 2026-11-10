#include "gfx.h"

#include <stdarg.h>

#include "lang.h"
#include "publish.h"
#include "sys.h"

/* gz: 20 frames solid, then 20 fading (at the N64's 20 fps) */
#define LOG_SOLID_FRAMES GAME_FPS
#define LOG_FADE_FRAMES GAME_FPS
#define LOG_COLS 48
#define SPLASH_FADE_FRAMES 30
#define OPAQUE 255 /* a row's text opacity, fully shown */
/* a character drawn as nothing: put in a panel's corners, it gives the panel its size */
#define FILLER 0x7F
/* the panels' indices in gfx_screen_t */
#define PANEL_MENU 0
#define PANEL_HUD 1

/* drawn this frame */
static gfx_panel_t menu_panel, hud_panel, input_panel;
static bool menu_open;

typedef struct {
  char msg[LOG_COLS + 1];
  int age; /* frames shown */
} log_line_t;
static log_line_t log_lines[GFX_LOG_MAX]; /* [0] the newest */
volatile uint32_t gfx_log_count;

static gfx_screen_t published_screens[PUBLISH_BUFFERS];
static publish_t published = PUBLISH_INIT;
static const char *volatile later_notice;

volatile bool gfx_on_screen;
volatile int gfx_menu_rows = GFX_ROWS;
volatile int gfx_opacity = 60;
volatile int gfx_menu_scale;
volatile int gfx_style = GFX_STYLE_GAME;
volatile int gfx_custom_x = -1, gfx_custom_y = -1;
volatile int gfx_pause_icon;
volatile uint32_t gfx_presents;

/* Write text into a panel from (col, row), clipped to the grid. */
static void put_text(gfx_panel_t *panel, int col, int row, gfx_style_t style, const char *text)
{
  if (row < 0 || row >= GFX_ROWS)
    return;
  for (int c = col; c >= 0 && c < GFX_COLS && *text; c++, text++) {
    panel->ch[row][c] = *text;
    panel->style[row][c] = style;
  }
}

void gfx_begin(void)
{
  memset(&menu_panel, 0, sizeof(menu_panel));
  menu_open = true;
}

void gfx_text(int col, int row, gfx_style_t style, const char *s)
{
  put_text(&menu_panel, col, row, style, s);
}

void gfx_row(int row, int kind)
{
  if (row >= 0 && row < GFX_ROWS)
    menu_panel.kind[row] = (uint8_t)kind;
}

void gfx_hint(const char *s)
{
  snprintf(menu_panel.hint, sizeof(menu_panel.hint), "%s", s ? s : "");
}

void gfx_scroll(int row, int rows, int items, int top)
{
  menu_panel.scroll_row = (uint8_t)row;
  menu_panel.scroll_rows = (uint8_t)rows;
  menu_panel.scroll_items = (uint16_t)items;
  menu_panel.scroll_top = (uint16_t)top;
}

void gfx_end(void) {}

void gfx_clear(void)
{
  menu_open = false;
}

void gfx_notice(const char *s)
{
  for (int i = GFX_LOG_MAX - 1; i > 0; i--)
    log_lines[i] = log_lines[i - 1];
  snprintf(log_lines[0].msg, sizeof(log_lines[0].msg), "%s", T(s));
  log_lines[0].age = 0;
  gfx_log_count++;
  log_printf("notice: %s", s);
}

void gfx_notice_later(const char *s)
{
  __atomic_store_n(&later_notice, s, __ATOMIC_RELEASE);
}

void gfx_logf(const char *fmt, ...)
{
  char buf[LOG_COLS + 1];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), TF(fmt), va);
  va_end(va);
  gfx_notice(buf);
}

/* --- the start-up splash (gfx_splash): shown in the log's place, then faded out as the log is --- */

static struct {
  char text[LOG_COLS + 1];
  uint8_t style;
  uint8_t indent;
} splash[GFX_LOG_ROWS];
static int splash_count;
static int splash_frames; /* left to show */

void gfx_splash(const char *const *lines, const uint8_t *styles, const uint8_t *indents, int n, int frames)
{
  splash_count = n < GFX_LOG_ROWS ? n : GFX_LOG_ROWS;
  for (int i = 0; i < splash_count; i++) {
    snprintf(splash[i].text, sizeof(splash[i].text), "%s", lines[i] ? lines[i] : "");
    splash[i].style = styles[i];
    splash[i].indent = indents[i];
  }
  splash_frames = frames;
}

/* The splash into the log panel, while it's shown; false once it's gone. */
static bool build_splash(gfx_panel_t *panel)
{
  if (splash_frames <= 0)
    return false;
  splash_frames--;
  memset(panel, 0, sizeof(*panel));
  panel->corner = GFX_BOTTOM_RIGHT;
  panel->clear = true;
  panel->yoff = 1;
  unsigned alpha =
    splash_frames >= SPLASH_FADE_FRAMES ? OPAQUE : (unsigned)splash_frames * OPAQUE / SPLASH_FADE_FRAMES;
  int cols = 0;
  for (int i = 0; i < splash_count; i++) {
    int width = splash[i].indent + (int)strlen(splash[i].text);
    if (width > cols)
      cols = width;
  }
  for (int i = 0; i < splash_count; i++) {
    put_text(panel, splash[i].indent, i, (gfx_style_t)splash[i].style, splash[i].text);
    panel->alpha[i] = (uint8_t)alpha;
  }
  /* the block's size, so its right edge sits at the corner: fillers top right and bottom left */
  if (cols > 0) {
    if (!panel->ch[0][cols - 1])
      panel->ch[0][cols - 1] = FILLER;
    if (!panel->ch[splash_count - 1][0])
      panel->ch[splash_count - 1][0] = FILLER;
  }
  return true;
}

/* --- the log (gz_log) --------------------------------------------------------------------------- */

/* a log line's opacity: solid, then fading out */
static uint8_t log_line_alpha(int age)
{
  if (age <= LOG_SOLID_FRAMES)
    return OPAQUE;
  return (uint8_t)(OPAQUE - (age - LOG_SOLID_FRAMES) * OPAQUE / LOG_FADE_FRAMES);
}

/* The log panel: newest at the bottom, each line right-aligned, as gz draws it. While anything is
 * shown the panel only grows (a new size means its blend with the picture starts over, 2 frames
 * without it), and it starts small again once it's empty. */
static void build_log(gfx_panel_t *panel)
{
  static int kept_cols, kept_rows;
  if (build_splash(panel))
    return;
  memset(panel, 0, sizeof(*panel));
  panel->corner = GFX_BOTTOM_RIGHT;
  panel->clear = true;
  panel->yoff = 1; /* text in the middle of the panel (nvn.c TEXT_TOP) */
  /* age the lines, dropping those that have faded out */
  int line_count = 0;
  int cols = 0;
  for (int i = 0; i < GFX_LOG_MAX; i++) {
    log_line_t *line = &log_lines[i];
    if (!line->msg[0])
      continue;
    if (++line->age > LOG_SOLID_FRAMES + LOG_FADE_FRAMES) {
      line->msg[0] = 0;
      continue;
    }
    int len = (int)strlen(line->msg);
    if (len > cols)
      cols = len;
    line_count = i + 1;
  }
  if (!line_count) {
    kept_cols = 0;
    kept_rows = 0;
    return;
  }
  if (cols > kept_cols)
    kept_cols = cols;
  if (line_count > kept_rows)
    kept_rows = line_count;
  for (int i = 0; i < line_count; i++) {
    const log_line_t *line = &log_lines[i];
    int row = kept_rows - 1 - i;
    if (!line->msg[0])
      continue;
    put_text(panel, kept_cols - (int)strlen(line->msg), row, GFX_LOG, line->msg);
    panel->alpha[row] = log_line_alpha(line->age);
  }
  /* the panel's size: a filler in the top-left and bottom-right corners */
  if (!panel->ch[0][0])
    panel->ch[0][0] = FILLER;
  if (!panel->ch[kept_rows - 1][kept_cols - 1])
    panel->ch[kept_rows - 1][kept_cols - 1] = FILLER;
}

/* --- the input display and the watches ----------------------------------------------------------- */

void gfx_input_background(bool on)
{
  input_panel.clear = !on;
}

void gfx_input_scale(int percent)
{
  input_panel.scale_pct = (uint8_t)percent;
}

void gfx_input(int col, int row, gfx_style_t style, const char *s)
{
  input_panel.corner = GFX_BOTTOM_LEFT;
  input_panel.yoff = 1; /* text in the middle of the panel (nvn.c TEXT_TOP) */
  if (row >= 0 && row < GFX_ROWS) {
    input_panel.alpha[row] = OPAQUE;
    input_panel.kind[row] = GFX_ROW_BUTTONS;
  }
  put_text(&input_panel, col, row, style, s);
}

void gfx_input_pad(const int8_t sticks[4], uint8_t clicks, int device)
{
  input_panel.pad = 1;
  memcpy(input_panel.sticks, sticks, 4);
  input_panel.clicks = clicks;
  input_panel.device = (uint8_t)device;
}

void gfx_hud_at(int x_permille, int y_permille)
{
  hud_panel.x = (int16_t)x_permille;
  hud_panel.y = (int16_t)y_permille;
}

void gfx_hud(gfx_corner_t corner, int row, gfx_style_t style, const char *s)
{
  hud_panel.corner = corner;
  hud_panel.yoff = 1; /* text in the middle of the panel (nvn.c TEXT_TOP) */
  if (row >= 0 && row < GFX_ROWS)
    hud_panel.kind[row] = GFX_ROW_WATCH;
  put_text(&hud_panel, 0, row, style, s);
}

/* --- log fallback (until drawing works) ------------------------------------------------- */

#define MENU_LOG_INTERVAL 120 /* frames */

static uint32_t logged_menu_hash, logged_selection;
static int frames_since_logged;

/* a menu character as plain text for the log (the icons as look-alikes) */
static char plain_char(char ch)
{
  switch (ch) {
    case GFX_CHECK_OFF: return 'o';
    case GFX_CHECK_ON: return 'x';
    case GFX_ICON_REMOVE: return '-';
    case GFX_ICON_FOLDER: return '+';
    case GFX_ICON_WRENCH: return '*';
    case GFX_ICON_UP: return '^';
    case GFX_ICON_DOWN: return 'v';
    case GFX_ICON_ADD: return '+';
    case (char)GFX_ICON_LOCK: return 'L';
    case GFX_ICON_LEFT: return '<';
    case GFX_ICON_RIGHT: return '>';
    case 0:
    case GFX_PICTURE:
    case GFX_WIDE_REST: return ' ';
    default: /* a picture's number (GFX_PICTURE_ID: the pictures number under gpuui.h's GPU_IMAGES, 80) */
      return (unsigned char)ch >= 0xA0 && (unsigned char)ch < 0xA0 + 80 ? ' ' : ch;
  }
}

static void log_menu(const gfx_panel_t *menu)
{
  if (!menu) {
    if (logged_menu_hash)
      log_printf("---- menu closed ----");
    logged_menu_hash = 0;
    logged_selection = 0;
    return;
  }
  /* at once when the selection or title changes, otherwise at most every 2 s */
  uint32_t selection = 0;
  for (int r = 0; r < menu->rows; r++)
    for (int c = 0; c < menu->cols; c++)
      if (menu->style[r][c] == GFX_SELECTED || menu->style[r][c] == GFX_TITLE)
        selection = selection * 31 + (uint8_t)menu->ch[r][c] + r;
  frames_since_logged++;
  if (menu->hash == logged_menu_hash || (selection == logged_selection && frames_since_logged < MENU_LOG_INTERVAL))
    return;
  logged_menu_hash = menu->hash;
  logged_selection = selection;
  frames_since_logged = 0;
  log_printf("---- menu ----");
  for (int r = 0; r < menu->rows; r++) {
    /* "> " before the selected row, then the row without trailing spaces */
    char line[GFX_COLS + 3];
    bool selected = false;
    for (int c = 0; c < GFX_COLS; c++) {
      line[c + 2] = plain_char(menu->ch[r][c]);
      selected |= menu->style[r][c] == GFX_SELECTED;
    }
    line[0] = selected ? '>' : ' ';
    line[1] = ' ';
    int len = GFX_COLS + 2;
    while (len > 0 && line[len - 1] == ' ')
      len--;
    line[len] = 0;
    log_printf("%s", line);
  }
}

/* --- publish ------------------------------------------------------------------------------ */

/* FNV-1a */
#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME 16777619u

static inline uint32_t hash_mix(uint32_t hash, uint32_t value)
{
  return (hash ^ value) * FNV_PRIME;
}

/* Set a panel's used area (rows, cols) and the hash of everything that changes how it's drawn. */
static void measure_panel(gfx_panel_t *panel)
{
  panel->rows = 0;
  panel->cols = 0;
  uint32_t hash = FNV_OFFSET_BASIS ^ panel->corner ^ (uint32_t)panel->clear << 8; /* the shadow depends on clear */
  hash = hash_mix(hash, (uint32_t)panel->pad << 9 ^ (uint32_t)panel->clicks << 10 ^ (uint32_t)panel->device << 24 ^
                          (uint32_t)(uint8_t)panel->yoff << 12 ^ (uint32_t)panel->scale_pct << 16);
  for (int i = 0; i < 4; i++) /* the sticks are drawn from these */
    hash = hash_mix(hash, (uint8_t)panel->sticks[i]);
  for (int r = 0; r < GFX_ROWS; r++)
    for (int c = 0; c < GFX_COLS; c++) {
      if (panel->ch[r][c] && panel->ch[r][c] != ' ') {
        if (r + 1 > panel->rows)
          panel->rows = r + 1;
        if (c + 1 > panel->cols)
          panel->cols = c + 1;
      }
      hash = hash_mix(hash, (uint8_t)panel->ch[r][c] ^ (uint32_t)panel->style[r][c] << 8);
    }
  for (int r = 0; r < GFX_ROWS; r++)
    hash = hash_mix(hash, panel->kind[r]);
  for (const char *h = panel->hint; *h; h++)
    hash = hash_mix(hash, (uint8_t)*h);
  hash = hash_mix(hash, (uint32_t)panel->scroll_row << 24 ^ (uint32_t)panel->scroll_rows << 16 ^ panel->scroll_top);
  hash = hash_mix(hash, panel->scroll_items);
  panel->hash = hash ? hash : 1;
}

void gfx_frame(void)
{
  const char *notice = __atomic_exchange_n(&later_notice, 0, __ATOMIC_ACQ_REL);
  if (notice)
    gfx_notice(notice);
  int next = publish_next(&published);
  gfx_screen_t *screen = &published_screens[next];

  gfx_panel_t *menu = &screen->panel[PANEL_MENU];
  if (menu_open)
    *menu = menu_panel;
  else
    memset(menu, 0, sizeof(*menu));
  menu->corner = GFX_TOP_LEFT;
  measure_panel(menu);

  /* the watches and the input display are drawn afresh each frame */
  screen->panel[PANEL_HUD] = hud_panel;
  measure_panel(&screen->panel[PANEL_HUD]);
  memset(&hud_panel, 0, sizeof(hud_panel));

  build_log(&screen->panel[GFX_PANEL_LOG]);
  measure_panel(&screen->panel[GFX_PANEL_LOG]);
  screen->panel[GFX_PANEL_INPUT] = input_panel;
  measure_panel(&screen->panel[GFX_PANEL_INPUT]);
  memset(&input_panel, 0, sizeof(input_panel));

  bool anything = screen->panel[PANEL_MENU].rows || screen->panel[PANEL_HUD].rows ||
                  screen->panel[GFX_PANEL_LOG].rows || screen->panel[GFX_PANEL_INPUT].rows;
  publish_set(&published, anything ? next : -1);
  if (!gfx_on_screen)
    log_menu(menu_open ? menu : 0);
}

const gfx_screen_t *gfx_published(void)
{
  int index = publish_take(&published);
  return index < 0 ? 0 : &published_screens[index];
}

void gfx_release(void) { publish_release(&published); }
