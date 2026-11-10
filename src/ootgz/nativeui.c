/* Panels in the game's type (see nativeui.h).
 *
 * A panel is gz's grid of cells: a cell is 8 x 10 font pixels, the text starts one cell in and half a
 * glyph down (nvn.c's panel_pixel), and the whole is scaled by scale / 16. Here each text row is drawn
 * at screen resolution into a strip (RGB), then written into the panel's buffer in the picture's
 * format:
 *   - a run of text (cells up to a style change, an icon or two spaces) is set in Clarimo from its first
 *     cell, word after word; a number (a word with digits) that forms a column with numbers in other
 *     rows is set on gz's grid instead: from its cell when they start in the same cell (values after a
 *     label and one space), to its last cell when they only end in the same one (right-aligned);
 *   - Clarimo's ascent and descent fill most of a row (TEXT_FILL), so nothing reaches into the next;
 *   - the selected entry (GFX_SELECTED) is DemiBold on a light band that fades out at both ends, as the
 *     game marks its own menus' choice; titles are DemiBold in the game's gold;
 *   - checkboxes are drawn (a rounded box, filled with a tick when on), arrows and + come from the
 *     button glyph font, and what has no native form yet comes from gz's pixel art (legacy). */
#include "nativeui.h"

#include <stddef.h>

#include "font.h"
#include "gamefont.h"
#include "gameimage.h"
#include "gpuui.h"
#include "lang.h"
#include "sys.h"

void *memset(void *dst, int c, size_t n);

#define CELL_W FONT_W   /* 8 font pixels */
#define CELL_HT (FONT_H + 2) /* 10: nvn.c's CELL_H */
#define TEXT_TOP_FP (FONT_H / 2)
#define TEXT_FILL 0.9f  /* of a row's height: the font's ascent plus descent */
#define GLYPH_EM 1.75f  /* the button glyphs' em, against the text's (they're drawn small in their em) */
#define MAX_W 1600      /* nvn.c's widest panel: PANEL_W(GFX_COLS, 3x) is 1584 */
#define MAX_STRIP_H 32  /* a text row at 3x is 30 pixels */

/* the pixel formats (nvn.c's FMT_) */
enum { F_RGBA8 = 37, F_RGBA16F = 41, F_RGBX8_SRGB = 55, F_RGBA8_SRGB = 56, F_RGB10A2 = 61 };

typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t background = { 0x12, 0x10, 0x0E };
static const rgb_t band_light = { 0xFF, 0xF4, 0xDC }; /* the selection band and its hairlines */
static const struct { rgb_t colour; gf_face_t face; } styles[GFX_NSTYLES] = {
  [GFX_NORMAL] = { { 0xEC, 0xE8, 0xDE }, GF_REGULAR },
  [GFX_SELECTED] = { { 0xFF, 0xFF, 0xFF }, GF_BOLD },
  [GFX_TITLE] = { { 0xF5, 0xD6, 0x85 }, GF_BOLD },
  [GFX_DIM] = { { 0x96, 0x91, 0x87 }, GF_REGULAR },
  [GFX_NOTICE] = { { 0xFF, 0xE0, 0x90 }, GF_REGULAR },
  [GFX_LOG] = { { 0xCD, 0xC8, 0xBE }, GF_REGULAR },
  [GFX_FAINT] = { { 0x5A, 0x57, 0x52 }, GF_REGULAR },
};

static rgb_t strip[MAX_STRIP_H][MAX_W];
static int strip_w, strip_h;
/* the GPU path (nativeui_gpu_panel): the primitives below become quads (gpuui.c), at the strip's place in
 * the visible area, instead of pixels of the strip */
static bool gpu;
static float gpu_x, gpu_y;
static float gpu_fade = 1; /* the row's opacity: a panel with no background fades its rows (the log) */
static void gpu_checkbox(float x, float y, int size, bool on, rgb_t colour);
static void centred_glyph(uint32_t cp, int gem, float cx, float cy, gpu_colour_t colour);
static void gpu_icon(nui_legacy_fn legacy, unsigned char c, int cells, float x, float y, int w, int h, rgb_t colour);

static gpu_colour_t to_gpu(rgb_t c, float alpha)
{
  return (gpu_colour_t){ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha * gpu_fade };
}

bool nativeui_ready(void)
{
  return gamefont_ready();
}

/* --- pixels ---------------------------------------------------------------------------------- */

static inline uint8_t mix8(uint8_t under, uint8_t over, unsigned a)
{
  return (uint8_t)((under * (255 - a) + over * a + 127) / 255);
}

static inline void blend(int x, int y, rgb_t c, unsigned a)
{
  if (x < 0 || y < 0 || x >= strip_w || y >= strip_h || !a)
    return;
  rgb_t *p = &strip[y][x];
  p->r = mix8(p->r, c.r, a);
  p->g = mix8(p->g, c.g, a);
  p->b = mix8(p->b, c.b, a);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a square root by the CPU's own instruction: the compiler's falls back to sqrtf (for errno), which the
 * module doesn't import */
static inline float root(float v)
{
#if defined(__aarch64__)
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(v > 0 ? v : 0));
  return r;
#else
  return __builtin_sqrtf(v > 0 ? v : 0);
#endif
}

/* value / 255 as a half float (0..1) */
static uint16_t half_of(unsigned value)
{
  if (!value)
    return 0;
  float f = value / 255.0f;
  uint32_t bits;
  __builtin_memcpy(&bits, &f, 4);
  int exponent = (int)((bits >> 23) & 0xFF) - 127 + 15;
  return (uint16_t)(exponent << 10 | ((bits >> 13) & 0x3FF));
}

static void encode_row(uint8_t *dst, const rgb_t *src, int w, int format, int bpp)
{
  static uint16_t half[256];
  static bool half_ready;
  if (format == F_RGBA16F && !half_ready) {
    for (unsigned i = 0; i < 256; i++)
      half[i] = half_of(i);
    half_ready = true;
  }
  for (int x = 0; x < w; x++) {
    rgb_t c = src[x];
    if (bpp == 8) {
      uint64_t v = half[c.r] | (uint64_t)half[c.g] << 16 | (uint64_t)half[c.b] << 32 | (uint64_t)0x3C00 << 48;
      __builtin_memcpy(dst + x * 8, &v, 8);
    } else {
      uint32_t v = format == F_RGB10A2
                     ? (uint32_t)(c.r * 1023 / 255) | (uint32_t)(c.g * 1023 / 255) << 10 | (uint32_t)(c.b * 1023 / 255) << 20 | 3u << 30
                     : (uint32_t)c.r | (uint32_t)c.g << 8 | (uint32_t)c.b << 16 | 0xFFu << 24;
      __builtin_memcpy(dst + x * 4, &v, 4);
    }
  }
}

/* --- text -------------------------------------------------------------------------------------- */

/* a glyph's coverage at pen (x64 in 1/64 pixel) and baseline y */
static void draw_glyph(const gf_glyph_t *g, int x64, int baseline, rgb_t colour)
{
  int left = (x64 + 32) / 64 + g->x0;
  int top = baseline + g->y0;
  for (int gy = 0; gy < g->h; gy++)
    for (int gx = 0; gx < g->w; gx++)
      blend(left + gx, top + gy, colour, g->coverage[gy * g->w + gx]);
}

static bool is_numeric_run(const char *s, int n)
{
  bool digit = false;
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9')
      digit = true;
    else if (c != '.' && c != ',' && c != '-' && c != '+' && c != ':' && c != '%' && c != '/' && c != 'x')
      return false;
  }
  return digit;
}

/* one glyph: on the GPU from the atlas, else blended into the strip */
static void put_glyph(gf_face_t face, int em, uint32_t cp, const gf_glyph_t *g, int x64, int baseline, rgb_t colour)
{
  if (gpu) { /* a soft shadow first, so the text reads on bright scenes too */
    float x = gpu_x + x64 / 64.0f, y = gpu_y + baseline, d = em / 14.0f > 1 ? em / 14.0f : 1;
    gpuui_glyph(face, em, cp, x + d, y + d, (gpu_colour_t){ 0, 0, 0, 0.55f * gpu_fade });
    gpuui_glyph(face, em, cp, x, y, to_gpu(colour, 1));
  } else
    draw_glyph(g, x64, baseline, colour);
}

/* the run's characters as drawn: one face, kerning between neighbours */
static void draw_run(const char *s, int n, gf_face_t face, int em, int x64, int baseline, rgb_t colour)
{
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    gf_face_t f = face;
    uint32_t cp = c;
    const gf_glyph_t *g = gamefont_glyph(face, em, c);
    if (!g) {
      f = GF_REGULAR;
      cp = '?';
      g = gamefont_glyph(f, em, cp);
    }
    if (g) {
      put_glyph(f, em, cp, g, x64, baseline, colour);
      x64 += g->advance;
    }
    if (i + 1 < n)
      x64 += gamefont_kern(face, em, c, (unsigned char)s[i + 1]);
  }
}

/* --- icons ------------------------------------------------------------------------------------- */

static float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
  float dx = bx - ax, dy = by - ay;
  float t = clampf(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0, 1);
  float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
  return root(ex * ex + ey * ey);
}

/* gz's checkbox as a rounded box: outlined, filled with a tick when on */
static void draw_checkbox(int left, int top, int size, bool on, rgb_t colour)
{
  if (gpu) {
    gpu_checkbox(gpu_x + left, gpu_y + top, size, on, colour);
    return;
  }
  float half = size / 2.0f;
  float radius = size * 0.22f;
  float line = size / 9.0f > 1.0f ? size / 9.0f : 1.0f;
  rgb_t dark = background;
  for (int y = 0; y < size + 2; y++) {
    for (int x = 0; x < size + 2; x++) {
      /* signed distance to the box's edge (negative inside) */
      float px = x + 0.5f - 1 - half, py = y + 0.5f - 1 - half;
      float qx = __builtin_fabsf(px) - (half - radius), qy = __builtin_fabsf(py) - (half - radius);
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      float d = root(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - radius;
      if (on) {
        float fill = clampf(0.5f - d, 0, 1);
        blend(left + x - 1, top + y - 1, colour, (unsigned)(fill * 235));
        /* the tick, through the box's lower left to its upper right */
        float ux = (x - 1 + 0.5f) / size, uy = (y - 1 + 0.5f) / size;
        float t = segment_distance(ux, uy, 0.24f, 0.52f, 0.42f, 0.70f);
        float t2 = segment_distance(ux, uy, 0.42f, 0.70f, 0.77f, 0.30f);
        float tick = clampf((line * 0.75f - (t < t2 ? t : t2) * size) + 0.5f, 0, 1);
        blend(left + x - 1, top + y - 1, dark, (unsigned)(tick * fill * 255));
      } else {
        float edge = clampf(line / 2 + 0.5f - __builtin_fabsf(d + line / 2), 0, 1);
        blend(left + x - 1, top + y - 1, colour, (unsigned)(edge * 230));
      }
    }
  }
}

/* a button-glyph icon centred in its cell (or cells) */
static void draw_symbol(uint32_t cp, int em, int left, int width, int strip_mid, rgb_t colour)
{
  const gf_glyph_t *g = gamefont_glyph(GF_GLYPHS, em, cp);
  if (!g)
    return;
  int x64 = (left + width / 2 - g->w / 2 - g->x0) * 64;
  int baseline = strip_mid - (g->y0 + g->h / 2);
  put_glyph(GF_GLYPHS, em, cp, g, x64, baseline, colour);
}

static uint32_t symbol_for(unsigned char c)
{
  switch (c) {
    case GFX_ICON_UP: return GF_ARROW_UP;
    case GFX_ICON_DOWN: return GF_ARROW_DOWN;
    case GFX_ICON_LEFT: return GF_ARROW_LEFT;
    case GFX_ICON_RIGHT: return GF_ARROW_RIGHT;
    case GFX_ICON_ADD: return GF_PLUS;
    case GFX_ICON_REMOVE: return GF_CLOSE;
    case GFX_ICON_WRENCH: return GF_GEAR; /* gz's wrench ("edit in memory"): the game has no wrench */
    default: return 0;
  }
}

/* gz's pixel art for an icon with no native form, scaled like the rest of the panel */
static void draw_legacy(unsigned char c, int left, int cells, int style, int scale, nui_legacy_fn legacy)
{
  if (!legacy)
    return;
  int w = cells * CELL_W * scale / NUI_SC_ONE;
  if (gpu) {
    gpu_icon(legacy, c, cells, gpu_x + left, gpu_y, w, strip_h, styles[style].colour);
    return;
  }
  for (int y = 0; y < strip_h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t rgb[3];
      if (legacy(c, x * NUI_SC_ONE / scale, y * NUI_SC_ONE / scale, style, rgb))
        blend(left + x, y, (rgb_t){ rgb[0], rgb[1], rgb[2] }, 255);
    }
  }
}

/* --- a row ------------------------------------------------------------------------------------- */

static int style_force = -1; /* while a row is drawn as it was or will be (its text's change, menu_row) */
static inline int style_of(const gfx_panel_t *panel, int row, int col)
{
  int style = panel->style[row][col] < GFX_NSTYLES ? panel->style[row][col] : GFX_NORMAL;
  return style_force >= 0 && (style == GFX_NORMAL || style == GFX_SELECTED) ? style_force : style;
}

static inline int cell_x(int col, int scale) { return (CELL_W + col * CELL_W) * scale / NUI_SC_ONE; }

static inline bool is_text(unsigned char c) { return c >= 0x20 && c < 0x7F; }

/* the light band behind the selected entry: fades in over its first fifth and out over its last third */
static void draw_selection(int from, int to)
{
  int width = to - from;
  if (width <= 0)
    return;
  if (gpu) { /* three quads each for the band and its hairlines: fade in, full, fade out */
    float x0 = gpu_x + from, w = (float)width, y = gpu_y, h = (float)strip_h;
    float stops[4] = { 0, 0.2f, 0.67f, 1 };
    for (int k = 0; k < 3; k++) {
      float a0 = k == 0 ? 0 : 1, a1 = k == 2 ? 0 : 1;
      float xa = x0 + stops[k] * w, xb = x0 + stops[k + 1] * w;
      gpu_colour_t band[4] = { to_gpu(band_light, a0 * 0.16f), to_gpu(band_light, a1 * 0.16f),
                               to_gpu(band_light, a0 * 0.16f), to_gpu(band_light, a1 * 0.16f) };
      gpuui_rect(xa, y, xb - xa, h, band);
      gpu_colour_t line[4] = { to_gpu(band_light, a0 * 0.45f), to_gpu(band_light, a1 * 0.45f),
                               to_gpu(band_light, a0 * 0.45f), to_gpu(band_light, a1 * 0.45f) };
      gpuui_rect(xa, y, xb - xa, 1, line);
      gpuui_rect(xa, y + h - 1, xb - xa, 1, line);
    }
    return;
  }
  for (int x = from; x < to; x++) {
    float u = (x - from + 0.5f) / width;
    float a = u < 0.2f ? u / 0.2f : u > 0.67f ? (1 - u) / 0.33f : 1;
    for (int y = 0; y < strip_h; y++)
      blend(x, y, band_light, (unsigned)(a * 40));
    blend(x, 0, band_light, (unsigned)(a * 110));
    blend(x, strip_h - 1, band_light, (unsigned)(a * 110));
  }
}

/* one past the last cell of the run of text starting at c (to a style change, an icon, the end, or
 * two spaces) */
static int run_end(const gfx_panel_t *panel, int row, int c, int cols)
{
  const char *ch = panel->ch[row];
  int style = style_of(panel, row, c);
  while (c < cols && is_text((unsigned char)ch[c]) && style_of(panel, row, c) == style &&
         !(ch[c] == ' ' && (c + 1 >= cols || ch[c + 1] == ' ' || !ch[c + 1])))
    c++;
  return c;
}

/* one past the last cell of the word starting at c, within a run ending at end */
static int word_end(const char *ch, int c, int end)
{
  while (c < end && ch[c] != ' ')
    c++;
  return c;
}

/* the numbers that form columns, by their first cell: 1 a left-aligned column, 2 a right-aligned one */
enum { FLOW = 0, COLUMN_LEFT = 1, COLUMN_RIGHT = 2 };
static uint8_t align_right[GFX_ROWS][GFX_COLS];

static void plan_numbers(const gfx_panel_t *panel, int rows, int cols)
{
  enum { MAX_NUMBERS = GFX_ROWS * GFX_COLS / 2 };
  static struct { uint8_t row, start, end; } nums[MAX_NUMBERS];
  static uint8_t starting[GFX_COLS + 1];  /* numbers starting at a cell */
  static int8_t end_start[GFX_COLS + 1]; /* the start of a number ending at a cell, -2 when there are several */
  int n = 0;
  __builtin_memset(starting, 0, sizeof(starting));
  __builtin_memset(end_start, -1, sizeof(end_start));
  for (int row = 0; row < rows; row++) {
    const char *ch = panel->ch[row];
    for (int c = 0; c < cols;) {
      if (!is_text((unsigned char)ch[c]) || ch[c] == ' ') {
        c++;
        continue;
      }
      int end = run_end(panel, row, c, cols);
      while (c < end) {
        int we = word_end(ch, c, end);
        align_right[row][c] = FLOW;
        if (is_numeric_run(ch + c, we - c) && n < MAX_NUMBERS) {
          nums[n++] = (typeof(nums[0])){ (uint8_t)row, (uint8_t)c, (uint8_t)we };
          starting[c] += starting[c] < 255;
          end_start[we] = end_start[we] == -1 ? (int8_t)c : end_start[we] == c ? end_start[we] : -2;
        }
        c = we < end ? we + 1 : we;
      }
    }
  }
  /* a column that ends in one cell from different starts is right-aligned (gz padded it); one that starts
   * in one cell (in another row too) is left-aligned; a number in neither is part of its text. (Two numbers
   * of one row never share a start or an end, so these are always other rows'.) */
  for (int i = 0; i < n; i++)
    align_right[nums[i].row][nums[i].start] = end_start[nums[i].end] == -2 ? COLUMN_RIGHT
                                              : starting[nums[i].start] > 1 ? COLUMN_LEFT
                                                                            : FLOW;
}

