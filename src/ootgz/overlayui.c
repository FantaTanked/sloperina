/* The air walls' outlines and the enemies' health in the game's style, on the GPU (nvn.c draws them in
 * its gpuui frame, under the panels). gz's style, or the game's if its GPU path can't start, keeps the
 * cleared rectangles (airwall.c, enemyhp.c).
 *
 * The outlines are smooth lines with a soft glow under them, fading a little with distance, hidden where Link
 * is in front of them (the scene's depth made his alone, scenedepth.c and gpuui.c; unless airwalls_xray).
 *
 * A health bar is built from the HUD's magic meter (MpGauge*: its shadow, its grainy track, its fill of
 * segments with their green made red, its frame), with the life and the most over it on the number plate
 * of the minigames' counters (NumBaseMiniGame), in the game's type. When the life drops, the part lost
 * stays a moment in a pale gold and then drains away, as the game's own meters empty. Without those
 * pictures (still loading, or not found) the bar is plain, in their colours. A bar fades out while its enemy
 * is hidden behind the scenery (scenedepth.c; unless enemy_hp_xray). */
#include "overlayui.h"

#include "airwall.h"
#include "enemyhp.h"
#include "gamefont.h"
#include "gameimage.h"
#include "gfx.h"
#include "gpuui.h"
#include "scenedepth.h"
#include "sys.h"
#include "uishader.h"

float sqrtf(float x); /* from sdk */

#define REF_H 1080.0f /* sizes are in pixels of a 1080-high picture */

volatile bool overlayui_failed;

bool overlayui_on_gpu(void)
{
  return gfx_style == GFX_STYLE_GAME && uishader_program(0) && gamefont_ready() && !overlayui_failed;
}

/* --- the air walls' outlines ------------------------------------------------------------------------ */

#define LINE_W 2.4f      /* a line's solid middle */
#define GLOW_W 9.0f      /* its glow's */
#define GLOW_ALPHA 0.22f
#define FADE_FROM 15.0f  /* metres: nearer lines are full, farther ones fade to FADE_TO */
#define FADE_OVER 45.0f
#define FADE_TO 0.45f
/* gpuui's quads are shared with the panels drawn after (MAX_QUADS a frame): the outlines take at most
 * this many lines, the nearest walls' first (airwall.c sorts them), and the nearest of them a glow */
#define MAX_LINES_DRAWN 1400
#define MAX_GLOWS 500

static const gpu_colour_t line_colours[AW_NCOLORS] = {
  [AW_BLOCKS] = { 0.30f, 0.86f, 1.0f, 1 }, /* cyan: stops Link */
  [AW_INSIDE] = { 1.0f, 0.32f, 0.30f, 1 }, /* red: Link is inside it */
  [AW_OTHER] = { 0.66f, 0.58f, 1.0f, 1 },  /* violet: other kinds */
};

static bool draw_lines(float w, float h, float px)
{
  int n;
  const aw_line_t *lines = airwall_lines(&n);
  if (!lines || !n)
    return false;
  for (int pass = 0; pass < 2; pass++) { /* the glows first, then the lines over them */
    int count = pass ? (n < MAX_LINES_DRAWN ? n : MAX_LINES_DRAWN) : (n < MAX_GLOWS ? n : MAX_GLOWS);
    for (int i = 0; i < count; i++) {
      const aw_line_t *l = &lines[i];
      gpu_colour_t c = line_colours[l->color < AW_NCOLORS ? l->color : 0];
      float far = (l->near - FADE_FROM) / FADE_OVER;
      far = far < 0 ? 0 : far > 1 ? 1 : far;
      float a = 1 - (1 - FADE_TO) * far;
      c.a = pass ? a : a * GLOW_ALPHA;
      if (airwalls_xray)
        gpuui_line(l->x0 * w, l->y0 * h, l->x1 * w, l->y1 * h, (pass ? LINE_W : GLOW_W) * px, c);
      else /* hidden where the scene is nearer (with nvn.c's depth for the frame) */
        gpuui_line_depth(l->x0 * w, l->y0 * h, l->d0, l->x1 * w, l->y1 * h, l->d1, (pass ? LINE_W : GLOW_W) * px, c);
    }
  }
  return true;
}

/* --- the enemies' health ---------------------------------------------------------------------------- */

