/* Enemy health. Each frame the living enemies within EH_RANGE of Link are projected with the game's
 * camera (view.c) and handed to the render thread (publish.h) in the form it draws: in the game's style
 * a bar each (where over its head, its life), which overlayui.c draws on the GPU; in gz's (or if the GPU
 * path can't start) rectangles that nvn.c clears into the picture (no shaders, no font): a bar (dark back,
 * green fill, red below a quarter) with "life/max" above it in seven-segment digits. An enemy at 0 life
 * has no bar; actors.c leaves out those that haven't appeared yet. Nothing is drawn while the game is
 * paused (its pause screen or an item menu), as the air walls. Each drop in an enemy's life goes to the
 * log file ("enemy hp:" lines). */
#include "enemyhp.h"

#include "gamedata.h"
#include "link.h"
#include "publish.h"
#include "sys.h"
#include "view.h"

float sqrtf(float x); /* from sdk */

bool enemy_hp_shown, enemy_hp_xray;

#define MAX_ENEMIES 32
#define MAX_RECTS 2048
#define EH_RANGE 30.0f      /* metres from Link */
#define NEAR_PLANE 0.05f
#define REF_WIDTH 1920      /* reference pixels, as airwall.c */
#define REF_HEIGHT 1080
#define BAR_W 120.0f
#define BAR_H 10.0f
#define BAR_BORDER 2.0f
#define LOW_LIFE 0.25f      /* below this share of max the fill is red */
#define DIGIT_W 10.0f
#define DIGIT_H 18.0f
#define STROKE 3.0f         /* a segment's thickness */
#define CHAR_GAP 4.0f
#define TEXT_GAP 6.0f       /* between the text and the bar */
#define TEXT_PAD 3.0f       /* the text's dark background reaches this far round it */
/* A point on an enemy counts as hidden where the scene is nearer than it by more than this (metres, and of
 * its distance): the enemy's own body, round its middle, isn't */
#define HIDDEN_M 0.5f
#define HIDDEN_SHARE 0.04f

/* --- rectangles ------------------------------------------------------------------------------------ */

static aw_dot_t rects[PUBLISH_BUFFERS][MAX_RECTS];
static int n_rects[PUBLISH_BUFFERS];
static eh_bar_t bars[PUBLISH_BUFFERS][MAX_ENEMIES];
static int n_bars[PUBLISH_BUFFERS];
static publish_t published = PUBLISH_INIT;
static int taken = -1; /* the render thread's, from enemyhp_take to enemyhp_release */