static void draw_row(const gfx_panel_t *panel, int row, int cols, int scale, nui_legacy_fn legacy)
{
  const char *ch = panel->ch[row];
  int fill = (int)(strip_h * TEXT_FILL + 0.5f);
  int em = gamefont_em_for_height(GF_REGULAR, fill);
  int baseline = (strip_h - fill + 1) / 2 + gamefont_ascent(GF_REGULAR, em);
  int mid = strip_h / 2;

  /* the selected entry's band, under everything else */
  int first = -1, last = -1;
  for (int c = 0; c < cols; c++) {
    if (ch[c] && ch[c] != ' ' && style_of(panel, row, c) == GFX_SELECTED) {
      if (first < 0)
        first = c;
      last = c;
    }
  }
  if (first >= 0)
    draw_selection(cell_x(first, scale) - 6 * scale / NUI_SC_ONE, cell_x(last + 1, scale) + 10 * scale / NUI_SC_ONE);

  for (int c = 0; c < cols;) {
    unsigned char k = (unsigned char)ch[c];
    int style = style_of(panel, row, c);
    rgb_t colour = styles[style].colour;
    if (!k || k == ' ' || k == GFX_WIDE_REST) {
      c++;
      continue;
    }
    if (k == GFX_PICTURE) { /* the game's picture of an item: the GPU's drawing only */
      c += 2;
      continue;
    }
    if (!is_text(k)) { /* an icon: one cell, or two with GFX_WIDE_REST after it */
      int cells = c + 1 < cols && (unsigned char)ch[c + 1] == GFX_WIDE_REST ? 2 : 1;
      int left = cell_x(c, scale), width = cells * CELL_W * scale / NUI_SC_ONE;
      uint32_t symbol = symbol_for(k);
      if (k == GFX_CHECK_OFF || k == GFX_CHECK_ON) {
        int size = 8 * scale / NUI_SC_ONE;
        draw_checkbox(left + (width - size) / 2, mid - size / 2, size, k == GFX_CHECK_ON, colour);
      } else if (symbol) {
        draw_symbol(symbol, (int)(em * GLYPH_EM), left, width, mid, colour);
      } else {
        draw_legacy(k, left, cells, style, scale, legacy);
      }
      c += cells;
      continue;
    }
    int end = run_end(panel, row, c, cols);
    gf_face_t face = styles[style].face;
    const gf_glyph_t *space = gamefont_glyph(face, em, ' ');
    int space64 = space ? space->advance : 0;
    int pen = 0, start64 = cell_x(c, scale) * 64;
    if (style == GFX_LOG && end >= cols) { /* a log line: right-aligned at the corner, as gz has it */
      int all = 0;
      for (int w = c; w < end;) {
        int we = word_end(ch, w, end);
        all += gamefont_measure(face, em, ch + w, we - w) + (w > c ? space64 : 0);
        w = we < end ? we + 1 : we;
      }
      start64 = cell_x(cols, scale) * 64 - all;
    }
    for (int w = c; w < end;) { /* the run's first word starts at its cell, the rest follow it */
      int we = word_end(ch, w, end);
      int column = is_numeric_run(ch + w, we - w) ? align_right[row][w] : FLOW;
      int width = gamefont_measure(face, em, ch + w, we - w);
      int x64 = w == c ? start64 : column == COLUMN_LEFT ? cell_x(w, scale) * 64 : pen + space64;
      if (column == COLUMN_RIGHT)
        x64 = cell_x(we, scale) * 64 - width;
      draw_run(ch + w, we - w, face, em, x64, baseline, colour);
      pen = x64 + width;
      w = we < end ? we + 1 : we;
    }
    c = end;
  }
}

/* --- a panel ---------------------------------------------------------------------------------- */

static void render(const gfx_panel_t *panel, uint8_t *dst, int scale, int w, int h, int format,
                   int bytes_per_pixel, nui_legacy_fn legacy)
{
  int cols = panel->cols < GFX_COLS ? panel->cols : GFX_COLS;
  int rows = panel->rows < GFX_ROWS ? panel->rows : GFX_ROWS;
  plan_numbers(panel, rows, cols);
  size_t stride = (size_t)w * bytes_per_pixel;

  /* the background row, encoded once */
  strip_w = w;
  for (int x = 0; x < w; x++)
    strip[0][x] = background;
  static uint8_t blank[MAX_W * 8];
  encode_row(blank, strip[0], w, format, bytes_per_pixel);

  int y = 0;
  for (int row = 0; row <= rows && y < h; row++) {
    /* the text row's strip: from its top to the next row's (font rows TEXT_TOP + yoff + row x 10) */
    int top = (TEXT_TOP_FP + panel->yoff + row * CELL_HT) * scale / NUI_SC_ONE;
    for (; y < top && y < h; y++)
      __builtin_memcpy(dst + (size_t)y * stride, blank, stride);
    if (row == rows)
      break;
    int bottom = (TEXT_TOP_FP + panel->yoff + (row + 1) * CELL_HT) * scale / NUI_SC_ONE;
    strip_h = bottom - top;
    for (int sy = 0; sy < strip_h; sy++)
      for (int x = 0; x < w; x++)
        strip[sy][x] = background;
    draw_row(panel, row, cols, scale, legacy);
    for (int sy = 0; sy < strip_h && y < h; sy++, y++)
      encode_row(dst + (size_t)y * stride, strip[sy], w, format, bytes_per_pixel);
  }
  for (; y < h; y++)
    __builtin_memcpy(dst + (size_t)y * stride, blank, stride);
}

bool nativeui_rasterise(const gfx_panel_t *panel, uint8_t *dst, int scale, int w, int h, int format,
                        int bytes_per_pixel, nui_legacy_fn legacy)
{
  if (!nativeui_ready() || panel->pad || w <= 0 || h <= 0 || w > MAX_W || (bytes_per_pixel != 4 && bytes_per_pixel != 8))
    return false;
  int row_h = CELL_HT * scale / NUI_SC_ONE;
  if (row_h <= 0 || row_h + 1 > MAX_STRIP_H)
    return false;
  /* a draw the glyph cache had no room for is drawn again once it has started afresh */
  for (int attempt = 0; attempt < 2; attempt++) {
    gamefont_begin_draw();
    render(panel, dst, scale, w, h, format, bytes_per_pixel, legacy);
    if (gamefont_draw_complete())
      return true;
  }
  return false; /* the caller draws it its own way */
}

/* --- the GPU path ------------------------------------------------------------------------------- */
/* The menu is laid out the game's way, by what its rows are (gfx_row): taller rows, the breadcrumb in
 * the game's gold over a hairline, headings with a rule, values right-aligned between chevrons, the
 * hint's buttons as the game's button glyphs, and the controls at its foot. The other panels keep
 * gz's rows. Shapes (chevrons, checkboxes, gz's icons) are painted once into gpuui's atlas. */

static const rgb_t band_dark = { 0x0A, 0x09, 0x08 };
static const rgb_t crumb = { 0xC4, 0xB2, 0x84 }; /* the breadcrumb's path, before the menu's own name */

/* each kind of row: its height and its text's (ascent plus descent), in font pixels */
static const struct { float height, text; } row_fp[GFX_ROW_KINDS] = {
  [GFX_ROW_TEXT] = { CELL_HT, CELL_HT * TEXT_FILL },
  [GFX_ROW_TITLE] = { 15, 11 },
  [GFX_ROW_GAP] = { 7, 0 },
  [GFX_ROW_HEADING] = { 13, 8 }, /* set low in its row: the space above a group */
  [GFX_ROW_ITEM] = { 13, 9 },
  [GFX_ROW_LINE] = { 11, 8.5f },
  [GFX_ROW_HINT] = { 11, 8.5f },
  [GFX_ROW_WATCH] = { 11, 9 },
  [GFX_ROW_BUTTONS] = { 12, 9 },
  [GFX_ROW_SKIP] = { 0, 0 },
  [GFX_ROW_CAPTION] = { 12, 7.5f }, /* smaller than the watches under it; its line in the room below the text */
};
#define CAPTION_TEXT_AT 0.38f /* of the space a caption's row has over its text: above it (the rest: its line) */
#define CAPTION_LINE_UP 2.5f  /* its line: this many font pixels up from the row's foot */
#define MENU_PAD_FP 9       /* the menu's padding above the title and below the controls */
#define LIST_PAD_FP 5       /* the other panels' (the watches, the input display) */
#define BOX_PAD_X 14        /* the menu's box: from its sides to what's in it (the title, the selection's end) */
#define BOX_PAD_Y 12        /* ... from its top to the title, and from the controls to its foot */
#define MENU_EDGE_Y (BOX_PAD_Y - MENU_PAD_FP) /* what the box adds above and below the grid's own padding */
#define GUIDE_FP 12         /* the controls' row, after a gap under the hint */
#define TITLE_GAP_FP 11     /* the gap under the title, its ornament line LINE_IN_GAP down it: room for the
                               cursor's glow on the first item below it */
#define LINE_IN_GAP 0.42f
#define ROW_UNIT_FP 13      /* nativeui_menu_rows counts a row as this: the tallest in the list */
#define HEADING_DROP 0.65f  /* of the space a heading's row has over its text: above it */
#define SCROLL_MARK_COL 0   /* menu.c: the ^ / v marks */

static float fp_px; /* screen pixels per font pixel (scale / 16) */

#define SELECT_OVER 4.5f /* the menu's cursor reaches about this far past what's in it (its room and line) */
#define SCROLL_W 4.5f   /* the scroll bar's width (it's in the list's first column, as the game has it left) */
#define THUMB_W 0.55f   /* its thumb's, of that: inside the track with room round it, as the game's */

/* the menu's list scrolls, and the game's scroll bar can be drawn */
static bool scroll_bar_shown(const gfx_panel_t *panel)
{
  return panel->scroll_rows && panel->scroll_items > panel->scroll_rows && gpuui_image_ready(GAMEIMAGE_SCROLL_BASE) &&
         gpuui_image_ready(GAMEIMAGE_SCROLL_BAR);
}

static bool laid_out(const gfx_panel_t *panel)
{
  for (int r = 0; r < GFX_ROWS; r++)
    if (panel->kind[r] & GFX_ROW_KIND)
      return true;
  return false;
}

/* the menu (its breadcrumb at the top) and its controls at the foot; else a list (the watches) */
static bool is_menu(const gfx_panel_t *panel)
{
  return (panel->kind[0] & GFX_ROW_KIND) == GFX_ROW_TITLE;
}

/* the controls go at the foot of a menu with items (not while one of them is edited on its own) */
static bool shows_controls(const gfx_panel_t *panel)
{
  if (!is_menu(panel))
    return false;
  for (int r = 0; r < GFX_ROWS; r++)
    if ((panel->kind[r] & GFX_ROW_KIND) == GFX_ROW_ITEM)
      return true;
  return false;
}

static int kind_of(const gfx_panel_t *panel, int row)
{
  int k = panel->kind[row] & GFX_ROW_KIND;
  return k < GFX_ROW_KINDS ? k : GFX_ROW_TEXT;
}

/* a row's height in font pixels: its kind's, the gap under the title taller */
static float row_height_fp(const gfx_panel_t *panel, int row)
{
  int kind = kind_of(panel, row);
  if (kind == GFX_ROW_GAP && row > 0 && kind_of(panel, row - 1) == GFX_ROW_TITLE)
    return TITLE_GAP_FP;
  return row_fp[kind].height;
}

/* the height of rows [0, rows) laid out, with the padding (and the menu's controls), in font pixels */
static float menu_height_fp(const gfx_panel_t *panel, int rows)
{
  float h = is_menu(panel) ? 2 * MENU_PAD_FP : 2 * LIST_PAD_FP;
  if (shows_controls(panel))
    h += row_fp[GFX_ROW_GAP].height + GUIDE_FP;
  for (int r = 0; r < rows; r++)
    h += row_height_fp(panel, r);
  return h;
}

int nativeui_menu_rows(int space, int scale)
{
  /* menu.c's rows: the title (one row, mostly), a gap, items, a gap and the hint; every row counted as
   * ROW_UNIT_FP but the two gaps and the title's extra */
  float fp = (float)space * NUI_SC_ONE / scale - 2 * MENU_PAD_FP - 2 * MENU_EDGE_Y - row_fp[GFX_ROW_GAP].height - GUIDE_FP;
  fp -= TITLE_GAP_FP - row_fp[GFX_ROW_GAP].height;
  fp += 2 * (ROW_UNIT_FP - row_fp[GFX_ROW_GAP].height) - (row_fp[GFX_ROW_TITLE].height - ROW_UNIT_FP);
  return fp > 0 ? (int)(fp / ROW_UNIT_FP) : 0;
}

/* --- shapes, painted into the atlas -------------------------------------------------------------- */

enum {
  SHAPE_CHEVRON = 1 /* + its direction */, SHAPE_BOX = 8, SHAPE_BOX_FILLED, SHAPE_TICK, SHAPE_CROSS, SHAPE_PLUS,
  SHAPE_FOLDER, SHAPE_LOCK,
  SHAPE_ICON = 0x100 /* + gz's character */
};
enum { RIGHT, LEFT, UP, DOWN };
static const int directions[4] = { RIGHT, LEFT, UP, DOWN };

static uint8_t coverage_of(float c) { return (uint8_t)(clampf(c, 0, 1) * 255 + 0.5f); }

/* a chevron's two strokes, pointing its way, filling its w x h */
static void paint_chevron(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  int dir = *(const int *)arg;
  bool across = dir == RIGHT || dir == LEFT;
  float t = (across ? w : h) * 0.34f;
  t = t < 1.2f ? 1.2f : t;
  float pad = t / 2 + 0.5f;
  float ax = pad, ay = pad, bx = w - pad, by = h / 2.0f, cx = pad, cy = h - pad; /* right */
  if (!across) { /* down: the tip at the bottom */
    ax = pad, ay = pad, bx = w / 2.0f, by = h - pad, cx = w - pad, cy = pad;
  }
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float px = x + 0.5f, py = y + 0.5f;
      if (dir == LEFT)
        px = w - px;
      if (dir == UP)
        py = h - py;
      float d1 = segment_distance(px, py, ax, ay, bx, by), d2 = segment_distance(px, py, bx, by, cx, cy);
      cov[y * stride + x] = coverage_of(t / 2 + 0.5f - (d1 < d2 ? d1 : d2));
    }
  }
}

/* signed distance to a rounded square of side s (centred in it), negative inside */
static float box_distance(float px, float py, float s, float radius)
{
  float half = s / 2 - 0.5f;
  float qx = __builtin_fabsf(px - s / 2) - (half - radius), qy = __builtin_fabsf(py - s / 2) - (half - radius);
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  return root(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - radius;
}

/* gz's checkbox, rounded: its outline, or (arg set) filled */
static void paint_box(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  float s = (float)w, line = s / 8 > 1.2f ? s / 8 : 1.2f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float d = box_distance(x + 0.5f, y + 0.5f, s, s * 0.24f);
      cov[y * stride + x] = coverage_of(arg ? 0.5f - d : line / 2 + 0.5f - __builtin_fabsf(d + line / 2));
    }
  }
}

/* the tick in a checked box */
static void paint_tick(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  (void)arg;
  float s = (float)w, t = s * 0.15f > 1.3f ? s * 0.15f : 1.3f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float ux = x + 0.5f, uy = y + 0.5f;
      float d1 = segment_distance(ux, uy, s * 0.26f, s * 0.52f, s * 0.43f, s * 0.69f);
      float d2 = segment_distance(ux, uy, s * 0.43f, s * 0.69f, s * 0.75f, s * 0.32f);
      cov[y * stride + x] = coverage_of(t / 2 + 0.5f - (d1 < d2 ? d1 : d2));
    }
  }
}

/* a thin cross (arg NULL) or plus (arg set) filling its square */
static void paint_cross(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  float s = (float)w, t = s * 0.16f > 1.2f ? s * 0.16f : 1.2f, p = t / 2 + 0.5f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float px = x + 0.5f, py = y + 0.5f, d1, d2;
      if (arg) {
        d1 = segment_distance(px, py, s / 2, p, s / 2, s - p);
        d2 = segment_distance(px, py, p, s / 2, s - p, s / 2);
      } else {
        d1 = segment_distance(px, py, p, p, s - p, s - p);
        d2 = segment_distance(px, py, s - p, p, p, s - p);
      }
      cov[y * stride + x] = coverage_of(t / 2 + 0.5f - (d1 < d2 ? d1 : d2));
    }
  }
}

/* gz's pixel art for an icon with no native form, as a shape: its brightness, one colour like the
 * game's own icons */
typedef struct { nui_legacy_fn legacy; unsigned char c; int cells; } icon_art_t;

static void paint_icon(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  const icon_art_t *art = arg;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t rgb[3];
      unsigned v = 0;
      if (art->legacy(art->c, x * art->cells * CELL_W / w, y * CELL_HT / h, GFX_NORMAL, rgb)) {
        v = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
        v = v > rgb[2] ? v : rgb[2];
        v = v * 5 / 4 > 255 ? 255 : v * 5 / 4;
      }
      cov[y * stride + x] = (uint8_t)v;
    }
  }
}

/* a chevron of height h (pointing across) or width h (up or down) with its middle at (x, y) */
static void chevron(int dir, float h, float x, float y, gpu_colour_t colour)
{
  int big = (int)(h + 0.5f), small = (int)(h * 0.56f + 0.5f);
  if (big < 3)
    return;
  int w = dir == RIGHT || dir == LEFT ? small : big, hh = dir == RIGHT || dir == LEFT ? big : small;
  gpuui_shape(SHAPE_CHEVRON + dir, w, hh, paint_chevron, &directions[dir], x - w / 2.0f, y - hh / 2.0f, colour);
}