#define BAR_W 196.0f     /* the meter's length */
#define BAR_ASPECT (21.0f / 507 * 1.4f) /* its height to its length: MpGaugeBase's, a little thicker */
#define FILL_INSET 0.16f /* the fill inside the frame, of the bar's height each side */
#define PLATE_H 24.0f    /* the number plate */
#define PLATE_CAP 0.2f   /* of the plate's picture at each end: its pointed ends, kept */
#define PLATE_GAP 3.0f   /* between the plate and the bar */
#define SHADOW_ALPHA 0.35f
#define DIGITS_H 13.0f   /* the life's type: its capitals */
#define LOW_LIFE 0.25f   /* below this share the fill glows brighter, slowly */
#define MAX_BARS_DRAWN 16 /* the nearest (each takes some 40 to 60 of the frame's quads: the fill's tiles, 2 a segment) */
#define DRAIN_WAIT_MS 280
#define DRAIN_MS 420

/* each enemy's lost part, as it drains (the render thread's, by its actor) */
#define MAX_TRACKED 32
static struct {
  uintptr_t id;
  int32_t life;
  float shown;     /* the share the lost part reaches down from */
  uint64_t hit, seen;
} tracked[MAX_TRACKED];

static float ms_since(uint64_t then, uint64_t now) { return (float)(now - then) * 1000.0f / (float)sys_tick_freq(); }

/* the share above the life still shown as lost (its draining part reaches from life to it) */
static float drained(const eh_bar_t *b, float share, uint64_t now)
{
  int slot = -1, free_slot = -1;
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (tracked[i].id == b->id && tracked[i].seen)
      slot = i;
    else if (free_slot < 0 && (!tracked[i].seen || ms_since(tracked[i].seen, now) > 2000))
      free_slot = i;
  }
  if (slot < 0) {
    if (free_slot < 0)
      return share;
    slot = free_slot;
    tracked[slot].id = b->id, tracked[slot].life = b->life, tracked[slot].shown = share, tracked[slot].hit = 0;
  }
  if (b->life < tracked[slot].life) { /* a hit: the lost part from where the bar showed it */
    float from = tracked[slot].shown;
    if (tracked[slot].hit) { /* still draining from an earlier hit: from where that has got to */
      float t = (ms_since(tracked[slot].hit, now) - DRAIN_WAIT_MS) / DRAIN_MS;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      float prev = b->max_life > 0 ? (float)tracked[slot].life / (float)b->max_life : 0;
      from = from + (prev - from) * (1 - (1 - t) * (1 - t));
    }
    tracked[slot].shown = from;
    tracked[slot].hit = now;
  } else if (b->life > tracked[slot].life) { /* healed: nothing lost */
    tracked[slot].hit = 0;
  }
  tracked[slot].life = b->life;
  tracked[slot].seen = now;
  if (!tracked[slot].hit)
    return tracked[slot].shown = share;
  float t = (ms_since(tracked[slot].hit, now) - DRAIN_WAIT_MS) / DRAIN_MS;
  if (t >= 1) {
    tracked[slot].hit = 0;
    return tracked[slot].shown = share;
  }
  t = t < 0 ? 0 : t;
  return tracked[slot].shown + (share - tracked[slot].shown) * (1 - (1 - t) * (1 - t)); /* easing out */
}

/* a picture stretched across, its first and last `cap` of it (as fractions of its width) kept at its
 * height's scale */
static void stretched(int image, float x, float y, float w, float h, float cap, float aspect, gpu_colour_t colour)
{
  float end = cap * h / aspect; /* a kept end on screen (aspect: the picture's height to its width) */
  if (2 * end > w)
    end = w / 2;
  float xs[4] = { x, x + end, x + w - end, x + w }, ss[4] = { 0, cap, 1 - cap, 1 };
  for (int i = 0; i < 3; i++) {
    float crop[4] = { ss[i], 0, ss[i + 1], 1 };
    if (xs[i + 1] > xs[i])
      gpuui_image(image, xs[i], y, xs[i + 1] - xs[i], h, crop, colour);
  }
}

static float text_width(gf_face_t face, int em, const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return gamefont_measure(face, em, s, n) / 64.0f;
}

/* the text, its slashes `lift` up (and a soft shadow down and right, px out) */
static void draw_text(gf_face_t face, int em, const char *s, float x, float baseline, float lift, gpu_colour_t colour,
                      float px)
{
  gpu_colour_t shadow = { 0, 0, 0, 0.45f * colour.a };
  for (const char *p = s; *p; p++) {
    float y = *p == '/' ? baseline - lift : baseline;
    gpuui_glyph(face, em, (unsigned char)*p, x + px, y + px, shadow);
    x += gpuui_glyph(face, em, (unsigned char)*p, x, y, colour);
  }
}

