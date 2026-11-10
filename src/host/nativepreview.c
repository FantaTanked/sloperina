/* nativepreview: renders panels the way nativeui.c draws them in the game, on the PC, from the
 * game's own files (a RomFS folder of the user's dump), so the drawing can be checked without the
 * game (CONTRIBUTING.md: "check drawing by compiling the drawing code on the PC").
 *
 *   make nativepreview
 *   obj/host/nativepreview <romfs folder> <out.ppm> [scale, 16 = 1x; default 40]
 *
 * It builds the module's gamefile.c, zstd.c, gamefont.c and nativeui.c for the host, with nn::fs and
 * the module's log replaced by stdio below. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ootgz/gamefont.h"
#include "../ootgz/gfx.h"
#include "../ootgz/nativeui.h"
#include "../ootgz/sdfs.h"

/* --- the module's environment, on stdio ------------------------------------------------------- */

static const char *romfs;

nn_result_t fs_open(fs_file_t *out, const char *path, int mode)
{
  (void)mode;
  const char *prefix = "content:";
  if (strncmp(path, prefix, strlen(prefix)))
    return 1;
  char full[1024];
  snprintf(full, sizeof(full), "%s%s", romfs, path + strlen(prefix));
  FILE *f = fopen(full, "rb");
  out->handle = f;
  return f ? 0 : 1;
}

nn_result_t fs_size(int64_t *out, fs_file_t f)
{
  FILE *file = f.handle;
  long at = ftell(file);
  fseek(file, 0, SEEK_END);
  *out = ftell(file);
  fseek(file, at, SEEK_SET);
  return 0;
}

nn_result_t fs_read(size_t *read, fs_file_t f, int64_t offset, void *buf, size_t size)
{
  fseek(f.handle, (long)offset, SEEK_SET);
  *read = fread(buf, 1, size, f.handle);
  return 0;
}

void fs_close(fs_file_t f)
{
  fclose(f.handle);
}