/* gz's checkbox, size x size at (x, y): an outline, or the game's gold with a tick when on */
static void gpu_checkbox(float x, float y, int size, bool on, rgb_t colour)
{
  if (size < 4)
    return;
  if (on) {
    gpuui_shape(SHAPE_BOX_FILLED, size, size, paint_box, &directions[0], x, y, to_gpu(styles[GFX_TITLE].colour, 1));
    gpuui_shape(SHAPE_TICK, size, size, paint_tick, 0, x, y, to_gpu(band_dark, 1));
  } else {
    gpuui_shape(SHAPE_BOX, size, size, paint_box, 0, x, y, to_gpu(colour, 0.9f));
  }
}

/* gz's art for icon c (cells wide) as a w x h shape at (x, y) */
/* coverage of a shape by 4 x 4 samples a pixel */
typedef bool (*inside_t)(float x, float y, int w, int h, const void *arg);
static void paint_sampled(uint8_t *cov, int w, int h, int stride, inside_t inside, const void *arg)
{
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int n = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
          n += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4, w, h, arg);
      cov[y * stride + x] = (uint8_t)(n * 255 / 16);
    }
  }
}

/* inside a rounded rectangle */
static bool in_rounded(float x, float y, float x0, float y0, float x1, float y1, float r)
{
  if (x < x0 || x > x1 || y < y0 || y > y1)
    return false;
  float cx = x < x0 + r ? x0 + r : x > x1 - r ? x1 - r : x, cy = y < y0 + r ? y0 + r : y > y1 - r ? y1 - r : y;
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

/* gz's folder ("the built-in watches") as an outline: a body with a tab on its top left */
static bool inside_folder(float x, float y, int w, int h, const void *arg)
{
  (void)arg;
  float t = h * 0.12f > 1.2f ? h * 0.12f : 1.2f, r = h * 0.14f;
  bool outer = in_rounded(x, y, 0, h * 0.22f, (float)w, (float)h, r) || in_rounded(x, y, 0, 0, w * 0.45f, h * 0.4f, r);
  bool inner = in_rounded(x, y, t, h * 0.22f + t, w - t, h - t, r * 0.5f) ||
               in_rounded(x, y, t, t, w * 0.45f - t, h * 0.4f, r * 0.5f);
  return outer && !inner;
}
static void paint_folder(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  paint_sampled(cov, w, h, stride, inside_folder, arg);
}

/* gz's padlock (a watch held, as JS height's lock): a body, its shackle above it */
static bool inside_lock(float x, float y, int w, int h, const void *arg)
{
  (void)arg;
  float t = w * 0.15f > 1.2f ? w * 0.15f : 1.2f, body = h * 0.5f;
  if (in_rounded(x, y, 0, body, (float)w, (float)h, w * 0.14f))
    return true;
  /* the shackle: an upturned U, an arc over two legs down into the body */
  float cx = w / 2.0f, cy = h * 0.32f, ro = w * 0.32f, dx = x - cx;
  if (y > cy)
    return y < body + 1 && dx * dx <= ro * ro && dx * dx >= (ro - t) * (ro - t);
  float d = root(dx * dx + (y - cy) * (y - cy));
  return d <= ro && d >= ro - t;
}
static void paint_lock(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  paint_sampled(cov, w, h, stride, inside_lock, arg);
}

/* an icon the game has no form of, in a w x h place at (x, y): the folder and the padlock as shapes in the
 * line of the others, anything else as gz's art */
static void gpu_icon(nui_legacy_fn legacy, unsigned char c, int cells, float x, float y, int w, int h, rgb_t colour)
{
  if (w <= 0 || h <= 0)
    return;
  if (c == GFX_ICON_FOLDER || c == (unsigned char)GFX_ICON_LOCK) {
    int sh = (int)(h * (c == GFX_ICON_FOLDER ? 0.62f : 0.7f) + 0.5f);
    int sw = c == GFX_ICON_FOLDER ? (int)(sh * 1.3f + 0.5f) : (int)(sh * 0.78f + 0.5f);
    gpuui_shape(c == GFX_ICON_FOLDER ? SHAPE_FOLDER : SHAPE_LOCK, sw, sh,
                c == GFX_ICON_FOLDER ? paint_folder : paint_lock, 0, x + (w - sw) / 2.0f, y + (h - sh) / 2.0f,
                to_gpu(colour, 1));
    return;
  }
  if (!legacy)
    return;
  icon_art_t art = { legacy, c, cells };
  gpuui_shape(SHAPE_ICON + c, w, h, paint_icon, &art, x, y, to_gpu(colour, 1));
}

/* --- the band ----------------------------------------------------------------------------------- */

/* a horizontal band from x0 to x1 that fades in over `in` of it and out over `out` (fractions) */
static void faded_strip(float x0, float x1, float y, float h, float in, float out, rgb_t c, float alpha)
{
  float w = x1 - x0;
  if (w <= 0 || h <= 0)
    return;
  float stops[4] = { 0, in, 1 - out, 1 };
  for (int k = 0; k < 3; k++) {
    float a0 = k == 0 ? 0 : alpha, a1 = k == 2 ? 0 : alpha;
    float xa = x0 + stops[k] * w, xb = x0 + stops[k + 1] * w;
    gpu_colour_t q[4] = { to_gpu(c, a0), to_gpu(c, a1), to_gpu(c, a0), to_gpu(c, a1) };
    gpuui_rect(xa, y, xb - xa, h, q);
  }
}

static float smooth(float t)
{
  t = clampf(t, 0, 1);
  return t * t * (3 - 2 * t);
}

/* one of the game's pictures stretched along one axis with its ends kept (its first and last `cap` pixels
 * along it, scaled as the other axis is): across (a bar) or down (vertical); tinted by `colour`, or with
 * `map` its black and white mapped to map[0] and map[1] (at colour's alpha), as the game's materials do */
static void picture_bar(int image, float x, float y, float w, float h, float cap, bool vertical, gpu_colour_t colour,
                        const gpu_colour_t *map)
{
  static const int sizes[GAMEIMAGES][2] = {
    [GAMEIMAGE_CURSOR_LINE] = { 640, 173 }, [GAMEIMAGE_CURSOR_GLOW] = { 640, 173 },
    [GAMEIMAGE_SCROLL_BAR] = { 22, 136 }, [GAMEIMAGE_SCROLL_BASE] = { 32, 312 }, [GAMEIMAGE_LINE] = { 240, 43 },
  };
  float tw = (float)sizes[image][0], th = (float)sizes[image][1];
  if (tw <= 0 || w <= 0 || h <= 0)
    return;
  float along = vertical ? h : w, across = vertical ? w : h, len = vertical ? th : tw;
  float end = cap * across / (vertical ? tw : th); /* an end on screen */
  if (2 * end > along)
    end = along / 2;
  float c = cap / len; /* an end in the picture */
  float at[4] = { 0, end, along - end, along }, st[4] = { 0, c, 1 - c, 1 };
  for (int k = 0; k < 3; k++) {
    float crop[4];
    float px = x, py = y, pw = w, ph = h;
    if (vertical) {
      crop[0] = 0, crop[1] = st[k], crop[2] = 1, crop[3] = st[k + 1];
      py = y + at[k], ph = at[k + 1] - at[k];
    } else {
      crop[0] = st[k], crop[1] = 0, crop[2] = st[k + 1], crop[3] = 1;
      px = x + at[k], pw = at[k + 1] - at[k];
    }
    if (map)
      gpuui_image_mapped(image, px, py, pw, ph, crop, map[0], map[1], colour.a);
    else
      gpuui_image(image, px, py, pw, ph, crop, colour);
  }
}

/* one of the game's frames: its picture cut in nine, the corners and edges `border` pixels of it (`px` screen
 * pixels each), the middle left out, stretched round a w x h box */
static void picture_frame_mapped(int image, float x, float y, float w, float h, float border, float px, float tw,
                                 float th, gpu_colour_t colour, const gpu_colour_t *map)
{
  float b = border * px; /* on screen */
  if (2 * b > w)
    b = w / 2;
  if (2 * b > h)
    b = h / 2;
  float xs[4] = { x, x + b, x + w - b, x + w }, ys[4] = { y, y + b, y + h - b, y + h };
  float ss[4] = { 0, border / tw, 1 - border / tw, 1 }, ts[4] = { 0, border / th, 1 - border / th, 1 };
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 3; i++) {
      if (i == 1 && j == 1)
        continue;
      float crop[4] = { ss[i], ts[j], ss[i + 1], ts[j + 1] };
      if (map)
        gpuui_image_mapped(image, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], crop, map[0], map[1], colour.a);
      else
        gpuui_image(image, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], crop, colour);
    }
  }
}

static void picture_frame(int image, float x, float y, float w, float h, float border, float px, float tw, float th,
                          gpu_colour_t colour)
{
  picture_frame_mapped(image, x, y, w, h, border, px, tw, th, colour, 0);
}

/* The panel's background: a dark band that fades out on its sides (over fl, fr, ft and fb pixels; band_of
 * fades only the left and right, as the game's control guide). A grid of quads, finer in the fades, each corner's alpha the
 * product of the two fades there (one quad per corner would show its diagonal). */
#define FADE_STEPS 4
static void draw_band(float x, float y, float w, float h, float fl, float fr, float ft, float fb, float alpha)
{
  enum { N = 2 * (FADE_STEPS + 1) };
  float xs[N], ax[N], ys[N], ay[N];
  for (int i = 0; i <= FADE_STEPS; i++) {
    float t = (float)i / FADE_STEPS;
    xs[i] = x + fl * t, ax[i] = smooth(t);
    xs[N - 1 - i] = x + w - fr * t, ax[N - 1 - i] = smooth(t);
    ys[i] = y + ft * t, ay[i] = smooth(t);
    ys[N - 1 - i] = y + h - fb * t, ay[N - 1 - i] = smooth(t);
  }
  for (int j = 0; j + 1 < N; j++) {
    for (int i = 0; i + 1 < N; i++) {
      if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j])
        continue;
      gpu_colour_t c[4] = { to_gpu(band_dark, alpha * ax[i] * ay[j]), to_gpu(band_dark, alpha * ax[i + 1] * ay[j]),
                            to_gpu(band_dark, alpha * ax[i] * ay[j + 1]), to_gpu(band_dark, alpha * ax[i + 1] * ay[j + 1]) };
      if (c[0].a + c[1].a + c[2].a + c[3].a > 0)
        gpuui_rect(xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], c);
    }
  }
}

/* The menu's box, as the game's info boxes (an item's description on its pause screen): near black and
 * solid, with hard edges. At the backdrop opacity on a curve that puts its default (60%) at about the
 * game's box's density (84%), the ends staying where they are. */
static void draw_box(float x, float y, float w, float h, float opacity)
{
  float clear = 1 - clampf(opacity, 0, 1);
  if (w > 0 && h > 0)
    gpuui_fill(x, y, w, h, to_gpu(band_dark, 1 - clear * clear));
}

/* The game's ornament line, as its info boxes have under their title, from x0 to x1 with its line at y,
 * h high: its picture's curl at each end facing out (the left part of it, mirrored for the right) and its
 * line stretched between. LINE_CAP of the picture holds the curl and where the line tapers in; the line
 * is LINE_AT down it. */
#define LINE_CAP 0.6f
#define LINE_AT 0.62f
static bool ornament_line(float x0, float x1, float y, float h, gpu_colour_t colour)
{
  float cap = 240 * LINE_CAP * h / 43, top = y - LINE_AT * h;
  if (!gpuui_image_ready(GAMEIMAGE_LINE) || 2 * cap > x1 - x0)
    return false;
  const float left[4] = { 0, 0, LINE_CAP, 1 }, line[4] = { 0.75f, 0, 0.9f, 1 }, right[4] = { LINE_CAP, 0, 0, 1 };
  gpuui_image(GAMEIMAGE_LINE, x0, top, cap, h, left, colour);
  gpuui_image(GAMEIMAGE_LINE, x0 + cap, top, x1 - x0 - 2 * cap, h, line, colour);
  gpuui_image(GAMEIMAGE_LINE, x1 - cap, top, cap, h, right, colour);
  return true;
}

/* --- a menu row ----------------------------------------------------------------------------------- */

typedef struct {
  const gfx_panel_t *panel;
  int row, cols, kind, scale;
  nui_legacy_fn legacy;
  float h;            /* the row's height (pixels); gpu_x / gpu_y are its top-left */
  float right;        /* the content's right edge, from gpu_x */
  int em, baseline;   /* the text's em and baseline (from gpu_y), in pixels */
  int switch_from;    /* an item's "  < value >" from this cell (right-aligned), or cols */
  float switch_shift; /* ... moved right this far */
  int indent_from;    /* an item under a heading (GFX_ROW_INDENT): its cells from this one ... */
  float indent_shift; /* ... moved this far (left: gz's indent is two cells, wide in the game's type) */
  int opens_at;       /* an item that opens a menu: its " >" cell, or -1 */
  int crumb_end;      /* title rows: the path up to this cell of the last one, then the menu's name */
  bool grid;          /* a character a cell (GFX_ROW_GRID) */
  float value_right;  /* the menu's value column ends here (0: at `right`) */
  int first_text, last_text, first_sel, last_sel; /* row_setup's: the cells with text, the selected ones */
  int last_title_row;
} mrow_t;

#define LIST_COL 2                    /* menu.c's ITEM_COL: the items' first cell */
#define ITEM_INDENT_COL (LIST_COL + 2) /* ... and an indented one's (menu.h's MENU_INDENT, 2) */
#define INDENT_EM 0.5f                 /* an indented item's start from its heading's, in its em */
static float col_x(const mrow_t *r, int col)
{
  return (float)cell_x(col, r->scale) + (col >= r->indent_from ? r->indent_shift : 0) +
         (col >= r->switch_from ? r->switch_shift : 0);
}

/* the hint's buttons, drawn as the game's button glyphs */
static const struct { const char *word; uint32_t glyph, other; } buttons[] = {
  { "L/R", GF_BTN_L, GF_BTN_R }, { "ZL/ZR", GF_BTN_ZL, GF_BTN_ZR },
  { "d-left/right", GF_DPAD_LEFT_RIGHT, 0 }, { "d-l/r", GF_DPAD_LEFT_RIGHT, 0 }, { "d-up/down", GF_DPAD_UP_DOWN, 0 },
  { "d-u/d", GF_DPAD_UP_DOWN, 0 }, { "d-left", GF_DPAD_LEFT, 0 }, { "d-right", GF_DPAD_RIGHT, 0 }, /* not menu text */
  { "d-up", GF_DPAD_UP, 0 }, { "d-down", GF_DPAD_DOWN, 0 }, { "d-pad", GF_DPAD, 0 }, /* not menu text */
  { "L3", GF_LSTICK_PRESS, 0 }, { "R3", GF_RSTICK_PRESS, 0 },
  { "ZL", GF_BTN_ZL, 0 }, { "ZR", GF_BTN_ZR, 0 }, { "L", GF_BTN_L, 0 }, { "R", GF_BTN_R, 0 }, { "A", GF_BTN_A, 0 },
  { "B", GF_BTN_B, 0 }, { "X", GF_BTN_X, 0 }, { "Y", GF_BTN_Y, 0 },
};

/* the button the word (less an opening bracket and a closing ':', ',', ')' or '.') names: its glyph, 0 if
 * none (*other: a second one, as in "L/R"); *pre and *post: the characters around it */
static uint32_t button_in(const char *w, int n, int *pre, int *post, uint32_t *other)
{
  *pre = n > 0 && w[0] == '(';
  *post = 0;
  while (n - *post > *pre && (w[n - 1 - *post] == ':' || w[n - 1 - *post] == ',' || w[n - 1 - *post] == ')' ||
                              w[n - 1 - *post] == '.'))
    (*post)++;
  int len = n - *pre - *post;
  for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
    const char *b = buttons[i].word;
    int k = 0;
    while (k < len && b[k] && b[k] == w[*pre + k])
      k++;
    if (k == len && !b[k]) {
      /* a single letter in a translation is a button only as "L:" or "(L": "A" or "Y" may be a word there */
      bool colon = *post > 0 && w[n - *post] == ':';
      if (len == 1 && lang_current() != 0 && !colon && !*pre)
        return 0;
      *other = buttons[i].other;
      return buttons[i].glyph;
    }
  }
  return 0;
}

static int glyph_em(int em) { return (int)(em * GLYPH_EM); }

/* the type's capitals' height at em, in pixels: its "H" (an estimate until the type is there) */
static float cap_height(gf_face_t face, int em)
{
  const gf_glyph_t *h = gamefont_glyph(face, em, 'H');
  return h && h->h > 0 ? (float)-h->y0 : em * 0.66f;
}

/* where the row's checkboxes, chevrons and symbols are centred, above the baseline: the capitals' middle, the
 * row's middle (row_metrics) */
static float icon_mid(const mrow_t *r)
{
  return cap_height(r->kind == GFX_ROW_TITLE ? GF_BOLD : GF_REGULAR, r->em) / 2;
}

/* a button glyph with its left edge at x, centred on the text's middle; returns its width */
static float button_glyph(const mrow_t *r, uint32_t cp, float x, bool draw, rgb_t colour)
{
  int gem = glyph_em(r->em);
  const gf_glyph_t *g = gamefont_glyph(GF_GLYPHS, gem, cp);
  if (!g)
    return 0;
  if (draw) {
    int mid = r->baseline - (int)(icon_mid(r) + 0.5f);
    int base = mid - (g->y0 + g->h / 2);
    put_glyph(GF_GLYPHS, gem, cp, g, (int)((x - g->x0) * 64), base, colour);
  }
  return (float)g->w;
}

static gf_face_t face_at(const mrow_t *r, int col, int style)
{
  if (r->kind == GFX_ROW_TITLE)
    return r->row == r->last_title_row && col >= r->crumb_end ? GF_BOLD : GF_REGULAR;
  return styles[style].face;
}