/* a w x h frame t thick */
static void outline(float x, float y, float w, float h, float t, gpu_colour_t colour)
{
  gpuui_fill(x, y, w, t, colour);
  gpuui_fill(x, y + h - t, w, t, colour);
  gpuui_fill(x, y + t, t, h - 2 * t, colour);
  gpuui_fill(x + w - t, y + t, t, h - 2 * t, colour);
}

/* the game's pictures for the bars (else, if they're not there, plain bars in their colours) */
static bool bar_pictures(void)
{
  return gpuui_image_ready(GAMEIMAGE_GAUGE_BASE) && gpuui_image_ready(GAMEIMAGE_GAUGE_FILL) &&
         gpuui_image_ready(GAMEIMAGE_GAUGE_FRAME) && gpuui_image_ready(GAMEIMAGE_PLATE) &&
         gpuui_image_ready(GAMEIMAGE_PLATE_RIM);
}

/* The fill is a segment for each unit of life (or each round step of it, when there are too many). MpGaugeCenter
 * (1024 x 43) is 16 segments of 64 texels: a light edge (their first 4), the body, a dark divider (their last 7). A
 * segment is drawn from them at the meter's scale: an edge, the body in tiles of their bodies' even middles (stretched,
 * a wide segment's grain ran into streaks), a divider. */
#define FILL_TEX_W 1024.0f
#define FILL_TEX_H 43.0f
#define SEG_TEX 64.0f
#define SEG_EDGE 4.0f
#define SEG_DIVIDER 7.0f
#define SEG_BODY_FROM 8.0f  /* a body's even middle, without the light edge's and the divider's shading */
#define SEG_BODY_TO 52.0f
#define MAX_SEGMENTS 12

/* the life a segment stands for: 1 while the most is within MAX_SEGMENTS, else the smallest round step that keeps
 * the segments within it (a Stalfos' 18 in 9 of 2) */
static int life_step(int max)
{
  static const int steps[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000 };
  for (unsigned i = 0; i < sizeof steps / sizeof *steps; i++)
    if ((max + steps[i] - 1) / steps[i] <= MAX_SEGMENTS)
      return steps[i];
  return (max + MAX_SEGMENTS - 1) / MAX_SEGMENTS;
}

/* a part of the fill's picture (s0..s1 across) at x, w wide, cut to lo..hi across; mapped (the lost part) or tinted */
static void fill_piece(float x, float y, float w, float h, float s0, float s1, float lo, float hi, gpu_colour_t tint,
                       const gpu_colour_t *map)
{
  float a = x > lo ? x : lo, b = x + w < hi ? x + w : hi;
  if (w <= 0 || b - a < 0.01f)
    return;
  float crop[4] = { s0 + (s1 - s0) * (a - x) / w, 0, s0 + (s1 - s0) * (b - x) / w, 1 };
  if (map)
    gpuui_image_mapped(GAMEIMAGE_GAUGE_FILL, a, y, b - a, h, crop, map[0], map[1], 1);
  else
    gpuui_image(GAMEIMAGE_GAUGE_FILL, a, y, b - a, h, crop, tint);
}

/* the fill's segments over the shares lo..hi of the meter (fx, fy, fw x fh), each `step` of the most's life; without
 * the pictures plain, a darker line closing each */