void log_printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fputs("sloperinaGZ: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

uint64_t sys_tick(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t sys_tick_freq(void)
{
  return 1000000000u;
}

/* --- sample panels ----------------------------------------------------------------------------- */

static void put(gfx_panel_t *p, int row, int col, gfx_style_t style, const char *s)
{
  for (int i = 0; s[i] && col + i < GFX_COLS; i++) {
    p->ch[row][col + i] = s[i];
    p->style[row][col + i] = (uint8_t)style;
  }
  if (row + 1 > p->rows)
    p->rows = row + 1;
  int end = col + (int)strlen(s);
  if (end > p->cols)
    p->cols = end;
}

static void menu_panel(gfx_panel_t *p)
{
  memset(p, 0, sizeof(*p));
  put(p, 0, 0, GFX_TITLE, "sloperinaGZ 0.0.3");
  static const char *const items[] = { "link", "warps", "scene", "cheats", "inventory", "equips",
                                       "story", "watches", "debug", "settings" };
  for (int i = 0; i < 10; i++)
    put(p, 2 + i, 1, i == 1 ? GFX_SELECTED : GFX_NORMAL, items[i]);
}

static void settings_panel(gfx_panel_t *p)
{
  memset(p, 0, sizeof(*p));
  put(p, 0, 0, GFX_TITLE, "settings");
  put(p, 2, 0, GFX_DIM, "menu settings");
  put(p, 3, 1, GFX_NORMAL, "language");
  put(p, 3, 22, GFX_NORMAL, "English");
  put(p, 4, 1, GFX_SELECTED, "style");
  put(p, 4, 22, GFX_SELECTED, "the game's");
  put(p, 5, 1, GFX_NORMAL, "background opacity");
  put(p, 5, 22, GFX_NORMAL, "60%");
  put(p, 7, 0, GFX_DIM, "cheats");
  static const char *const cheats[] = { "infinite health", "infinite magic", "infinite rupees", "2x speed" };
  for (int i = 0; i < 4; i++) {
    char box[3] = { i % 2 ? GFX_CHECK_ON : GFX_CHECK_OFF, GFX_WIDE_REST, 0 };
    put(p, 8 + i, 1, GFX_NORMAL, box);
    put(p, 8 + i, 4, GFX_NORMAL, cheats[i]);
  }
  char arrows[] = { GFX_ICON_UP, ' ', GFX_ICON_DOWN, ' ', GFX_ICON_ADD, ' ', GFX_ICON_REMOVE, 0 };
  put(p, 13, 1, GFX_NORMAL, arrows);
  put(p, 13, 10, GFX_NORMAL, "watch rows");
}

static void watches_panel(gfx_panel_t *p)
{
  memset(p, 0, sizeof(*p));
  static const char *const rows[][2] = { { "x", "18.74" }, { "y", "189.19" }, { "z", "27.05" }, { "facing", "154.2" },
                                         { "speed", "1.00" }, { "camera angle", "154.2" }, { "js height", "Max" } };
  for (int i = 0; i < 7; i++) {
    put(p, i, 0, GFX_DIM, rows[i][0]);
    char v[16];
    snprintf(v, sizeof(v), "%10s", rows[i][1]);
    put(p, i, 14, GFX_NORMAL, v);
  }
  /* as sloperina's watches write them: values from column 13, after a 12-column label */
  put(p, 8, 0, GFX_NORMAL, "facing");
  put(p, 8, 13, GFX_NORMAL, "0.0");
  put(p, 9, 0, GFX_NORMAL, "camera angle 180.0");
}

/* --- drawing ----------------------------------------------------------------------------------- */

#define PANEL_W(cols, sc) ((((cols) + 2) * 8 * (sc) + 15) / 16)
#define PANEL_H(rows, sc) ((((rows) * 10 + 8) * (sc) + 15) / 16)

static int render(const gfx_panel_t *p, int scale, uint8_t *canvas, int cw, int ch, int x0, int y0)
{
  int w = PANEL_W(p->cols < 16 ? 16 : p->cols, scale), h = PANEL_H(p->rows, scale);
  uint8_t *buf = malloc((size_t)w * h * 4);
  if (!nativeui_rasterise(p, buf, scale, w, h, 37 /* RGBA8 */, 4, 0)) {
    fprintf(stderr, "nativeui_rasterise refused the panel\n");
    free(buf);
    return 0;
  }
  for (int y = 0; y < h && y0 + y < ch; y++)
    for (int x = 0; x < w && x0 + x < cw; x++)
      memcpy(canvas + ((size_t)(y0 + y) * cw + x0 + x) * 3, buf + ((size_t)y * w + x) * 4, 3);
  free(buf);
  return w;
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s <romfs folder> <out.ppm> [scale, 16 = 1x]\n", argv[0]);
    return 1;
  }
  romfs = argv[1];
  int scale = argc > 3 ? atoi(argv[3]) : 40;
  if (!gamefont_load()) {
    fprintf(stderr, "the game's fonts didn't load\n");
    return 1;
  }
  int cw = 2200, ch = 900;
  uint8_t *canvas = calloc((size_t)cw * ch, 3);
  for (size_t i = 0; i < (size_t)cw * ch; i++) { /* a mid grey, so the panels' edges show */
    canvas[i * 3] = 0x50;
    canvas[i * 3 + 1] = 0x4c;
    canvas[i * 3 + 2] = 0x44;
  }
  gfx_panel_t p;
  int x = 20;
  menu_panel(&p);
  x += render(&p, scale, canvas, cw, ch, x, 20) + 20;
  settings_panel(&p);
  x += render(&p, scale, canvas, cw, ch, x, 20) + 20;
  watches_panel(&p);
  render(&p, scale, canvas, cw, ch, x, 20);
  FILE *f = fopen(argv[2], "wb");
  fprintf(f, "P6\n%d %d\n255\n", cw, ch);
  fwrite(canvas, 3, (size_t)cw * ch, f);
  fclose(f);
  return 0;
}