static rgb_t colour_at(const mrow_t *r, int col, int style)
{
  if (r->kind == GFX_ROW_WATCH && col < r->switch_from && style == GFX_NORMAL)
    return styles[GFX_DIM].colour;
  if (r->kind == GFX_ROW_TITLE && !(r->row == r->last_title_row && col >= r->crumb_end))
    return crumb;
  return styles[style].colour;
}

/* one word at x: as text, a chevron or a button glyph; returns its width */
/* one word (w, n characters; at cell c of the row, for its place in the row) at x: as text, a chevron or a
 * button glyph; returns its width */
static float text_word(const mrow_t *r, const char *w, int n, int c, int style, float x, bool draw)
{
  gf_face_t face = face_at(r, c, style);
  rgb_t colour = colour_at(r, c, style);
  float chev = r->em * 0.62f;
  if (n == 1 && (w[0] == '<' || w[0] == '>') &&
      (r->kind == GFX_ROW_TITLE || (r->kind == GFX_ROW_ITEM && c >= r->switch_from))) {
    float cw = chev * 0.56f;
    if (draw) {
      gpu_colour_t col = to_gpu(r->kind == GFX_ROW_TITLE ? styles[GFX_DIM].colour : colour, 1);
      chevron(w[0] == '<' ? LEFT : RIGHT, chev, gpu_x + x + cw / 2, gpu_y + r->baseline - icon_mid(r), col);
    }
    return cw;
  }
  int pre = 0, post = 0;
  uint32_t other = 0, glyph = 0;
  if (r->kind == GFX_ROW_HINT || r->kind == GFX_ROW_BUTTONS) {
    glyph = button_in(w, n, &pre, &post, &other);
  } else if ((r->kind == GFX_ROW_ITEM || r->kind == GFX_ROW_LINE) && !r->grid) {
    /* a button named in a label, "(R + d-up)", or a value that is one, "< L >" */
    glyph = button_in(w, n, &pre, &post, &other);
    bool closes = false;
    for (int i = n - post; i < n; i++)
      closes |= w[i] == ')';
    if (!pre && !closes && c < r->switch_from)
      glyph = 0;
  }
  if (r->kind == GFX_ROW_BUTTONS && n == 1 && (w[0] == '+' || w[0] == '-')) {
    glyph = w[0] == '+' ? GF_PLUS : GF_MINUS;
    pre = post = 0;
  }
  if (glyph) {
    float width = 0;
    if (pre) {
      if (draw)
        draw_run(w, pre, face, r->em, (int)(x * 64), r->baseline, colour);
      width += gamefont_measure(face, r->em, w, pre) / 64.0f + r->em * 0.08f;
    }
    width += button_glyph(r, glyph, x + width, draw, styles[GFX_NORMAL].colour);
    if (other) {
      if (draw)
        draw_run("/", 1, face, r->em, (int)((x + width) * 64), r->baseline, colour);
      width += gamefont_measure(face, r->em, "/", 1) / 64.0f;
      width += button_glyph(r, other, x + width, draw, styles[GFX_NORMAL].colour);
    }
    bool colon = post > 0 && w[n - post] == ':'; /* a ':' after a button is the glyph's to say */
    int shown = post - colon;
    if (colon) {
      if (draw && shown > 0)
        draw_run(w + n - post + 1, shown, face, r->em, (int)((x + width) * 64), r->baseline, colour);
      width += shown > 0 ? gamefont_measure(face, r->em, w + n - post + 1, shown) / 64.0f : 0;
    } else if (post) {
      if (draw)
        draw_run(w + n - post, post, face, r->em, (int)((x + width) * 64), r->baseline, colour);
      width += gamefont_measure(face, r->em, w + n - post, post) / 64.0f;
    }
    return width;
  }
  if (draw)
    draw_run(w, n, face, r->em, (int)(x * 64), r->baseline, colour);
  return gamefont_measure(face, r->em, w, n) / 64.0f;
}

static float menu_word(const mrow_t *r, int c, int n, int style, float x, bool draw)
{
  return text_word(r, r->panel->ch[r->row] + c, n, c, style, x, draw);
}

/* an icon cell (or two) at col: a checkbox, a button glyph or gz's art as a shape */
/* The game's picture of an item (gameimage.h's GAMEPIC_*), the part of it its picture covers fitted into a
 * max_w x max_h box centred on (cx, cy), so each looks as big as the others and none reaches past its row.
 * Drawn a few times a fraction of a pixel apart, each at 1/k of what's under it (their average): an icon
 * (shrunk to GPU_ICON_PX) a little bigger than it's shown, a picture of the shared archive much bigger, so
 * its detail doesn't break up into noise. */
#define HEART_BOX_X0 0.04f /* the heart container's covered part of its picture (300x267) */
#define HEART_BOX_Y0 0.06f
static void draw_picture(int pic, float cx, float cy, float max_w, float max_h)
{
  bool icon = pic >= GAMEIMAGES_SHARED;
  if (icon ? !gpuui_icon_ready(pic - GAMEIMAGES_SHARED) : !gpuui_image_ready(pic))
    return;
  float box[4] = { HEART_BOX_X0, HEART_BOX_Y0, 1 - HEART_BOX_X0, 1 - HEART_BOX_Y0 };
  float aspect = 300.0f / 267; /* the picture's own (an icon's are square) */
  if (icon)
    gameimage_icon_box(pic - GAMEIMAGES_SHARED, box), aspect = 1;
  float bw = (box[2] - box[0]) * aspect, bh = box[3] - box[1];
  float k_size = max_w / bw < max_h / bh ? max_w / bw : max_h / bh;
  float w = bw * k_size, h = bh * k_size;
  float x = (float)(int)(cx - w / 2 + 0.5f), y = (float)(int)(cy - h / 2 + 0.5f);
  int taps = icon ? 2 : 4, k = 1;
  for (int j = 0; j < taps; j++) {
    for (int i = 0; i < taps; i++, k++) {
      float dx = (i + 0.5f) / taps - 0.5f, dy = (j + 0.5f) / taps - 0.5f;
      gpu_colour_t c = { 1, 1, 1, gpu_fade / (float)k };
      if (icon)
        gpuui_icon(pic - GAMEIMAGES_SHARED, x + dx, y + dy, w, h, box, c);
      else
        gpuui_image(pic, x + dx, y + dy, w, h, box, c);
    }
  }
}

/* a checkbox's size in a row (menu_icon draws it at the right of its cell) */
static int checkbox_size(const mrow_t *r) { return (int)(r->em * 0.8f + 0.5f); }

static void menu_icon(const mrow_t *r, unsigned char k, float left, int cells, int style)
{
  float width = (float)cells * CELL_W * r->scale / NUI_SC_ONE;
  float mid = r->baseline - icon_mid(r);
  rgb_t colour = styles[style].colour;
  if (k == GFX_CHECK_OFF || k == GFX_CHECK_ON) {
    int size = checkbox_size(r);
    gpu_checkbox(gpu_x + left + width - size - r->em * 0.4f, gpu_y + mid - size / 2.0f, size, k == GFX_CHECK_ON, colour);
    return;
  }
  float cx = gpu_x + left + width / 2, cy = gpu_y + mid;
  int dir = k == GFX_ICON_UP ? UP : k == GFX_ICON_DOWN ? DOWN : k == GFX_ICON_LEFT ? LEFT : k == GFX_ICON_RIGHT ? RIGHT : -1;
  if (dir >= 0) {
    chevron(dir, r->em * 0.62f, cx, cy, to_gpu(colour, 1));
    return;
  }
  if (k == GFX_ICON_REMOVE || k == GFX_ICON_ADD) {
    int size = (int)(r->em * 0.5f + 0.5f);
    gpuui_shape(k == GFX_ICON_ADD ? SHAPE_PLUS : SHAPE_CROSS, size, size, paint_cross,
                k == GFX_ICON_ADD ? &directions[0] : 0, cx - size / 2.0f, cy - size / 2.0f, to_gpu(colour, 1));
    return;
  }
  if (k == GFX_ICON_WRENCH) { /* the game's gear, as big as the text's capitals */
    centred_glyph(GF_GEAR, (int)(r->em * 1.25f), cx, cy, to_gpu(colour, 1));
    return;
  }
  uint32_t symbol = symbol_for(k);
  if (symbol) {
    const gf_glyph_t *g = gamefont_glyph(GF_GLYPHS, glyph_em(r->em), symbol);
    if (g)
      button_glyph(r, symbol, left + (width - g->w) / 2, true, colour);
    return;
  }
  int h = (int)(CELL_HT * fp_px + 0.5f);
  gpu_icon(r->legacy, k, cells, gpu_x + left, gpu_y + mid - h / 2.0f, (int)(width + 0.5f), h, colour);
}

/* The game's options cursor (OptionBtnThinCursorLine / Glow, 640x173). The line's picture is a distance
 * field: a bar whose alpha climbs evenly from LINE_FROM pixels in from the picture's edge to full LINE_RAMP
 * further in. The game cuts its line out of it at a level; so does this: the alpha mapped steeply round
 * LINE_LEVEL (a crisp outer edge, where that level is), the picture cut in nine at LINE_CUT (the middle left
 * out: a crisp inner edge), and scaled so the line is CURSOR_LINE_FP thick. The glow's picture is a soft bar,
 * from nothing GLOW_FROM in to full at GLOW_FULL: cut there, it's a halo from the line out, in the game's
 * blue-white. */
#define LINE_FROM 30.0f
#define LINE_RAMP 29.0f
#define LINE_LEVEL 0.3f
#define LINE_CUT 48.0f
#define GLOW_FROM 14.0f
#define GLOW_FULL 72.0f
#define CURSOR_LINE_FP 1.6f /* the line's thickness */
#define CURSOR_GLOW_FP 3.5f /* how far the halo reaches past it */

/* where the selected part of the last row measured is drawn (menu_cells) */
static float sel_lo, sel_hi;
static void note_selected(int style, float x0, float x1)
{
  if (style == GFX_SELECTED) {
    sel_lo = x0 < sel_lo ? x0 : sel_lo;
    sel_hi = x1 > sel_hi ? x1 : sel_hi;
  }
}

/* the row's cells [from, cols) as drawn (or only measured); returns the right end of what's there */
static float menu_cells(const mrow_t *r, int from, bool draw)
{
  const gfx_panel_t *panel = r->panel;
  const char *ch = panel->ch[r->row];
  bool flow = r->kind == GFX_ROW_HINT || r->kind == GFX_ROW_TITLE; /* runs follow each other, not their cells */
  float pen = -1, end = 0;
  const gf_glyph_t *space = gamefont_glyph(GF_REGULAR, r->em, ' ');
  float space_w = space ? space->advance / 64.0f : r->em * 0.25f;
  for (int c = from; c < r->cols;) {
    unsigned char k = (unsigned char)ch[c];
    int style = style_of(panel, r->row, c);
    if (!k || k == ' ' || k == GFX_WIDE_REST || k == 0x7F || c == r->opens_at) {
      c++;
      continue;
    }
    bool list_row = r->kind == GFX_ROW_ITEM || r->kind == GFX_ROW_LINE || r->kind == GFX_ROW_HEADING;
    if (list_row && c == SCROLL_MARK_COL && (k == '^' || k == 'v') &&
        (c + 1 >= r->cols || ch[c + 1] == ' ' || !ch[c + 1])) {
      if (draw && !scroll_bar_shown(panel)) /* the scroll bar says it, once it can be drawn */
        chevron(k == '^' ? UP : DOWN, r->em * 0.55f, gpu_x + col_x(r, c) + CELL_W * fp_px / 2,
                gpu_y + r->baseline - icon_mid(r), to_gpu(styles[GFX_DIM].colour, 1));
      c++;
      continue;
    }
    if (k == GFX_PICTURE) { /* the game's picture of the item: two cells, its number in the second */
      float left = col_x(r, c), width = 2 * CELL_W * fp_px;
      int pic = c + 1 < r->cols ? GFX_PICTURE_OF(ch[c + 1]) : -1;
      /* as tall as the row leaves inside the cursor's line, less half a font pixel above and below; as wide
       * as its two cells */
      float max_h = r->h - CURSOR_LINE_FP * fp_px - fp_px;
      if (draw && pic > 0 && pic < GAMEIMAGES)
        draw_picture(pic, gpu_x + left + width / 2, gpu_y + r->h / 2, width - fp_px, max_h);
      note_selected(style, left, left + width);
      end = pen = left + width;
      c += 2;
      continue;
    }
    if (!is_text(k)) {
      int cells = c + 1 < r->cols && (unsigned char)ch[c + 1] == GFX_WIDE_REST ? 2 : 1;
      float left = col_x(r, c), width = cells * CELL_W * fp_px;
      /* after an item's value, an icon follows it (gz padded them into a column far right) */
      if (!r->grid && r->kind == GFX_ROW_ITEM && c > r->switch_from && pen >= 0)
        left = pen + r->em * 0.5f;
      if (draw)
        menu_icon(r, k, left, cells, style);
      if (k == GFX_CHECK_OFF || k == GFX_CHECK_ON) { /* the box itself, as menu_icon draws it */
        float size = (float)checkbox_size(r);
        note_selected(style, left + width - size - r->em * 0.4f, left + width - r->em * 0.4f);
      } else {
        note_selected(style, left, left + width);
      }
      end = pen = left + width;
      c += cells;
      continue;
    }
    if (r->grid) { /* each word at its cell; digits and a cursor's word a character a cell, as gz sets them */
      int we = c;
      bool hex = true, mixed = false; /* hex digits (an address, a byte); a cursor inside the word */
      for (; we < r->cols && is_text((unsigned char)ch[we]) && ch[we] != ' '; we++) {
        unsigned char q = (unsigned char)ch[we];
        hex &= (q >= '0' && q <= '9') || (q >= 'a' && q <= 'f') || (q >= 'A' && q <= 'F') || q == 'x';
        mixed |= style_of(panel, r->row, we) != style;
      }
      if (hex || mixed) {
        for (int i = c; i < we; i++) {
          int st = style_of(panel, r->row, i);
          gf_face_t f = face_at(r, i, st);
          const gf_glyph_t *g = gamefont_glyph(f, r->em, (unsigned char)ch[i]);
          if (draw && g)
            draw_run(ch + i, 1, f, r->em, (int)((col_x(r, i) + (CELL_W * fp_px - g->advance / 64.0f) / 2) * 64),
                     r->baseline, colour_at(r, i, st));
        }
        for (int i = c; i < we; i++)
          note_selected(style_of(panel, r->row, i), col_x(r, i), col_x(r, i + 1));
        end = pen = col_x(r, we);
      } else {
        float x = col_x(r, c);
        end = pen = x + menu_word(r, c, we - c, style, x, draw);
        note_selected(style, x, pen);
      }
      c = we;
      continue;
    }
    int run = run_end(panel, r->row, c, r->cols);
    if (r->opens_at > c && run > r->opens_at)
      run = r->opens_at;
    if (r->switch_from > c && run > r->switch_from) /* the value starts a run of its own, at its place */
      run = r->switch_from;
    float x = flow && pen >= 0 ? pen + r->em * 0.9f : col_x(r, c);
    for (int w = c; w < run;) {
      int we = word_end(ch, w, run);
      if (w != c)
        x = pen + space_w;
      else if (!flow && c < r->switch_from && is_numeric_run(ch + w, we - w) && align_right[r->row][w] == COLUMN_RIGHT)
        x = col_x(r, we) - gamefont_measure(face_at(r, w, style), r->em, ch + w, we - w) / 64.0f;
      pen = x + menu_word(r, w, we - w, style, x, draw);
      note_selected(style, x, pen);
      w = we < run ? we + 1 : we;
    }
    end = pen;
    c = run;
  }
  return end;
}

/* a row's type size and baseline, its cells with text and the selected ones, and its parts (a switch, the
 * chevron of a menu it opens); false if there's nothing in it */
/* the row's type size and baseline, for its kind */
/* The row's type and where its baseline is: its capitals centred in the row, as the game sets a button's
 * text (descenders hang below), or a heading's set low in it. */
static void row_metrics(mrow_t *r)
{
  int text_h = (int)(row_fp[r->kind].text * fp_px + 0.5f);
  gf_face_t face = r->kind == GFX_ROW_TITLE || r->kind == GFX_ROW_CAPTION ? GF_BOLD : GF_REGULAR;
  r->em = gamefont_em_for_height(face, text_h);
  float cap = cap_height(face, r->em);
  float above = (r->h - cap) * (r->kind == GFX_ROW_HEADING ? HEADING_DROP : r->kind == GFX_ROW_CAPTION ? CAPTION_TEXT_AT : 0.5f);
  r->baseline = (int)(above + cap + 0.5f);
}