static void segments(float fx, float fy, float fw, float fh, int max, float lo, float hi, gpu_colour_t tint,
                     const gpu_colour_t *map, bool pictures)
{
  if (max <= 0 || hi <= lo)
    return;
  int step = life_step(max), n = (max + step - 1) / step;
  float k = fh / FILL_TEX_H, cut_lo = fx + fw * lo, cut_hi = fx + fw * hi;
  for (int i = 0; i < n; i++) {
    int end = (i + 1) * step < max ? (i + 1) * step : max;
    float s0 = fx + fw * (float)(i * step) / (float)max, s1 = fx + fw * (float)end / (float)max, sw = s1 - s0;
    if (s1 <= cut_lo || s0 >= cut_hi)
      continue;
    float e = SEG_EDGE * k, d = SEG_DIVIDER * k;
    if (e + d > sw * 0.6f) { /* a narrow segment keeps its body */
      float f = sw * 0.6f / (e + d);
      e *= f, d *= f;
    }
    if (!pictures) {
      gpu_colour_t dark = { tint.r * 0.45f, tint.g * 0.45f, tint.b * 0.45f, tint.a };
      float a = s0 > cut_lo ? s0 : cut_lo, b = s1 - d < cut_hi ? s1 - d : cut_hi;
      if (b > a)
        gpuui_fill(a, fy, b - a, fh, tint);
      if (s1 - d < cut_hi)
        gpuui_fill(s1 - d, fy, (s1 < cut_hi ? s1 : cut_hi) - (s1 - d), fh, dark);
      continue;
    }
    float t0 = (float)(1 + i % 14) * SEG_TEX; /* one of the picture's inner segments */
    fill_piece(s0, fy, e, fh, t0 / FILL_TEX_W, (t0 + SEG_EDGE) / FILL_TEX_W, cut_lo, cut_hi, tint, map);
    float tile = (SEG_BODY_TO - SEG_BODY_FROM) * k, body = sw - e - d;
    int tiles = body > 0 ? (int)(body / tile + 0.999f) : 0;
    for (int j = 0; j < tiles; j++) { /* each from another of the picture's segments, the last one cut */
      float tx = s0 + e + (float)j * tile, tw = j + 1 < tiles ? tile : body - (float)j * tile;
      float u0 = (float)(1 + (i * 5 + j) % 14) * SEG_TEX + SEG_BODY_FROM;
      fill_piece(tx, fy, tw, fh, u0 / FILL_TEX_W, (u0 + (SEG_BODY_TO - SEG_BODY_FROM) * tw / tile) / FILL_TEX_W, cut_lo,
                 cut_hi, tint, map);
    }
    fill_piece(s1 - d, fy, d, fh, (t0 + SEG_TEX - SEG_DIVIDER) / FILL_TEX_W, (t0 + SEG_TEX) / FILL_TEX_W, cut_lo, cut_hi,
               tint, map);
  }
}