static uint16_t to_unit16(float v, float size)
{
  v = v / size * 65535;
  return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

static void add_rect(aw_dot_t *out, int *n, float x0, float y0, float x1, float y1, uint8_t color)
{
  if (*n >= MAX_RECTS || x1 <= x0 || y1 <= y0)
    return;
  aw_dot_t *r = &out[(*n)++];
  r->x0 = to_unit16(x0, REF_WIDTH);
  r->x1 = to_unit16(x1, REF_WIDTH);
  r->y0 = to_unit16(y0, REF_HEIGHT);
  r->y1 = to_unit16(y1, REF_HEIGHT);
  r->color = color;
}

/* --- seven-segment text ---------------------------------------------------------------------------- */

/* segments a b c d e f g as bits 0..6 */
static const uint8_t digit_segments[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void add_digit(aw_dot_t *out, int *n, float x, float y, int digit)
{
  uint8_t s = digit_segments[digit];
  float w = DIGIT_W, h = DIGIT_H, t = STROKE, mid = DIGIT_H / 2;
  if (s & 0x01) add_rect(out, n, x, y, x + w, y + t, EH_TEXT);                       /* a */
  if (s & 0x02) add_rect(out, n, x + w - t, y, x + w, y + mid, EH_TEXT);             /* b */
  if (s & 0x04) add_rect(out, n, x + w - t, y + mid, x + w, y + h, EH_TEXT);         /* c */
  if (s & 0x08) add_rect(out, n, x, y + h - t, x + w, y + h, EH_TEXT);               /* d */
  if (s & 0x10) add_rect(out, n, x, y + mid, x + t, y + h, EH_TEXT);                 /* e */
  if (s & 0x20) add_rect(out, n, x, y, x + t, y + mid, EH_TEXT);                     /* f */
  if (s & 0x40) add_rect(out, n, x, y + mid - t / 2, x + w, y + mid + t / 2, EH_TEXT); /* g */
}

/* a slash: a staircase of small squares from bottom-left to top-right */
static void add_slash(aw_dot_t *out, int *n, float x, float y)
{
  const int steps = 4;
  for (int i = 0; i < steps; i++) {
    float sx = x + i * (DIGIT_W - STROKE) / (steps - 1);
    float sy = y + DIGIT_H - (i + 1) * DIGIT_H / steps;
    add_rect(out, n, sx, sy, sx + STROKE, sy + DIGIT_H / steps, EH_TEXT);
  }
}

/* "life/max" laid out from x: digits (0-9) and '/'; returns the width */
static float text_width(const char *s)
{
  float w = 0;
  for (; *s; s++)
    w += DIGIT_W + (s[1] ? CHAR_GAP : 0);
  return w;
}

static void add_text(aw_dot_t *out, int *n, float x, float y, const char *s)
{
  for (; *s; s++, x += DIGIT_W + CHAR_GAP) {
    if (*s >= '0' && *s <= '9')
      add_digit(out, n, x, y, *s - '0');
    else if (*s == '/')
      add_slash(out, n, x, y);
  }
}

/* --- one enemy -------------------------------------------------------------------------------------- */

/* an enemy's bar in bar_out[*n_bar], or (no bar_out) its rectangles in out[*n...] */
static void add_enemy(aw_dot_t *out, int *n, eh_bar_t *bar_out, int *n_bar, const view_cam_t *cam, const enemy_t *e)
{
  float head[3] = { e->pos[0], e->pos[1] + e->height, e->pos[2] };
  float v[3], sx, sy;
  view_point(cam, head, v);
  if (v[2] < NEAR_PLANE)
    return;
  view_screen(cam, v, &sx, &sy);
  if (sx < 0 || sx > 1 || sy < 0 || sy > 1)
    return;
  if (bar_out) {
    if (*n_bar >= MAX_ENEMIES)
      return;
    eh_bar_t *bar = &bar_out[(*n_bar)++];
    *bar = (eh_bar_t){ e->id, sx, sy, v[2], e->life, e->max_life, 0, { 0 }, { 0 }, { 0 } };
    /* the points: a quarter, a half, three quarters of the way up, and the bar's (its bottom, over the head) */
    for (int k = 1; k <= EH_PROBES; k++) {
      float up[3] = { e->pos[0], e->pos[1] + e->height * k / EH_PROBES, e->pos[2] }, pv[3], px, py;
      view_point(cam, up, pv);
      if (pv[2] < cam->near)
        continue;
      view_screen(cam, pv, &px, &py);
      if (px < 0 || px > 1 || py < 0 || py > 1)
        continue;
      bar->probe_x[bar->n_probes] = px;
      bar->probe_y[bar->n_probes] = py;
      bar->probe_depth[bar->n_probes] = view_depth(cam, pv[2] - HIDDEN_M - HIDDEN_SHARE * pv[2]);
      bar->n_probes++;
    }
    return;
  }
  float cx = sx * REF_WIDTH, bottom = sy * REF_HEIGHT;

  /* the bar, its bottom at the head point */
  float bx0 = cx - BAR_W / 2, by0 = bottom - BAR_H;
  float share = e->max_life > 0 ? (float)e->life / (float)e->max_life : 0;
  share = share < 0 ? 0 : share > 1 ? 1 : share;
  add_rect(out, n, bx0 - BAR_BORDER, by0 - BAR_BORDER, bx0 + BAR_W + BAR_BORDER, bottom + BAR_BORDER, EH_BACK);
  add_rect(out, n, bx0, by0, bx0 + BAR_W * share, bottom, share < LOW_LIFE ? EH_LOW : EH_FILL);

  /* "life/max" above it, on a dark background */
  char text[24];
  snprintf(text, sizeof(text), "%d/%d", (int)e->life, (int)e->max_life);
  float tw = text_width(text);
  float tx = cx - tw / 2, ty = by0 - BAR_BORDER - TEXT_GAP - DIGIT_H;
  add_rect(out, n, tx - TEXT_PAD, ty - TEXT_PAD, tx + tw + TEXT_PAD, ty + DIGIT_H + TEXT_PAD, EH_BACK);
  add_text(out, n, tx, ty, text);
}

/* --- hits ------------------------------------------------------------------------------------------- */

static struct { uintptr_t id; int32_t life; } seen[MAX_ENEMIES];
static int n_seen;

/* Log each drop in life since last frame (the log file only), then remember this frame's enemies. */
static void log_hits(const enemy_t *enemies, int n)
{
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n_seen; j++)
      if (seen[j].id == enemies[i].id && enemies[i].life < seen[j].life)
        log_printf("enemy hp: %#lx %d -> %d of %d", (unsigned long)enemies[i].id, (int)seen[j].life,
                   (int)enemies[i].life, (int)enemies[i].max_life);
  n_seen = 0;
  for (int i = 0; i < n && n_seen < MAX_ENEMIES; i++) {
    seen[n_seen].id = enemies[i].id;
    seen[n_seen].life = enemies[i].life;
    n_seen++;
  }
}

/* --- frame ------------------------------------------------------------------------------------------ */

void enemyhp_frame(bool gpu)
{
  int f = publish_next(&published), n = 0, nb = 0;
  if (enemy_hp_shown) {
    enemy_t enemies[MAX_ENEMIES];
    int n_enemies = enemies_read(enemies, MAX_ENEMIES);
    log_hits(enemies, n_enemies);
    vec3f_t link;
    view_cam_t cam;
    if (!gd_game_paused(0, 0) && link_drawn_pos(&link) && view_camera(&cam)) {
      for (int i = 0; i < n_enemies; i++) {
        const enemy_t *e = &enemies[i];
        float dx = e->pos[0] - link.x, dy = e->pos[1] - link.y, dz = e->pos[2] - link.z;
        if (e->life > 0 && e->max_life > 0 && sqrtf(dx * dx + dy * dy + dz * dz) < EH_RANGE)
          add_enemy(rects[f], &n, gpu ? bars[f] : 0, &nb, &cam, e);
      }
    }
  } else {
    n_seen = 0;
  }
  n_rects[f] = n;
  n_bars[f] = nb;
  publish_set(&published, n || nb ? f : -1);
}

void enemyhp_take(void)
{
  taken = publish_take(&published);
}

const eh_bar_t *enemyhp_bars(int *n)
{
  *n = taken < 0 ? 0 : n_bars[taken];
  return taken < 0 ? 0 : bars[taken];
}

const aw_dot_t *enemyhp_published(int *n)
{
  *n = taken < 0 ? 0 : n_rects[taken];
  return taken < 0 ? 0 : rects[taken];
}

void enemyhp_release(void)
{
  taken = -1;
  publish_release(&published);
}