static bool row_setup(mrow_t *r)
{
  const gfx_panel_t *panel = r->panel;
  const char *ch = panel->ch[r->row];
  row_metrics(r);
  r->switch_from = r->cols;
  r->switch_shift = 0;
  r->opens_at = -1;
  /* an item under a heading: gz indents it MENU_INDENT cells, about two of the game's letters too many; here
   * it's INDENT_EM in from the heading's start, every row of the group moved alike so their columns stay */
  r->indent_from = r->cols;
  r->indent_shift = 0;
  if ((panel->kind[r->row] & GFX_ROW_INDENT) && !r->grid) {
    r->indent_from = ITEM_INDENT_COL;
    r->indent_shift = r->em * INDENT_EM - (float)(cell_x(ITEM_INDENT_COL, r->scale) - cell_x(LIST_COL, r->scale));
  }

  int first_text = -1, last_text = -1, first_sel = -1, last_sel = -1;
  for (int c = 0; c < r->cols; c++) {
    unsigned char k = (unsigned char)ch[c];
    if (!k || k == ' ' || k == 0x7F || (c == SCROLL_MARK_COL && (k == '^' || k == 'v')))
      continue;
    if (first_text < 0)
      first_text = c;
    last_text = c;
    if (style_of(panel, r->row, c) == GFX_SELECTED) {
      if (first_sel < 0)
        first_sel = c;
      last_sel = c;
    }
  }
  r->first_text = first_text, r->last_text = last_text, r->first_sel = first_sel, r->last_sel = last_sel;
  if (first_text < 0)
    return false;
  if (r->kind == GFX_ROW_ITEM && !r->grid) {
    if (panel->kind[r->row] & GFX_ROW_OPENS) { /* its line's " >": a chevron at the right edge instead */
      for (int c = r->cols - 1; c > first_text; c--) {
        if (ch[c] == '>') {
          r->opens_at = c;
          break;
        }
        if (ch[c] && ch[c] != ' ' && ch[c] != 0x7F)
          break;
      }
    }
    for (int c = first_text + 2; c < r->cols; c++) { /* "label < value >": the value right-aligned */
      if (ch[c] == '<' && ch[c - 1] == ' ' && (c + 1 >= r->cols || ch[c + 1] == ' ')) {
        r->switch_from = c;
        break;
      }
    }
  }
  if (r->kind == GFX_ROW_WATCH && GFX_WATCH_LABEL_W + 1 < r->cols) /* the value right-aligned */
    r->switch_from = GFX_WATCH_LABEL_W + 1;
  return true;
}

/* --- the cursor's drawing and motion ------------------------------------------------------------------- */

/* the cursor round (x0, y0)-(x1, y1) (its line's middle), its line `thin` thinner (0: CURSOR_LINE_FP thick)
 * and its glow `glow_thin` smaller, at `alpha`, its glow at `glow` */
static void cursor_shape(float x0, float y0, float x1, float y1, float thin, float glow_thin, float alpha, float glow)
{
  float line = CURSOR_LINE_FP * fp_px * (1 - thin), half = line / 2;
  x0 -= half, y0 -= half, x1 += half, y1 += half; /* its outer edge */
  /* the glow: its full part from the line's outer edge in, its halo CURSOR_GLOW_FP out */
  float gpx = CURSOR_GLOW_FP * fp_px * (1 - glow_thin) / (GLOW_FULL - GLOW_FROM), g = GLOW_FULL * gpx;
  if (glow > 0 && gpx > 0)
    picture_frame(GAMEIMAGE_CURSOR_GLOW, x0 - g, y0 - g, x1 - x0 + 2 * g, y1 - y0 + 2 * g, GLOW_FULL, gpx, 640, 173,
                  (gpu_colour_t){ 0.80f, 0.87f, 1, 0.5f * glow * alpha * gpu_fade });
  /* the line: LINE_LEVEL's place at its outer edge, the cut `line` in; the alpha climbing over a screen pixel
   * there */
  if (line < 0.5f)
    return;
  float level_at = LINE_FROM + LINE_LEVEL * LINE_RAMP, lpx = line / (LINE_CUT - level_at);
  float l = level_at * lpx, k = LINE_RAMP * lpx;
  const gpu_colour_t map[2] = { { 1, 1, 1, -k * LINE_LEVEL }, { 1, 1, 1, k * (1 - LINE_LEVEL) } };
  picture_frame_mapped(GAMEIMAGE_CURSOR_LINE, x0 - l, y0 - l, x1 - x0 + 2 * l, y1 - y0 + 2 * l, LINE_CUT, lpx, 640, 173,
                       (gpu_colour_t){ 1, 1, 1, alpha * gpu_fade }, map);
}

/* The cursor's motion, after the game's options cursor (PaOptionBtnL_00's animations, in frames of 1/60 s).
 * On a new item it shows at once and fades in over 4 frames as its line and glow grow from 0.4 and 0.6 thinner
 * to full and its glow brightens from 0.74 over 8 (Select); the cursor it left fades out over 8 frames as they
 * thin again over 14 (UnSelect); while it stays it breathes on a 40-frame loop, its glow dimming to 0.88 and
 * widening by 1/8 and back (CursorLoop). Each curve is the game's: a cubic between its keys, with their
 * slopes. */
#define FRAME_MS (1000.0f / 60)
#define SELECT_FRAMES 8
#define UNSELECT_FRAMES 14
#define LOOP_FRAMES 40
#define CURSOR_GONE_MS 400 /* not drawn this long: the menu was closed, and its cursor shows at once */

static struct {
  bool on, whole; /* whole: round a whole item (its text changes with it, menu_row) */
  float x0, y0, x1, y1;
  uint64_t since;
} cursor_now, cursor_was;
static uint64_t cursor_seen; /* when the cursor was last drawn (sys_tick) */
static bool cursor_jump;     /* the menu changed: the next cursor isn't a move from the last */
static bool cursor_still;    /* drawing the menu going out (draw_menu_moving): its cursor as it was */

static float frames_since(uint64_t then)
{
  return (float)(sys_tick() - then) * 1000.0f / (float)sys_tick_freq() / FRAME_MS;
}

/* a curve between two keys at frames 0 and n: its values and slopes (per frame) there, at frame f */
static float hermite(float f, float n, float v0, float s0, float v1, float s1)
{
  float t = clampf(f / n, 0, 1), t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * s0 * n + (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * s1 * n;
}

/* the cursor this frame, round (x0, y0)-(x1, y1): drawn as it moves in, the one it left as it goes */
static void cursor_at(float x0, float y0, float x1, float y1, bool whole)
{
  if (cursor_still) {
    cursor_shape(x0, y0, x1, y1, 0, 0, 1, 1);
    return;
  }
  uint64_t now = sys_tick();
  /* drawn a moment ago (the menu stayed open; frames can be 100 ms apart when the game is slow) */
  bool seen = cursor_seen && (float)(now - cursor_seen) * 1000.0f / (float)sys_tick_freq() < CURSOR_GONE_MS;
  bool moved = !cursor_now.on || __builtin_fabsf(x0 - cursor_now.x0) > 1 || __builtin_fabsf(y0 - cursor_now.y0) > 1 ||
               __builtin_fabsf(x1 - cursor_now.x1) > 1 || __builtin_fabsf(y1 - cursor_now.y1) > 1;
  if (moved) {
    if (seen && cursor_now.on && !cursor_jump) { /* the one it left goes */
      cursor_was = cursor_now;
      cursor_was.since = now;
    } else {
      cursor_was.on = false;
    }
    /* a move (or into another menu): from Select's start; the menu just opened: past it, breathing */
    uint64_t past = (uint64_t)(SELECT_FRAMES * FRAME_MS / 1000 * (float)sys_tick_freq());
    cursor_now = (__typeof__(cursor_now)){ true, whole, x0, y0, x1, y1, seen || cursor_jump ? now : now - past };
  }
  cursor_jump = false;
  cursor_seen = now;
  /* Select, then CursorLoop */
  float f = frames_since(cursor_now.since);
  float alpha = hermite(f, 4, 0, 0, 1, 0);
  float thin = hermite(f, SELECT_FRAMES, 0.4f, 0, 0, 0), glow_thin = hermite(f, SELECT_FRAMES, 0.6f, 0, 0, 0);
  float glow = hermite(f, SELECT_FRAMES, 0.74f, 0.0724f, 1, -0.0017f);
  if (f >= SELECT_FRAMES) { /* breathing: half a loop out, half back */
    float l = f - SELECT_FRAMES;
    l -= (float)(int)(l / LOOP_FRAMES) * LOOP_FRAMES;
    float k = l < LOOP_FRAMES / 2 ? hermite(l, LOOP_FRAMES / 2, 0, 0, 1, 0) : hermite(l - LOOP_FRAMES / 2, LOOP_FRAMES / 2, 1, 0, 0, 0);
    glow = 1 - 0.12f * k;
    glow_thin = -0.125f * k;
  }
  cursor_shape(x0, y0, x1, y1, thin, glow_thin, alpha, glow);
}

/* the cursor the menu left, as it goes (UnSelect); after the menu's rows */
static void cursor_leaving(void)
{
  if (!cursor_was.on)
    return;
  float f = frames_since(cursor_was.since);
  if (f >= UNSELECT_FRAMES) {
    cursor_was.on = false;
    return;
  }
  float alpha = hermite(f, 8, 1, 0, 0, 0);
  float thin = hermite(f, UNSELECT_FRAMES, 0, 0, 0.4f, 0), glow_thin = hermite(f, UNSELECT_FRAMES, 0, 0, 0.6f, 0);
  cursor_shape(cursor_was.x0, cursor_was.y0, cursor_was.x1, cursor_was.y1, thin, glow_thin, alpha, 1);
}

/* the row's text and icons, drawn in style `force` (its normal and selected cells; -1: as they are) at alpha,
 * scaled by s about (ox, oy); returns where they end */
#define TEXT_GROW 0.04f
static float row_text(mrow_t *r, int force, float alpha, float s, float ox, float oy)
{
  if (alpha <= 0)
    return 0;
  float fade = gpu_fade;
  gpu_fade *= alpha;
  style_force = force;
  if (gpu && s != 1)
    gpuui_scale(s, ox, oy);
  float end = menu_cells(r, 0, true);
  if (r->opens_at >= 0) {
    int style = style_of(r->panel, r->row, r->opens_at);
    float a = style == GFX_SELECTED ? 1 : 0.55f;
    chevron(RIGHT, r->em * 0.62f, gpu_x + r->right - r->em * 0.2f, gpu_y + r->baseline - icon_mid(r),
            to_gpu(styles[style].colour, a));
  }
  if (gpu && s != 1)
    gpuui_scale(1, 0, 0);
  style_force = -1;
  gpu_fade = fade;
  return end;
}

static void menu_row(mrow_t *r)
{
  if (!row_setup(r))
    return;
  int first_text = r->first_text, last_text = r->last_text, first_sel = r->first_sel, last_sel = r->last_sel;
  if ((r->kind == GFX_ROW_ITEM || r->kind == GFX_ROW_WATCH) && r->switch_from < r->cols) {
    /* the value right-aligned: at the menu's value column, else at the right edge */
    float natural = menu_cells(r, r->switch_from, false);
    bool column = r->value_right > 0 && r->kind == GFX_ROW_ITEM; /* the column is after every label */
    float shift = (column ? r->value_right : r->right) - natural;
    /* a watch's too, leftwards: its panel is as wide as its labels, a gap and its widest value (watch_widths) */
    r->switch_shift = column || shift > 0 || r->kind == GFX_ROW_WATCH ? shift : 0;
  }

  /* the selection: across the row when the whole item is selected, else under its selected part; the
   * game's own options cursor (a soft bar and its glow) once its pictures are there */
  if (first_sel >= 0) {
    /* where the selected part is drawn, which needn't be its cells: an icon after a right-aligned value (a
     * watch's sorting arrows) follows the value */
    sel_lo = 1e9f, sel_hi = -1e9f;
    menu_cells(r, 0, false);
    if (sel_hi < sel_lo)
      sel_lo = col_x(r, first_sel), sel_hi = col_x(r, last_sel + 1);
  }
  if (first_sel >= 0 && gpuui_image_ready(GAMEIMAGE_CURSOR_GLOW) && gpuui_image_ready(GAMEIMAGE_CURSOR_LINE)) {
    /* the game's cursor: a white frame round the item (or the part of it selected), its glow outside */
    bool whole = first_sel == first_text && last_sel == last_text;
    /* the line's middle runs on the row's edges, and round what's selected (across to the row's end when it's
     * the whole item) with the same room left and right as the row leaves above and below its capitals inside
     * the line, as the game's cursor sits round a control; (x0, y0)-(x1, y1) is its outer edge */
    float line = CURSOR_LINE_FP * fp_px;
    float room = (r->h - line - cap_height(GF_REGULAR, r->em)) / 2;
    room = room > fp_px ? room : fp_px;
    float half = line / 2; /* (x0, y0)-(x1, y1): the line's middle */
    float x0 = gpu_x + sel_lo - room - half, x1 = gpu_x + (whole ? r->right : sel_hi) + room + half;
    cursor_at(x0, gpu_y, x1, gpu_y + r->h, whole);
  } else if (first_sel >= 0) {
    bool whole = first_sel == first_text && last_sel == last_text;
    float from = whole ? (float)cell_x(1, r->scale) - 6 * fp_px : sel_lo - 3 * fp_px;
    float to = whole ? r->right + 10 * fp_px : sel_hi + 3 * fp_px;
    float y = gpu_y, h = r->h, line = fp_px * 0.5f < 1 ? 1 : (float)(int)(fp_px * 0.5f + 0.5f);
    faded_strip(gpu_x + from, gpu_x + to, y, h, whole ? 0.08f : 0.25f, whole ? 0.4f : 0.25f, band_light, 0.16f);
    faded_strip(gpu_x + from, gpu_x + to, y, line, whole ? 0.08f : 0.25f, whole ? 0.4f : 0.25f, band_light, 0.45f);
    faded_strip(gpu_x + from, gpu_x + to, y + h - line, line, whole ? 0.08f : 0.25f, whole ? 0.4f : 0.25f, band_light,
                0.45f);
  }

  /* the item's text as the cursor comes (or as it goes): its new look fading in as it grows from
   * 1 - TEXT_GROW to full, the old one fading out (Select's and UnSelect's timing) */
  float end = 0, f_in = -1, f_out = -1;
  bool whole_sel = first_sel >= 0 && first_sel == first_text && last_sel == last_text;
  if (gpu && !cursor_still && whole_sel && cursor_now.on && cursor_now.whole && __builtin_fabsf(gpu_y - cursor_now.y0) < 1)
    f_in = frames_since(cursor_now.since);
  else if (gpu && !cursor_still && first_sel < 0 && cursor_was.on && cursor_was.whole &&
           __builtin_fabsf(gpu_y - cursor_was.y0) < 1)
    f_out = frames_since(cursor_was.since);
  float ox = gpu_x + col_x(r, first_text), oy = gpu_y + r->h / 2;
  if (f_in >= 0 && f_in < SELECT_FRAMES) {
    float a = hermite(f_in, 4, 0, 0, 1, 0);
    row_text(r, GFX_NORMAL, 1 - a, 1, ox, oy);
    end = row_text(r, -1, a, hermite(f_in, SELECT_FRAMES, 1 - TEXT_GROW, 0, 1, 0), ox, oy);
  } else if (f_out >= 0 && f_out < SELECT_FRAMES) {
    float a = hermite(f_out, SELECT_FRAMES, 1, 0, 0, 0);
    end = row_text(r, -1, 1 - a, 1, ox, oy);
    row_text(r, GFX_SELECTED, a, hermite(f_out, SELECT_FRAMES, 1, 0, 1 - TEXT_GROW, 0), ox, oy);
  } else {
    end = row_text(r, -1, 1, 1, ox, oy);
  }
  if (r->kind == GFX_ROW_HEADING && end + r->em * 0.6f < r->right) { /* a rule from the heading on */
    float line = fp_px * 0.5f < 1 ? 1 : (float)(int)(fp_px * 0.5f + 0.5f);
    float y = gpu_y + r->baseline - r->em * 0.3f;
    faded_strip(gpu_x + end + r->em * 0.6f, gpu_x + r->right + 8 * fp_px, y, line, 0.02f, 0.6f, band_light, 0.24f);
  }
}

/* --- the hint, wrapped by its own type ------------------------------------------------------------ */

#define HINT_COL 2   /* menu.c's ITEM_COL */
#define HINT_LINES 8
static struct { int from, to; float indent; } hint_at[HINT_LINES];
static int hint_count;

/* the words of s[from, to) from x, drawn or only measured (2+ spaces between words: a wider gap, as gz's
 * hints part their instructions); returns where they end */
static float hint_words(const mrow_t *r, const char *s, int from, int to, float x, bool draw)
{
  const gf_glyph_t *space = gamefont_glyph(GF_REGULAR, r->em, ' ');
  float sp = space ? space->advance / 64.0f : r->em * 0.25f;
  bool first = true;
  for (int i = from; i < to;) {
    int spaces = 0;
    for (; i < to && s[i] == ' '; i++)
      spaces++;
    if (i >= to)
      break;
    int e = i;
    while (e < to && s[e] != ' ')
      e++;
    if (!first)
      x += spaces >= 2 ? r->em * 0.9f : sp;
    x += text_word(r, s + i, e - i, 0, GFX_DIM, x, draw);
    first = false;
    i = e;
  }
  return x;
}

/* the hint broken into lines `width` wide: at its own line ends, else between words; a line's indent is
 * kept on the lines it wraps to */
static void wrap_hint(const mrow_t *r, const char *s, float width)
{
  const gf_glyph_t *space = gamefont_glyph(GF_REGULAR, r->em, ' ');
  float sp = space ? space->advance / 64.0f : r->em * 0.25f;
  int len = (int)__builtin_strlen(s);
  hint_count = 0;
  for (int i = 0; i <= len && hint_count < HINT_LINES;) {
    int end = i;
    while (end < len && s[end] != '\n')
      end++;
    int indent = 0;
    while (i + indent < end && s[i + indent] == ' ')
      indent++;
    int start = i + indent;
    do {
      int best = start;
      for (int we = start; we < end;) { /* as many words as fit */
        while (we < end && s[we] == ' ')
          we++;
        while (we < end && s[we] != ' ')
          we++;
        if (best > start && hint_words(r, s, start, we, 0, false) > width - indent * sp)
          break;
        best = we;
      }
      hint_at[hint_count].from = start, hint_at[hint_count].to = best, hint_at[hint_count].indent = indent * sp;
      hint_count++;
      for (start = best; start < end && s[start] == ' ';)
        start++;
    } while (start < end && hint_count < HINT_LINES);
    if (end >= len)
      break;
    i = end + 1;
  }
}

/* --- measuring the menu: its value column and how wide it needs to be ------------------------------ */

/* the title rows: where the breadcrumb's path ends and the menu's own name starts */
static void find_crumbs(mrow_t *r)
{
  const gfx_panel_t *panel = r->panel;
  r->crumb_end = 0;
  r->last_title_row = -1;
  for (int row = 0; row < GFX_ROWS && kind_of(panel, row) == GFX_ROW_TITLE; row++)
    r->last_title_row = row;
  if (r->last_title_row >= 0) { /* after the last " > " of the breadcrumb */
    const char *ch = panel->ch[r->last_title_row];
    for (int c = 1; c < r->cols; c++)
      if (ch[c] == '>' && ch[c - 1] == ' ')
        r->crumb_end = c + 1;
  }
}

/* the menu's value column (the switches right-aligned at it, after the longest label with a gap; 0 when
 * there are none) and the right end of everything in it, from the grid's left edge. Every row is measured
 * as the cursor's bold sets it, wherever the cursor is, so the column and the menu's width stay put as it
 * moves (measured as drawn, they followed whichever row was bold). */
static void menu_layout(const gfx_panel_t *panel, int cols, int scale, float *value_right, float *content)
{
  int force = style_force;
  style_force = GFX_SELECTED;
  mrow_t r = { .panel = panel, .cols = cols, .scale = scale };
  find_crumbs(&r);
  int rows = panel->rows < GFX_ROWS ? panel->rows : GFX_ROWS;
  float labels = 0, values = 0, ends = 0, gap = 0;
  for (int row = 0; row < rows; row++) {
    r.row = row;
    r.kind = kind_of(panel, row);
    r.grid = (panel->kind[row] & GFX_ROW_GRID) != 0;
    r.h = row_height_fp(panel, row) * fp_px;
    r.cols = cols;
    if (r.kind != GFX_ROW_TITLE && r.kind != GFX_ROW_HEADING && r.kind != GFX_ROW_ITEM && r.kind != GFX_ROW_LINE)
      continue;
    if (!row_setup(&r))
      continue;
    if (r.kind == GFX_ROW_ITEM && r.switch_from < cols) {
      int from = r.switch_from;
      r.cols = from; /* the label alone */
      float label = menu_cells(&r, 0, false);
      r.cols = cols;
      float value = menu_cells(&r, from, false) - col_x(&r, from);
      labels = label > labels ? label : labels;
      values = value > values ? value : values;
      gap = r.em * 1.5f;
      continue;
    }
    float end = menu_cells(&r, 0, false) + (r.opens_at >= 0 ? r.em * 1.4f : 0); /* room for its chevron */
    ends = end > ends ? end : ends;
  }
  style_force = force;
  *value_right = values > 0 ? labels + gap + values : 0;
  *content = ends > *value_right ? ends : *value_right;
}

/* the last menu measured (nativeui_gpu_size, then nativeui_gpu_panel in the same frame) */
static const gfx_panel_t *measured_panel;
static uint32_t measured_hash;
static float measured_value_right;

/* the controls at the menu's foot, from x: L select, R + d-left return, R + right stick down close; drawn
 * or only measured; returns where they end */
static float draw_controls(mrow_t *r, float x, bool draw)
{
  int text_h = (int)(row_fp[GFX_ROW_HINT].text * fp_px + 0.5f);
  r->kind = GFX_ROW_HINT;
  r->em = gamefont_em_for_height(GF_REGULAR, text_h);
  float cap = cap_height(GF_REGULAR, r->em);
  r->baseline = (int)((r->h + cap) / 2 + 0.5f);
  static const struct { uint32_t first, second; const char *what; } controls[] = {
    { GF_BTN_L, 0, "select" }, { GF_BTN_R, GF_DPAD_LEFT, "return" }, { GF_BTN_R, GF_RSTICK_DOWN, "close" },
  };
  rgb_t dim = styles[GFX_DIM].colour, bright = styles[GFX_NORMAL].colour;
  float gap = r->em * 0.3f;
  float end = x;
  for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) { /* "what [button]", as the game's guide */
    const char *what = T(controls[i].what);
    int n = (int)__builtin_strlen(what);
    if (draw)
      draw_run(what, n, GF_REGULAR, r->em, (int)(x * 64), r->baseline, dim);
    x += gamefont_measure(GF_REGULAR, r->em, what, n) / 64.0f + gap;
    x += button_glyph(r, controls[i].first, x, draw, bright);
    if (controls[i].second) {
      x += gap;
      if (draw)
        draw_run("+", 1, GF_REGULAR, r->em, (int)(x * 64), r->baseline, dim);
      x += gamefont_measure(GF_REGULAR, r->em, "+", 1) / 64.0f + gap;
      x += button_glyph(r, controls[i].second, x, draw, bright);
    }
    end = x;
    x += r->em * 1.3f;
  }
  return end;
}