static void draw_bar(const eh_bar_t *b, float w, float h, float px, uint64_t now, bool pictures)
{
  float share = b->max_life > 0 ? (float)b->life / (float)b->max_life : 0;
  share = share < 0 ? 0 : share > 1 ? 1 : share;
  float lost = drained(b, share, now);
  float bw = BAR_W * px, bh = bw * BAR_ASPECT;
  /* its bottom at the head, on whole pixels: the meter's and the plate's rims are a pixel or two thin, and at the
   * fractions of a pixel a turning camera puts them on they came out thicker or fainter frame to frame (a flicker) */
  float cx = (float)(int)(b->x * w + 0.5f);
  float x = (float)(int)(cx - bw / 2 + 0.5f), y = (float)(int)(b->y * h - bh + 0.5f);

  /* the meter: shadow, track, fill and its lost part, frame */
  if (pictures && gpuui_image_ready(GAMEIMAGE_GAUGE_SHADOW)) {
    /* MpGaugeSh is the shadow's left end, 160 x 80, soft all round: its middle at half strength 27 high
     * (the meter's 21, and 3 more each side), the left end at half strength 20.5 in. Drawn at the meter's
     * scale, its left end and the same mirrored for the right with its plain middle stretched between, so
     * it reaches as far past the meter's ends as past its top and bottom. */
    float k = bh / 21, sh = 80 * k, sy = y + bh / 2 - sh / 2, end = 40 * k, sx = x - 23.5f * k;
    const float left[4] = { 0, 0, 0.25f, 1 }, mid[4] = { 0.5f, 0, 0.9f, 1 }, right[4] = { 0.25f, 0, 0, 1 };
    const gpu_colour_t shadow = { 0, 0, 0, SHADOW_ALPHA };
    float sw = bw + 2 * 23.5f * k;
    gpuui_image(GAMEIMAGE_GAUGE_SHADOW, sx, sy, end, sh, left, shadow);
    gpuui_image(GAMEIMAGE_GAUGE_SHADOW, sx + end, sy, sw - 2 * end, sh, mid, shadow);
    gpuui_image(GAMEIMAGE_GAUGE_SHADOW, sx + sw - end, sy, end, sh, right, shadow);
  }
  static const float all[4] = { 0, 0, 1, 1 };
  const gpu_colour_t track = { 0.16f, 0.13f, 0.11f, 0.92f }, rim = { 0.93f, 0.88f, 0.76f, 1 };
  if (pictures)
    gpuui_image(GAMEIMAGE_GAUGE_BASE, x, y, bw, bh, all, track);
  else
    gpuui_fill(x, y, bw, bh, track);
  float inset = bh * FILL_INSET, fx = x + inset, fy = y + inset, fw = bw - 2 * inset, fh = bh - 2 * inset;
  if (lost > share) { /* the lost part, pale gold, draining */
    const gpu_colour_t pale[2] = { { 0.86f, 0.70f, 0.42f, 0 }, { 1.0f, 0.95f, 0.78f, 1 } };
    segments(fx, fy, fw, fh, b->max_life, share, lost, (gpu_colour_t){ 0.96f, 0.86f, 0.62f, 1 }, pale, pictures);
  }
  if (share > 0) {
    float glow = 1;
    if (share < LOW_LIFE) { /* low: the fill brightens and dims, slowly */
      float t = (float)(now % (sys_tick_freq() * 6 / 5)) / (float)(sys_tick_freq() * 6 / 5);
      glow = 1.25f - 0.25f * (2 * (t < 0.5f ? t : 1 - t));
    }
    /* the magic meter's green, red and green swapped: its blue taken out too, for the hearts' red */
    gpu_colour_t red = pictures ? (gpu_colour_t){ 1.25f * glow, 0.85f * glow, 0.38f * glow, 1 }
                                : (gpu_colour_t){ 0.82f * glow, 0.17f * glow, 0.13f * glow, 1 };
    segments(fx, fy, fw, fh, b->max_life, 0, share, red, 0, pictures);
  }
  if (pictures)
    gpuui_image(GAMEIMAGE_GAUGE_FRAME, x, y, bw, bh, all, rim);
  else
    outline(x, y, bw, bh, inset * 0.6f > 1 ? inset * 0.6f : 1, rim);

  /* the life over the most, on the counters' plate */
  char text[24];
  snprintf(text, sizeof(text), "%d/%d", (int)b->life, (int)b->max_life);
  int em = gamefont_em_for_height(GF_BOLD, (int)(DIGITS_H * px + 0.5f));
  float tw = text_width(GF_BOLD, em, text), ph = PLATE_H * px, pw = tw + ph * 1.6f;
  float py = (float)(int)(y - PLATE_GAP * px - ph + 0.5f), plx = (float)(int)(cx - pw / 2 + 0.5f);
  const gpu_colour_t plate = { 0.07f, 0.06f, 0.05f, 0.88f }, plate_rim = { 0.93f, 0.84f, 0.58f, 0.9f };
  if (pictures) {
    stretched(GAMEIMAGE_PLATE, plx, py, pw, ph, PLATE_CAP, 80.0f / 280, plate);
    stretched(GAMEIMAGE_PLATE_RIM, plx, py, pw, ph, PLATE_CAP, 80.0f / 280, plate_rim);
  } else {
    gpuui_fill(plx, py, pw, ph, plate);
  }
  /* the digits centred on the plate, on a whole pixel; the type's slash reaches below them, so it's lifted
   * to the digits' middle, as a font's forms for figures have it */
  const gf_glyph_t *digit = gamefont_glyph(GF_BOLD, em, '0'), *slash = gamefont_glyph(GF_BOLD, em, '/');
  float mid = digit && digit->h > 0 ? digit->y0 + digit->h / 2.0f : em * -0.33f; /* the digits' middle, from the baseline */
  float lift = slash && slash->h > 0 ? (float)(int)(slash->y0 + slash->h / 2.0f - mid + 0.5f) : 0;
  float baseline = (float)(int)(py + ph / 2 - mid + 0.5f);
  draw_text(GF_BOLD, em, text, (float)(int)(cx - tw / 2 + 0.5f), baseline, lift, (gpu_colour_t){ 1, 1, 1, 1 }, px > 1 ? px : 1);
}

static bool draw_bars(float w, float h, float px)
{
  int n;
  const eh_bar_t *bars = enemyhp_bars(&n);
  if (!bars || !n || !gamefont_ready())
    return false;
  bool pictures = bar_pictures();
  int order[MAX_TRACKED];
  n = n < MAX_TRACKED ? n : MAX_TRACKED;
  for (int i = 0; i < n; i++) { /* the farthest first, so the nearest is drawn on top */
    int j = i;
    for (; j > 0 && bars[order[j - 1]].distance < bars[i].distance; j--)
      order[j] = order[j - 1];
    order[j] = i;
  }
  uint64_t now = sys_tick();
  for (int i = n > MAX_BARS_DRAWN ? n - MAX_BARS_DRAWN : 0; i < n; i++) { /* the nearest are last */
    const eh_bar_t *b = &bars[order[i]];
    float alpha = enemy_hp_xray ? 1 : scenedepth_bar_alpha(b->id, now);
    if (alpha <= 0.004f) /* behind the scenery */
      continue;
    gpuui_alpha(alpha);
    draw_bar(b, w, h, px, now, pictures);
  }
  gpuui_alpha(1);
  return true;
}

/* --- frame advance's icon ------------------------------------------------------------------------------ */

/* At the top left as gz has it: two bars (paused), or a bar and a play triangle (advancing a frame), drawn
 * as smooth shapes on a soft shadow. */
#define PAUSE_X 0.035f /* of the picture */
#define PAUSE_Y 0.045f
#define PAUSE_H 34.0f  /* its height */
#define PAUSED 1       /* gfx_pause_icon: paused (else advancing) */

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* a bar with round ends filling its w x h */
static void paint_bar(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  (void)arg;
  float r = w * 0.32f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float px = x + 0.5f, py = y + 0.5f;
      float qx = (px < w / 2.0f ? w / 2.0f - px : px - w / 2.0f) - (w / 2.0f - r);
      float qy = (py < h / 2.0f ? h / 2.0f - py : py - h / 2.0f) - (h / 2.0f - r);
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
      cov[y * stride + x] = (uint8_t)(clamp01(0.5f - d) * 255 + 0.5f);
    }
  }
}

/* a play triangle pointing right, filling its w x h, its corners a little round */
static void paint_play(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  (void)arg;
  float r = w * 0.12f, ax = r, ay = r, bx = r, by = h - r, cx = w - r, cy = h / 2.0f;
  float edges[3][3]; /* each edge's inward normal and offset: n . p - c >= 0 inside */
  const float pts[3][2] = { { ax, ay }, { cx, cy }, { bx, by } };
  for (int i = 0; i < 3; i++) {
    float x0 = pts[i][0], y0 = pts[i][1], x1 = pts[(i + 1) % 3][0], y1 = pts[(i + 1) % 3][1];
    float nx = y0 - y1, ny = x1 - x0, len = sqrtf(nx * nx + ny * ny); /* clockwise on screen: inward */
    nx /= len, ny /= len;
    edges[i][0] = nx, edges[i][1] = ny, edges[i][2] = nx * x0 + ny * y0;
  }
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float d = -1e9f; /* the distance outside the shrunk triangle, less r: rounded corners */
      for (int i = 0; i < 3; i++) {
        float e = edges[i][2] - (edges[i][0] * (x + 0.5f) + edges[i][1] * (y + 0.5f));
        d = e > d ? e : d;
      }
      cov[y * stride + x] = (uint8_t)(clamp01(0.5f - (d - r)) * 255 + 0.5f);
    }
  }
}

static bool draw_pause(float w, float h, float px, int icon)
{
  if (!icon)
    return false;
  int ih = (int)(PAUSE_H * px + 0.5f), bw = (int)(ih * 0.3f + 0.5f), gap = (int)(ih * 0.18f + 0.5f);
  int tw = (int)(ih * 0.62f + 0.5f);
  float x = (float)(int)(w * PAUSE_X), y = (float)(int)(h * PAUSE_Y), d = px * 1.5f;
  const gpu_colour_t shadow = { 0, 0, 0, 0.5f }, white = { 0.98f, 0.96f, 0.92f, 1 };
  for (int pass = 0; pass < 2; pass++) { /* the shadow, then the icon */
    float o = pass ? 0 : d;
    gpu_colour_t c = pass ? white : shadow;
    gpuui_shape(0xA10, bw, ih, paint_bar, 0, x + o, y + o, c);
    if (icon == PAUSED)
      gpuui_shape(0xA10, bw, ih, paint_bar, 0, x + bw + gap + o, y + o, c);
    else
      gpuui_shape(0xA11, tw, ih, paint_play, 0, x + bw + gap + o, y + o, c);
  }
  return true;
}

int overlayui_draw(int w, int h, int pause_icon)
{
  if (w <= 0 || h <= 0)
    return 0;
  float px = (float)h / REF_H;
  return (draw_lines((float)w, (float)h, px) ? OVERLAY_LINES : 0) | (draw_bars((float)w, (float)h, px) ? OVERLAY_BARS : 0) |
         (draw_pause((float)w, (float)h, px, pause_icon) ? OVERLAY_PAUSE : 0);
}