#define CONTROLS_COL 2 /* menu.c's ITEM_COL */

/* The ground a panel sits on, around gz's grid of it (font pixels): how far it reaches past the grid on
 * each side, and how far it fades on each side. The menu's is its box (draw_box: no fade, BOX_PAD_X / _Y
 * round what's in it); the others' a band as the game's control guide (CtrlGuideBG): fading out left and
 * right, its top and bottom straight, its full part taking the content with a padding all round
 * (BAND_PAD_X / _Y), nothing drawn in a fade. The grid's own margin (a cell left and right, gz's half row
 * above and below) counts towards it; the menu's selection reaches SELECT_OVER past its last cell. */
typedef struct { float l, r, t, b, fl, fr, ft, fb; } band_ext_t;

#define BAND_PAD_X 10
#define BAND_PAD_Y 6

static band_ext_t band_of(const gfx_panel_t *panel)
{
  if (panel->clear)
    return (band_ext_t){ 0 };
  if (!panel->pad && laid_out(panel) && is_menu(panel))
    return (band_ext_t){ BOX_PAD_X - CELL_W, SELECT_OVER + BOX_PAD_X - CELL_W, MENU_EDGE_Y, MENU_EDGE_Y, 0, 0, 0, 0 };
  float fl = 1.5f * CELL_W, fr = 2 * CELL_W;
  float top = !panel->pad && laid_out(panel) ? LIST_PAD_FP : TEXT_TOP_FP + panel->yoff;
  float pad_y = BAND_PAD_Y > top ? BAND_PAD_Y - top : 0; /* what the grid's own top margin doesn't give */
  return (band_ext_t){ BAND_PAD_X + fl - CELL_W, BAND_PAD_X + fr - CELL_W, pad_y, pad_y, fl, fr, 0, 0 };
}

static float pad_picture_width(const gfx_panel_t *panel, int rows, int scale);

/* The watches' labels' end and their values' widest, and the gap between (as menu_layout's). The values'
 * widest is kept while the same labels are shown at the same size, so the band holds still as they change. */
static void watch_widths(const gfx_panel_t *panel, int cols, int scale, float *labels, float *gap, float *values)
{
  static uint32_t kept_key;
  static float kept_values;
  mrow_t r = { .panel = panel, .cols = cols, .scale = scale };
  find_crumbs(&r);
  int rows = panel->rows < GFX_ROWS ? panel->rows : GFX_ROWS;
  uint32_t key = 2166136261u ^ (uint32_t)scale;
  *labels = *gap = *values = 0;
  for (int row = 0; row < rows; row++) {
    r.row = row;
    r.kind = kind_of(panel, row);
    r.grid = (panel->kind[row] & GFX_ROW_GRID) != 0;
    r.h = row_height_fp(panel, row) * fp_px;
    r.cols = cols;
    if (r.kind != GFX_ROW_WATCH || !row_setup(&r) || r.switch_from >= cols)
      continue;
    int from = r.switch_from;
    for (int c = 0; c < from; c++)
      key = (key ^ (uint8_t)panel->ch[row][c]) * 16777619u;
    r.cols = from; /* the label alone */
    float label = menu_cells(&r, 0, false);
    r.cols = cols;
    float value = menu_cells(&r, from, false) - col_x(&r, from);
    *labels = label > *labels ? label : *labels;
    *values = value > *values ? value : *values;
    *gap = r.em * 1.5f;
  }
  if (key != kept_key)
    kept_key = key, kept_values = 0;
  kept_values = *values > kept_values ? *values : kept_values;
  *values = kept_values;
}

bool nativeui_gpu_size(const gfx_panel_t *panel, int rows, int scale, int *w, int *h)
{
  fp_px = (float)scale / NUI_SC_ONE;
  band_ext_t e = band_of(panel);
  if (!laid_out(panel) || panel->pad) { /* gz's rows: the grid's size, and the band round it */
    float picture = panel->pad ? pad_picture_width(panel, rows, scale) : 0;
    if (picture > 0) /* the input display on the game's picture of its controller: as wide as that */
      *w = (int)(picture + 0.999f);
    *w += (int)((e.l + e.r) * fp_px + 0.999f);
    *h += (int)((e.t + e.b) * fp_px + 0.999f);
    return true;
  }
  if (is_menu(panel)) {
    /* as wide as what's in it (its values in a column after its labels), and its controls; and at least
     * 0.7 of gz's width, which holds its longest hint in the rows menu.c keeps for it (the game's type is
     * narrower than gz's cells) */
    int cols = panel->cols < GFX_COLS ? panel->cols : GFX_COLS;
    float value_right, content;
    menu_layout(panel, cols, scale, &value_right, &content);
    float need = content, least = 0.7f * (cols - HINT_COL) * CELL_W * fp_px + cell_x(HINT_COL, scale);
    need = need > least ? need : least;
    if (shows_controls(panel)) {
      mrow_t r = { .panel = panel, .scale = scale, .h = GUIDE_FP * fp_px };
      float controls = draw_controls(&r, (float)cell_x(CONTROLS_COL, scale), false);
      need = need > controls ? need : controls;
    }
    *w = (int)(need + CELL_W * fp_px + 0.999f); /* the grid's right margin after it: `right` is at need */
    measured_panel = panel;
    measured_hash = panel->hash;
    measured_value_right = value_right;
  } else { /* the watches: as wide as their labels, a gap and their widest value (right-aligned at the end) */
    int cols = panel->cols < GFX_COLS ? panel->cols : GFX_COLS;
    float labels, gap, values;
    watch_widths(panel, cols, scale, &labels, &gap, &values);
    if (values > 0)
      *w = (int)(labels + gap + values + CELL_W * fp_px + 0.999f);
    /* ... and at least as wide as their caption */
    mrow_t r = { .panel = panel, .cols = cols, .scale = scale };
    for (int row = 0; row < rows && row < GFX_ROWS; row++) {
      r.row = row;
      r.kind = kind_of(panel, row);
      r.h = row_height_fp(panel, row) * fp_px;
      if (r.kind != GFX_ROW_CAPTION || !row_setup(&r))
        continue;
      int need = (int)(menu_cells(&r, 0, false) + CELL_W * fp_px + 0.999f);
      *w = need > *w ? need : *w;
    }
  }
  rows = rows < GFX_ROWS ? rows : GFX_ROWS;
  *h = (int)((menu_height_fp(panel, rows) + e.t + e.b) * fp_px + 0.999f);
  *w += (int)((e.l + e.r) * fp_px + 0.999f);
  return true;
}

/* --- a panel ------------------------------------------------------------------------------------- */

/* a ring (arg NULL) or a disc (arg set) filling its square */
static void paint_round(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  float radius = w / 2.0f - 0.5f, line = w / 12.0f > 1.2f ? w / 12.0f : 1.2f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float dx = x + 0.5f - w / 2.0f, dy = y + 0.5f - h / 2.0f;
      float d = root(dx * dx + dy * dy) - radius; /* negative inside */
      cov[y * stride + x] = coverage_of(arg ? 0.5f - d : line / 2 + 0.5f - __builtin_fabsf(d + line / 2));
    }
  }
}

enum { SHAPE_RING = 0x20, SHAPE_DISC };

/* a button glyph centred on (cx, cy) */
static void centred_glyph(uint32_t cp, int gem, float cx, float cy, gpu_colour_t colour)
{
  const gf_glyph_t *g = gamefont_glyph(GF_GLYPHS, gem, cp);
  if (!g)
    return;
  float left = cx - g->w / 2.0f - g->x0, base = cy - (g->y0 + g->h / 2.0f);
  gpuui_glyph(GF_GLYPHS, gem, cp, left + 1, base + 1, (gpu_colour_t){ 0, 0, 0, 0.5f * colour.a });
  gpuui_glyph(GF_GLYPHS, gem, cp, left, base, colour);
}

/* the controller's buttons the input display shows by name (watches.c's pad_buttons), and their glyphs */
enum { PAD_ZL, PAD_ZR, PAD_L, PAD_R, PAD_A, PAD_B, PAD_X, PAD_Y, PAD_PLUS, PAD_MINUS, PAD_NAMES };
static const struct { const char *name; uint32_t glyph; } pad_names[PAD_NAMES] = {
  [PAD_ZL] = { "ZL", GF_BTN_ZL }, [PAD_ZR] = { "ZR", GF_BTN_ZR }, [PAD_L] = { "L", GF_BTN_L },
  [PAD_R] = { "R", GF_BTN_R }, [PAD_A] = { "A", GF_BTN_A }, [PAD_B] = { "B", GF_BTN_B }, [PAD_X] = { "X", GF_BTN_X },
  [PAD_Y] = { "Y", GF_BTN_Y }, [PAD_PLUS] = { "+", GF_PLUS }, [PAD_MINUS] = { "-", GF_MINUS },
};

/* the buttons' names in the panel, above its readouts row: where each is (its cells' middle, font pixels
 * from the text's top-left; x < 0: not there) and whether it's held (gz draws the others faint) */
typedef struct { float x[PAD_NAMES], y[PAD_NAMES]; bool held[PAD_NAMES]; } pad_names_t;

static void find_pad_names(const gfx_panel_t *panel, int cols, int rows, pad_names_t *out)
{
  for (int i = 0; i < PAD_NAMES; i++)
    out->x[i] = -1, out->held[i] = false;
  for (int row = 0; row + 1 < rows; row++) {
    const char *ch = panel->ch[row];
    for (int c = 0; c < cols;) {
      int e = c; /* a name: its text cells (gz puts nothing, not spaces, between the names) */
      while (e < cols && is_text((unsigned char)ch[e]) && ch[e] != ' ')
        e++;
      for (int i = 0; i < PAD_NAMES && e > c; i++) {
        int k = 0;
        while (c + k < e && pad_names[i].name[k] == ch[c + k])
          k++;
        if (c + k == e && !pad_names[i].name[k]) {
          out->x[i] = (c + e) * CELL_W / 2.0f;
          out->y[i] = row * CELL_HT + FONT_H / 2.0f;
          out->held[i] = style_of(panel, row, c) != GFX_FAINT;
        }
      }
      c = e > c ? e : c + 1;
    }
  }
}

/* Where things are in the game's pictures of its controllers, in their pixels (1160 x 680, measured on the
 * pictures as the GPU draws them): the part shown; the sticks' caps (their middles, radius to the outside of
 * their outline, the outline's width, the grey under them, how far they move at full tilt); the d-pad (four
 * buttons, or the Pro Controller's cross: its middle, half its arms' width inside the outline, their length);
 * A, B, X, Y, - and +; the shoulder buttons L and R (the Pro Controller's: a band each, the left one, the
 * right mirrored about the picture's middle; the Joy-Cons' are a thin line at the top edge, so theirs are
 * glyphs above the picture, at `shoulder`); ZL and ZR aren't seen from the front, so they're glyphs. */
typedef struct { int16_t x, y, r; } spot_t;
typedef struct { float x, y; } point_t;
static const point_t pro_l_band[] = { /* the top edge left to right, then the bottom edge back */
  { 255, 106 }, { 270, 86 }, { 285, 75 }, { 300, 67 }, { 315, 61 }, { 330, 56 }, { 345, 53 }, { 360, 51 },
  { 375, 49 }, { 390, 47 }, { 405, 46 }, { 420, 48 }, { 435, 54 }, { 452, 62 },
  { 452, 64 }, { 435, 65 }, { 420, 66 }, { 405, 67 }, { 390, 68 }, { 375, 70 }, { 360, 72 }, { 345, 74 },
  { 330, 78 }, { 315, 82 }, { 300, 87 }, { 285, 94 }, { 270, 102 }, { 255, 111 },
};
static const struct {
  int16_t crop[4];
  float stick_x[2], stick_y[2], stick_r, stick_line, travel;
  uint8_t under_stick;
  bool cross;                 /* the d-pad is a cross: cross_x, cross_y, its arms' half width and length */
  float cross_x, cross_y, arm_half, arm_len;
  spot_t dpad[4], abxy[4], minus, plus;
  bool bands;                 /* L and R lit on the picture (pro_l_band) */
  int16_t shoulder[2];
} pictures[GAMEIMAGES] = {
  [GAMEIMAGE_PRO] = { { 150, 30, 1010, 660 }, { 348.5f, 683.5f }, { 227.3f, 347.7f }, 51.5f, 2.0f, 17, 59,
                      true, 455.5f, 347.75f, 21.5f, 66,
                      { { 0 } },
                      { { 868, 228, 33 }, { 800, 288, 33 }, { 800, 168, 33 }, { 732, 228, 33 } },
                      { 478, 160, 18 }, { 683, 160, 18 }, true, { 353, 807 } },
  [GAMEIMAGE_GRIP] = { { 195, 25, 965, 665 }, { 364.7f, 792 }, { 196.8f, 351.8f }, 47.5f, 1.6f, 14, 92,
                       false, 0, 0, 0, 0,
                       { { 368, 305, 22 }, { 366, 392, 22 }, { 320, 350, 22 }, { 410, 352, 22 } },
                       { { 838, 196, 22 }, { 792, 238, 22 }, { 792, 152, 22 }, { 745, 195, 22 } },
                       { 425, 108, 14 }, { 738, 106, 14 }, false, { 360, 800 } },
  [GAMEIMAGE_CONSOLE] = { { 25, 90, 1125, 585 }, { 110.5f, 1046.5f }, { 226, 343 }, 36, 1.3f, 11, 92,
                          false, 0, 0, 0, 0,
                          { { 112, 308, 18 }, { 112, 376, 18 }, { 75, 343, 18 }, { 146, 342, 18 } },
                          { { 1078, 225, 18 }, { 1045, 258, 18 }, { 1045, 190, 18 }, { 1012, 225, 18 } },
                          { 152, 160, 10 }, { 1005, 157, 10 }, false, { 100, 1050 } },
};
#define PICTURE_W 1160.0f
#define PICTURE_H 680.0f
#define HELD_ALPHA 0.7f

enum { SHAPE_ARM = 0x30 /* + direction */, SHAPE_BAND = 0x38 /* + side */, SHAPE_CAP_RING = 0x40 /* + picture */ };

/* where the picture is drawn: its crop's top-left at (px, py), k screen pixels per picture pixel */
typedef struct { int dev; float px, py, k; } placed_t;

static float on_x(const placed_t *p, float x) { return p->px + (x - pictures[p->dev].crop[0]) * p->k; }
static float on_y(const placed_t *p, float y) { return p->py + (y - pictures[p->dev].crop[1]) * p->k; }

/* a round button lit up: a disc over it */
static void light(const placed_t *p, spot_t at)
{
  int d = (int)(2 * at.r * p->k + 0.5f);
  if (d < 3)
    d = 3;
  gpuui_shape(SHAPE_DISC, d, d, paint_round, &directions[0], on_x(p, at.x) - d / 2.0f, on_y(p, at.y) - d / 2.0f,
              to_gpu(styles[GFX_NORMAL].colour, HELD_ALPHA));
}

/* a d-pad arm: a rectangle rounded at its far end (arg: its direction) */
static bool inside_arm(float x, float y, int w, int h, const void *arg)
{
  int dir = *(const int *)arg;
  float across = dir == UP || dir == DOWN ? (float)w : (float)h, r = across * 0.2f;
  /* in the arm's own terms: u along it from its far end, v across it */
  float u = dir == UP ? y : dir == DOWN ? h - y : dir == LEFT ? x : w - x;
  float v = dir == UP || dir == DOWN ? x : y;
  if (u >= r || (v >= r && v <= across - r))
    return true;
  float cv = v < r ? r : across - r, du = u - r, dv = v - cv;
  return du * du + dv * dv <= r * r;
}
static void paint_arm(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  paint_sampled(cov, w, h, stride, inside_arm, arg);
}

/* a band of the Pro Controller's shoulder, as placed (arg: the band's placing) */
typedef struct { float x0, y0, k; bool mirror; } band_at_t;
static bool inside_band(float x, float y, int w, int h, const void *arg)
{
  (void)h;
  const band_at_t *b = arg;
  float px = b->x0 + (b->mirror ? w - x : x) / b->k, py = b->y0 + y / b->k; /* back to the left band's pixels */
  bool in = false;
  int n = (int)(sizeof(pro_l_band) / sizeof(pro_l_band[0]));
  for (int i = 0, j = n - 1; i < n; j = i++) { /* even-odd */
    point_t a = pro_l_band[i], c = pro_l_band[j];
    if ((a.y > py) != (c.y > py) && px < (c.x - a.x) * (py - a.y) / (c.y - a.y) + a.x)
      in = !in;
  }
  return in;
}
static void paint_band(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  paint_sampled(cov, w, h, stride, inside_band, arg);
}

/* the cap's outline, redrawn sharp over its moved copy (arg: the line's width in pixels) */
static void paint_cap_ring(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  float line = *(const float *)arg, radius = w / 2.0f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float dx = x + 0.5f - w / 2.0f, dy = y + 0.5f - h / 2.0f;
      float d = radius - root(dx * dx + dy * dy); /* in from the outside */
      cov[y * stride + x] = coverage_of(d < line / 2 ? d + 0.5f : line + 0.5f - d);
    }
  }
}

/* A stick: its cap moved by the tilt (and pushed in while clicked): the cap's place filled with the grey
 * under it, then the cap itself, cut from the picture a row of pixels at a time (a circle the quads can
 * draw), and its outline redrawn smooth. Untilted and not clicked, the picture is left as it is. */
/* How far stick s's cap is pushed in (0..1): it goes in over STICK_PRESS_MS as the stick is clicked and comes
 * back out over STICK_RELEASE_MS as it's let go, eased at both ends. */
#define STICK_PRESS_MS 70.0f
#define STICK_RELEASE_MS 140.0f
static float stick_pressed(int s, bool clicked)
{
  static float amount[2];
  static uint64_t last[2];
  uint64_t now = sys_tick();
  float ms = last[s] ? (float)(now - last[s]) * 1000.0f / (float)sys_tick_freq() : 1000;
  last[s] = now;
  float step = ms / (clicked ? STICK_PRESS_MS : STICK_RELEASE_MS);
  amount[s] = clampf(amount[s] + (clicked ? step : -step), 0, 1);
  return smooth(amount[s]);
}

/* the stick's cap where it's tilted, `press` of the way pushed in (smaller and darker) */
static void draw_stick(const placed_t *p, int s, int tilt_x, int tilt_y, float press)
{
  int dev = p->dev;
  float k = p->k, r = pictures[dev].stick_r;
  if (!tilt_x && !tilt_y && press <= 0)
    return;
  float cx = pictures[dev].stick_x[s], cy = pictures[dev].stick_y[s];
  float under = pictures[dev].under_stick / 255.0f;
  int d = (int)(2 * (r + 1) * k + 0.5f);
  gpuui_shape(SHAPE_DISC, d, d, paint_round, &directions[0], on_x(p, cx) - d / 2.0f, on_y(p, cy) - d / 2.0f,
              (gpu_colour_t){ under, under, under, gpu_fade });
  float scale = 1 - 0.08f * press, shade = 1 - 0.22f * press;
  float mx = on_x(p, cx + tilt_x * pictures[dev].travel / 100), my = on_y(p, cy - tilt_y * pictures[dev].travel / 100);
  float R = r * k * scale; /* the cap on screen */
  int rows = (int)(2 * R + 1);
  for (int j = 0; j < rows; j++) {
    float top = my - R + j, dy = top + 0.5f - my;
    if (dy * dy >= R * R)
      continue;
    float half = root(R * R - dy * dy);
    float h = top + 1 > my + R ? my + R - top : 1;
    /* the same row of the cap in the picture */
    float sx0 = cx - half / (k * scale), sx1 = cx + half / (k * scale);
    float sy0 = cy + (top - my) / (k * scale), sy1 = cy + (top + h - my) / (k * scale);
    float st[4] = { sx0 / PICTURE_W, sy0 / PICTURE_H, sx1 / PICTURE_W, sy1 / PICTURE_H };
    gpuui_image(dev, mx - half, top, 2 * half, h, st, (gpu_colour_t){ shade, shade, shade, gpu_fade });
  }
  int ring = (int)(2 * R + 0.5f);
  float line = pictures[dev].stick_line * k * scale;
  line = line < 1 ? 1 : line;
  gpuui_shape(SHAPE_CAP_RING + (uint32_t)dev, ring, ring, paint_cap_ring, &line, mx - ring / 2.0f, my - ring / 2.0f,
              (gpu_colour_t){ 0, 0, 0, 0.9f * gpu_fade });
}

/* the Pro Controller's d-pad arm held: lit in the arm's shape, inside its outline */
static void light_arm(const placed_t *p, int dir)
{
  const float hw = pictures[p->dev].arm_half, len = pictures[p->dev].arm_len, cx = pictures[p->dev].cross_x,
              cy = pictures[p->dev].cross_y, inner = hw + 2;
  float x0, y0, x1, y1;
  switch (dir) {
    case UP: x0 = cx - hw, x1 = cx + hw, y0 = cy - len, y1 = cy - inner; break;
    case DOWN: x0 = cx - hw, x1 = cx + hw, y0 = cy + inner, y1 = cy + len; break;
    case LEFT: x0 = cx - len, x1 = cx - inner, y0 = cy - hw, y1 = cy + hw; break;
    default: x0 = cx + inner, x1 = cx + len, y0 = cy - hw, y1 = cy + hw; break;
  }
  int w = (int)((x1 - x0) * p->k + 0.5f), h = (int)((y1 - y0) * p->k + 0.5f);
  if (w < 2 || h < 2)
    return;
  gpuui_shape(SHAPE_ARM + (uint32_t)dir, w, h, paint_arm, &directions[dir], on_x(p, x0), on_y(p, y0),
              to_gpu(styles[GFX_NORMAL].colour, HELD_ALPHA));
}

/* the Pro Controller's L or R held: lit in its band's shape */
static void light_band(const placed_t *p, int side)
{
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (unsigned i = 0; i < sizeof(pro_l_band) / sizeof(pro_l_band[0]); i++) {
    point_t q = pro_l_band[i];
    x0 = q.x < x0 ? q.x : x0, x1 = q.x > x1 ? q.x : x1, y0 = q.y < y0 ? q.y : y0, y1 = q.y > y1 ? q.y : y1;
  }
  int w = (int)((x1 - x0) * p->k + 1.5f), h = (int)((y1 - y0) * p->k + 1.5f);
  band_at_t at = { x0, y0, p->k, side == 1 };
  float left = side ? PICTURE_W - x1 : x0; /* the right band is the left one mirrored */
  gpuui_shape(SHAPE_BAND + (uint32_t)side, w, h, paint_band, &at, on_x(p, left), on_y(p, y0),
              to_gpu(styles[GFX_NORMAL].colour, HELD_ALPHA));
}

/* the input display on the game's picture of the controller in use: it in the middle, the sticks moved as
 * they're tilted, what's held lit on it, and the shoulder buttons it doesn't show as glyphs above it;
 * true if drawn */
/* the width inside its band the input display needs for its controller's picture at its height, with a cell
 * each side as draw_pad_picture leaves; 0 without the picture */
static float pad_picture_width(const gfx_panel_t *panel, int rows, int scale)
{
  int dev = panel->device < GAMEIMAGES ? panel->device : GAMEIMAGE_PRO;
  if (rows < 1 || !gpuui_image_ready(dev))
    return 0;
  const int16_t *crop = pictures[dev].crop;
  float readouts_top = (float)((TEXT_TOP_FP + panel->yoff + (rows - 1) * CELL_HT) * scale / NUI_SC_ONE);
  float area_h = readouts_top - CELL_HT * fp_px - 3 * fp_px; /* as draw_pad_picture's */
  float cw = crop[2] - crop[0], ch = crop[3] - crop[1];
  return area_h > 0 && ch > 0 ? cw * area_h / ch + 2 * CELL_W * fp_px : 0;
}

static bool draw_pad_picture(const gfx_panel_t *panel, const pad_names_t *names, float x, float y, float w,
                             float readouts_top, int gem, const bool readout_shown[2])
{
  int dev = panel->device < GAMEIMAGES ? panel->device : GAMEIMAGE_PRO;
  if (!gpuui_image_ready(dev))
    return false;
  const int16_t *crop = pictures[dev].crop;
  float shoulder_row = CELL_HT * fp_px; /* the glyphs' row at the top */
  float area_x = x + CELL_W * fp_px, area_w = w - 2 * CELL_W * fp_px;
  float area_y = y + shoulder_row + 2 * fp_px, area_h = readouts_top - area_y - fp_px;
  float cw = crop[2] - crop[0], ch = crop[3] - crop[1];
  float k = area_w / cw < area_h / ch ? area_w / cw : area_h / ch; /* screen pixels per picture pixel */
  placed_t p = { dev, area_x + (area_w - cw * k) / 2, area_y + (area_h - ch * k) / 2, k };
  float st[4] = { crop[0] / PICTURE_W, crop[1] / PICTURE_H, crop[2] / PICTURE_W, crop[3] / PICTURE_H };
  gpuui_image(dev, p.px, p.py, cw * k, ch * k, st, (gpu_colour_t){ 1, 1, 1, gpu_fade });
  /* the sticks, then what's held */
  for (int s = 0; s < 2; s++)
    draw_stick(&p, s, panel->sticks[2 * s], panel->sticks[2 * s + 1],
               stick_pressed(s, panel->clicks >> (GFX_CLICK_L3 + s) & 1));
  static const int abxy[4] = { PAD_A, PAD_B, PAD_X, PAD_Y };
  for (int i = 0; i < 4; i++)
    if (names->held[abxy[i]])
      light(&p, pictures[dev].abxy[i]);
  if (names->held[PAD_MINUS])
    light(&p, pictures[dev].minus);
  if (names->held[PAD_PLUS])
    light(&p, pictures[dev].plus);
  static const int arms[4] = { UP, DOWN, LEFT, RIGHT }; /* gfx.h's clicks order */
  for (int arm = 0; arm < 4; arm++) {
    if (!(panel->clicks >> (GFX_CLICK_DPAD_UP + arm) & 1))
      continue;
    if (pictures[dev].cross)
      light_arm(&p, arms[arm]);
    else
      light(&p, pictures[dev].dpad[arm]);
  }
  if (pictures[dev].bands) {
    if (names->held[PAD_L])
      light_band(&p, 0);
    if (names->held[PAD_R])
      light_band(&p, 1);
  }
  /* above it: ZL and ZR, and L and R when the picture can't show them */
  rgb_t lit = styles[GFX_NORMAL].colour;
  const float unlit = 0.38f;
  const gf_glyph_t *g = gamefont_glyph(GF_GLYPHS, gem, GF_BTN_ZL);
  float step = g ? g->w * 1.15f : gem * 0.8f, gy = y + shoulder_row / 2 + fp_px;
  for (int side = 0; side < 2; side++) {
    int inner = side ? PAD_R : PAD_L, outer = side ? PAD_ZR : PAD_ZL;
    float sx = on_x(&p, pictures[dev].shoulder[side]);
    float ox = pictures[dev].bands ? sx : sx + (side ? step : -step);
    if (ox - step / 2 < area_x) /* kept inside the panel */
      ox = area_x + step / 2;
    if (ox + step / 2 > area_x + area_w)
      ox = area_x + area_w - step / 2;
    if (!pictures[dev].bands)
      centred_glyph(pad_names[inner].glyph, gem, sx, gy, to_gpu(lit, names->held[inner] ? 1 : unlit));
    centred_glyph(pad_names[outer].glyph, gem, ox, gy, to_gpu(lit, names->held[outer] ? 1 : unlit));
  }
  /* under each stick shown (watches.c writes its readout then), its tilt: "x, y", centred on its place. The
   * places: under the sticks, or, where the widest two ("-100, -100") wouldn't fit side by side there (the
   * pro controller's sticks are close), the two a gap apart centred under the picture. Each is centred on its
   * place, so a value's width doesn't move it */
  int em = gamefont_em_for_height(GF_REGULAR, (int)(CELL_HT * TEXT_FILL * fp_px + 0.5f));
  int base = (int)(readouts_top + CELL_HT * fp_px * 0.5f) + gamefont_ascent(GF_REGULAR, em) / 2;
  float widest = gamefont_measure(GF_REGULAR, em, "-100, -100", 10) / 64.0f, gap = em * 1.2f;
  float at[2] = { on_x(&p, pictures[dev].stick_x[0]), on_x(&p, pictures[dev].stick_x[1]) };
  if (readout_shown[0] && readout_shown[1] && at[1] - at[0] < widest + gap) {
    float mid = on_x(&p, (pictures[dev].crop[0] + pictures[dev].crop[2]) / 2.0f);
    at[0] = mid - (widest + gap) / 2;
    at[1] = mid + (widest + gap) / 2;
  }
  for (int s = 0; s < 2; s++) {
    if (!readout_shown[s])
      continue;
    char text[16];
    snprintf(text, sizeof(text), "%d, %d", panel->sticks[2 * s], panel->sticks[2 * s + 1]);
    int n = (int)__builtin_strlen(text);
    float tw = gamefont_measure(GF_REGULAR, em, text, n) / 64.0f;
    /* centred on its place, or, where the widest wouldn't fit inside the panel there (the handheld's sticks
     * are at its ends), against the panel's side: its start (or its end) stays put as the value changes */
    float left = at[s] - tw / 2;
    if (at[s] - widest / 2 < area_x)
      left = area_x;
    else if (at[s] + widest / 2 > area_x + area_w)
      left = area_x + area_w - tw;
    draw_run(text, n, GF_REGULAR, em, (int)((left - gpu_x) * 64), base - (int)gpu_y, styles[GFX_LOG].colour);
  }
  return true;
}

/* The input display's controller in the game's style: on the game's picture of the controller in use
 * once it's loaded (gameimage.c), else its buttons as the game's button glyphs where gz has their names
 * (lit while held), the sticks as a ring with a dot where the stick is (filled while clicked in), the
 * d-pad as its glyph with the held arms lit. The stick readouts in its last row either way. */
static void draw_pad(const gfx_panel_t *panel, float x, float y, float w, int cols, int rows, int scale,
                     nui_legacy_fn legacy)
{
  float ox = x + cell_x(0, scale), oy = y + (TEXT_TOP_FP + panel->yoff) * fp_px; /* the text's top-left */
  int em = gamefont_em_for_height(GF_REGULAR, (int)(CELL_HT * TEXT_FILL * fp_px + 0.5f));
  int gem = (int)(em * 1.5f);
  rgb_t lit = styles[GFX_NORMAL].colour;
  float unlit = 0.38f;
  pad_names_t names;
  find_pad_names(panel, cols, rows, &names);
  int row = rows - 1; /* the readouts */
  int top = (TEXT_TOP_FP + panel->yoff + row * CELL_HT) * scale / NUI_SC_ONE;
  bool readout_shown[2] = { false, false }; /* watches.c's "L x y" / "R x y" in that row */
  for (int c = 0; c + 1 < cols; c++) {
    char k = panel->ch[row][c], next = panel->ch[row][c + 1];
    if ((k == 'L' || k == 'R') && (next == ' ' || next == '-' || (next >= '0' && next <= '9')))
      readout_shown[k == 'R'] = true;
  }
  gpu_x = x, gpu_y = y; /* draw_pad_picture's text is placed from here */
  if (draw_pad_picture(panel, &names, x, y, w, y + top, gem, readout_shown)) {
    return; /* the picture has its own readouts */
  } else {
    for (int i = 0; i < PAD_NAMES; i++)
      if (names.x[i] >= 0)
        centred_glyph(pad_names[i].glyph, gem, ox + names.x[i] * fp_px, oy + names.y[i] * fp_px,
                      to_gpu(lit, names.held[i] ? 1 : unlit));
    /* the sticks */
    static const int stick_at[2][2] = { { GFX_PAD_LSTICK_X, GFX_PAD_LSTICK_Y }, { GFX_PAD_RSTICK_X, GFX_PAD_RSTICK_Y } };
    for (int k = 0; k < 2; k++) {
      float cx = ox + stick_at[k][0] * fp_px, cy = oy + stick_at[k][1] * fp_px;
      int d = (int)(2 * GFX_PAD_STICK_R * fp_px + 0.5f), dot = (int)(4.6f * fp_px + 0.5f);
      if (panel->clicks >> (GFX_CLICK_L3 + k) & 1)
        gpuui_shape(SHAPE_DISC, d, d, paint_round, &directions[0], cx - d / 2.0f, cy - d / 2.0f, to_gpu(lit, 0.3f));
      gpuui_shape(SHAPE_RING, d, d, paint_round, 0, cx - d / 2.0f, cy - d / 2.0f, to_gpu(lit, 0.55f));
      float sx = cx + panel->sticks[2 * k] * GFX_PAD_STICK_TRAVEL * fp_px / 100;
      float sy = cy - panel->sticks[2 * k + 1] * GFX_PAD_STICK_TRAVEL * fp_px / 100;
      gpuui_shape(SHAPE_DISC, dot, dot, paint_round, &directions[0], sx - dot / 2.0f, sy - dot / 2.0f, to_gpu(lit, 1));
    }
    /* the d-pad: its glyph, and over it the glyph of each arm held */
    float dx = ox + GFX_PAD_DPAD_X * fp_px, dy = oy + GFX_PAD_DPAD_Y * fp_px;
    int held_arms = panel->clicks >> GFX_CLICK_DPAD_UP & 0xF;
    int dpad_em = gem * 2; /* as wide as gz's cross */
    if (!held_arms)
      centred_glyph(GF_DPAD, dpad_em, dx, dy, to_gpu(lit, unlit));
    for (int arm = 0; arm < 4; arm++) /* up, down, left, right: as GF_DPAD_UP's order */
      if (held_arms >> arm & 1)
        centred_glyph(GF_DPAD_UP + (uint32_t)arm, dpad_em, dx, dy, to_gpu(lit, 1));
  }
  /* the readouts: text, in gz's rows */
  strip_h = (TEXT_TOP_FP + panel->yoff + (row + 1) * CELL_HT) * scale / NUI_SC_ONE - top;
  gpu_x = x;
  gpu_y = y + top;
  draw_row(panel, row, cols, scale, legacy);
}

/* a listed panel's rows (the menu laid out, the watches) in the grid's place (x, y, w, h), at alpha */
static void draw_list(const gfx_panel_t *panel, float x, float y, float w, float h, int cols, int rows, int scale,
                      nui_legacy_fn legacy, float value_right, float alpha)
{
  float cell = CELL_W * fp_px;
  bool menu = is_menu(panel);
  mrow_t r = { .panel = panel, .cols = cols, .scale = scale, .legacy = legacy };
  r.right = w - cell; /* what's in it ends there (nativeui_gpu_size) */
  r.value_right = value_right;
  find_crumbs(&r);
  int hint_line = -1;
  float top = (menu ? MENU_PAD_FP : LIST_PAD_FP) * fp_px;
  int prev = GFX_ROW_TEXT;
  float row_top[GFX_ROWS + 1] = { 0 };
  for (int row = 0; row < rows; row++) {
    row_top[row] = top;
    row_top[row + 1] = top + row_height_fp(panel, row) * fp_px;
    r.row = row;
    r.kind = kind_of(panel, row);
    r.grid = (panel->kind[row] & GFX_ROW_GRID) != 0;
    r.h = row_height_fp(panel, row) * fp_px;
    if (top + r.h > h)
      break;
    gpu_x = (float)x;
    gpu_y = (float)(y + (int)(top + 0.5f));
    gpu_fade = alpha * (panel->clear ? panel->alpha[row] / 255.0f : 1);
    if (r.kind == GFX_ROW_GAP && prev == GFX_ROW_TITLE) { /* the game's ornament line under the title */
      float x0 = gpu_x + cell_x(0, scale) - 6 * fp_px, x1 = gpu_x + r.right + (SELECT_OVER + 6) * fp_px;
      if (!menu || !ornament_line(x0, x1, gpu_y + r.h * LINE_IN_GAP, 9 * fp_px, to_gpu(styles[GFX_DIM].colour, gpu_fade))) {
        float line = fp_px * 0.5f < 1 ? 1 : (float)(int)(fp_px * 0.5f + 0.5f);
        gpuui_fill(x0, gpu_y + r.h * LINE_IN_GAP - line / 2, x1 - x0, line, to_gpu(styles[GFX_DIM].colour, 0.6f * gpu_fade));
      }
    } else if (r.kind == GFX_ROW_HINT && gpu_fade > 0) { /* the hint, wrapped by its own type */
      row_metrics(&r);
      if (hint_line < 0) {
        float left = (float)cell_x(HINT_COL, scale);
        wrap_hint(&r, panel->hint, r.right - left);
        hint_line = 0;
      }
      if (hint_line < hint_count)
        hint_words(&r, panel->hint, hint_at[hint_line].from, hint_at[hint_line].to,
                   cell_x(HINT_COL, scale) + hint_at[hint_line].indent, true);
      hint_line++;
    } else if (r.kind != GFX_ROW_GAP && r.kind != GFX_ROW_SKIP && gpu_fade > 0) {
      menu_row(&r);
      if (r.kind == GFX_ROW_CAPTION) { /* its line under it, across the panel, fading out at its end */
        float line = fp_px * 0.5f < 1 ? 1 : (float)(int)(fp_px * 0.5f + 0.5f);
        faded_strip(gpu_x + cell_x(0, scale), gpu_x + r.right, gpu_y + r.h - CAPTION_LINE_UP * fp_px, line, 0,
                    0.35f, styles[GFX_TITLE].colour, 0.55f);
      }
    }
    prev = r.kind;
    top += r.h;
  }
  gpu_fade = alpha;
  if (menu && !cursor_still)
    cursor_leaving();
  if (menu && scroll_bar_shown(panel) && panel->scroll_row + panel->scroll_rows <= rows) {
    /* the game's scroll bar left of the list (in gz's column of its ^ / v): its track (a shade lighter than
     * the box) over the rows that scroll, its thumb for the part shown in the menu's gold, the picture's
     * grain kept as darker and lighter gold */
    float bx = x + cell_x(SCROLL_MARK_COL, scale) + (CELL_W - SCROLL_W) / 2 * fp_px, bw = SCROLL_W * fp_px;
    float by = y + row_top[panel->scroll_row], bh = row_top[panel->scroll_row + panel->scroll_rows] - row_top[panel->scroll_row];
    picture_bar(GAMEIMAGE_SCROLL_BASE, bx, by, bw, bh, 26, true, to_gpu(styles[GFX_FAINT].colour, 0.7f * gpu_fade), 0);
    float tw = bw * THUMB_W, inset = (bw - tw) / 2; /* the thumb runs inside the track, `inset` from its edges */
    float run = bh - 2 * inset, share = (float)panel->scroll_rows / panel->scroll_items;
    float th = run * share > 3 * tw ? run * share : 3 * tw;
    int last = panel->scroll_items - panel->scroll_rows;
    float ty = by + inset + (run - th) * (last > 0 ? (float)panel->scroll_top / last : 0);
    static const gpu_colour_t gold[2] = { { 0.62f, 0.48f, 0.22f, 0 }, { 1.0f, 0.93f, 0.70f, 1 } };
    picture_bar(GAMEIMAGE_SCROLL_BAR, bx + inset, ty, tw, th, 12, true, (gpu_colour_t){ 1, 1, 1, gpu_fade }, gold);
  }
  top += row_fp[GFX_ROW_GAP].height * fp_px;
  r.h = GUIDE_FP * fp_px;
  if (shows_controls(panel) && top + r.h <= h) {
    gpu_x = (float)x;
    gpu_y = (float)(y + (int)(top + 0.5f));
    draw_controls(&r, (float)cell_x(CONTROLS_COL, scale), true);
  }
  gpu_fade = 1;
}

/* --- the menu's motion ------------------------------------------------------------------------------
 * Only what keeps the eye on its place moves: going into a menu or back out of one. The band grows or
 * shrinks from its fixed top-left corner to the new menu's size instead of jumping, the old menu fades
 * out quickly and the new one fades in with a small slide from where it came (in from the right when
 * going deeper, from the left going back). All under 200 ms on a strong ease-out, taken up again from
 * where it is when the next change comes before it's done. Opening and closing the menu (a button combo,
 * used all the time) and moving through a list don't move. */
#define SIZE_MS 180
#define IN_MS 170
#define OUT_MS 90
#define SLIDE_FP 8
#define GONE_MS 100 /* not drawn this long: the menu was closed, and opens where it is */

/* the strong ease-out, cubic-bezier(0.23, 1, 0.32, 1), at t (0..1) */
static float ease_out(float t)
{
  if (t <= 0)
    return 0;
  if (t >= 1)
    return 1;
  const float x1 = 0.23f, y1 = 1, x2 = 0.32f, y2 = 1;
  float u = t; /* the curve's parameter whose x is t: Newton's method, from t */
  for (int i = 0; i < 6; i++) {
    float v = 1 - u;
    float x = 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u - t;
    float dx = 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2);
    if (dx > -1e-5f && dx < 1e-5f)
      break;
    u = clampf(u - x / dx, 0, 1);
  }
  float v = 1 - u;
  return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u;
}

static struct {
  uint64_t drawn;                /* when the menu was last drawn (sys_tick) */
  float w, h;                    /* the band's size now */
  float from_w, from_h, to_w, to_h;
  uint64_t size_start;
  gfx_panel_t last, old;         /* the menu as last drawn; the one fading out */
  float last_w, last_h, last_value_right, old_w, old_h, old_value_right; /* their grids' size, value column */
  int dir;                       /* the change: 1 deeper, -1 back, 0 sideways */
  uint64_t change_start;
  bool changing;
} motion;

static float since_ms(uint64_t then, uint64_t now) { return (float)(now - then) * 1000.0f / (float)sys_tick_freq(); }

/* how deep the menu is: its breadcrumb's " > " */
static int depth_of(const gfx_panel_t *panel)
{
  int n = 0;
  for (int row = 0; row < GFX_ROWS && kind_of(panel, row) == GFX_ROW_TITLE; row++)
    for (int c = 1; c + 1 < GFX_COLS; c++)
      n += panel->ch[row][c] == '>' && panel->ch[row][c - 1] == ' ';
  return n;
}

static bool same_menu(const gfx_panel_t *a, const gfx_panel_t *b)
{
  for (int row = 0; row < GFX_ROWS; row++) {
    bool ta = kind_of(a, row) == GFX_ROW_TITLE, tb = kind_of(b, row) == GFX_ROW_TITLE;
    if (ta != tb)
      return false;
    if (!ta)
      return true;
    for (int c = 0; c < GFX_COLS; c++)
      if (a->ch[row][c] != b->ch[row][c])
        return false;
  }
  return true;
}

/* the band's size this frame for a menu whose band is w x h: what it was, moving towards that */
static void menu_band_size(const gfx_panel_t *panel, float w, float h, uint64_t now, float *bw, float *bh)
{
  bool fresh = !motion.drawn || since_ms(motion.drawn, now) > GONE_MS;
  if (fresh) {
    motion.w = motion.from_w = motion.to_w = w;
    motion.h = motion.from_h = motion.to_h = h;
    motion.changing = false;
  } else {
    if (w != motion.to_w || h != motion.to_h) { /* a new size: from where the band is now */
      motion.from_w = motion.w, motion.from_h = motion.h;
      motion.to_w = w, motion.to_h = h;
      motion.size_start = now;
    }
    float e = ease_out(since_ms(motion.size_start, now) / SIZE_MS);
    motion.w = motion.from_w + (motion.to_w - motion.from_w) * e;
    motion.h = motion.from_h + (motion.to_h - motion.from_h) * e;
    if (!same_menu(panel, &motion.last)) { /* another menu: the last one fades out */
      cursor_jump = true, cursor_was.on = false;
      int a = depth_of(panel), b = depth_of(&motion.last);
      motion.dir = a > b ? 1 : a < b ? -1 : 0;
      motion.old = motion.last;
      motion.old_w = motion.last_w, motion.old_h = motion.last_h, motion.old_value_right = motion.last_value_right;
      motion.change_start = now;
      motion.changing = true;
    }
  }
  *bw = motion.w;
  *bh = motion.h;
}

/* the menu's content: the new one fading and sliding in, the old one fading out */
static void draw_menu_moving(const gfx_panel_t *panel, float x, float y, float w, float h, int cols, int rows,
                             int scale, nui_legacy_fn legacy, float value_right)
{
  uint64_t now = sys_tick();
  float alpha = 1, dx = 0;
  if (motion.changing) {
    float t = since_ms(motion.change_start, now);
    if (t < OUT_MS) { /* the old menu, going */
      int ocols = motion.old.cols < GFX_COLS ? motion.old.cols : GFX_COLS;
      int orows = motion.old.rows < GFX_ROWS ? motion.old.rows : GFX_ROWS;
      plan_numbers(&motion.old, orows, ocols);
      float out = 1 - ease_out(t / OUT_MS);
      cursor_still = true;
      draw_list(&motion.old, x - motion.dir * SLIDE_FP * fp_px * (1 - out), y, motion.old_w, motion.old_h, ocols, orows,
                scale, legacy, motion.old_value_right, out);
      cursor_still = false;
      plan_numbers(panel, rows, cols);
    }
    float in = ease_out(t / IN_MS);
    alpha = in;
    dx = motion.dir * SLIDE_FP * fp_px * (1 - in);
    if (t >= IN_MS)
      motion.changing = false;
  }
  draw_list(panel, x + dx, y, w, h, cols, rows, scale, legacy, value_right, alpha);
  motion.last = *panel;
  motion.last_w = w, motion.last_h = h, motion.last_value_right = value_right;
  motion.drawn = now ? now : 1;
}

bool nativeui_gpu_panel(const gfx_panel_t *panel, int x, int y, int w, int h, int scale, int opacity,
                        nui_legacy_fn legacy)
{
  if (!gpuui_ready() || !nativeui_ready() || w <= 0 || h <= 0)
    return false;
  int cols = panel->cols < GFX_COLS ? panel->cols : GFX_COLS;
  int rows = panel->rows < GFX_ROWS ? panel->rows : GFX_ROWS;
  plan_numbers(panel, rows, cols);
  fp_px = (float)scale / NUI_SC_ONE;
  bool listed = !panel->pad && laid_out(panel), menu = listed && is_menu(panel);
  float alpha = clampf(opacity / 100.0f, 0, 1);
  gpu_fade = 1;
  /* (x, y, w, h) is the band's place (nativeui_gpu_size); gz's grid of the panel sits inside it */
  band_ext_t e = band_of(panel);
  float bw = (float)w, bh = (float)h;
  if (menu) /* the menu's band grows and shrinks from its corner (draw_menu_moving) */
    menu_band_size(panel, bw, bh, sys_tick(), &bw, &bh);
  if (menu && !panel->clear) /* the menu's box, as the game's info boxes */
    draw_box((float)x, (float)y, bw, bh, alpha);
  else if (!panel->clear) /* the watches' band (and the input display's, its background on), as the game's guide */
    draw_band((float)x, (float)y, bw, bh, e.fl * fp_px, e.fr * fp_px, e.ft * fp_px, e.fb * fp_px, alpha);
  x += (int)(e.l * fp_px + 0.5f);
  y += (int)(e.t * fp_px + 0.5f);
  w -= (int)((e.l + e.r) * fp_px + 0.5f);
  h -= (int)((e.t + e.b) * fp_px + 0.5f);
  if (w <= 0 || h <= 0)
    return true;
  gpu = true;
  strip_w = w;
  if (panel->pad) {
    draw_pad(panel, (float)x, (float)y, (float)w, cols, rows, scale, legacy);
    gpu = false;
    return true;
  }
  if (!listed) { /* gz's rows (the log: each row fading on its own) */
    for (int row = 0; row < rows; row++) {
      int top = (TEXT_TOP_FP + panel->yoff + row * CELL_HT) * scale / NUI_SC_ONE;
      int bottom = (TEXT_TOP_FP + panel->yoff + (row + 1) * CELL_HT) * scale / NUI_SC_ONE;
      if (top >= h)
        break;
      gpu_fade = panel->clear ? panel->alpha[row] / 255.0f : 1;
      if (gpu_fade <= 0)
        continue;
      strip_h = bottom - top;
      gpu_x = (float)x;
      gpu_y = (float)(y + top);
      draw_row(panel, row, cols, scale, legacy);
    }
    gpu_fade = 1;
    gpu = false;
    return true;
  }

  float value_right = measured_panel == panel && measured_hash == panel->hash ? measured_value_right : 0;
  if (menu) {
    draw_menu_moving(panel, (float)x, (float)y, (float)w, (float)h, cols, rows, scale, legacy, value_right);
  } else {
    draw_list(panel, (float)x, (float)y, (float)w, (float)h, cols, rows, scale, legacy, value_right, 1);
  }
  gpu = false;
  return true;
}
