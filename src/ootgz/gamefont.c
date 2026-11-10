/* The game's UI fonts (see gamefont.h).
 *
 * Font2/Scalable/<name>.Nin_Ounce_NVN2.bfotf.zs: zstd with dictionary 1, then Nintendo's font scramble:
 * u32 magic, u32 size, the OpenType font, every big-endian word XORed with one key. The key is
 * word 1 ^ (decompressed size - 8), and word 0 ^ key is then 0x7F9A0218 (0xA6018502 for this game).
 * The fonts are OpenType with CFF outlines ("OTTO"), which stb_truetype rasterises.
 *
 * Glyphs are rasterised on the render thread the first time they're drawn at a size, into an arena;
 * the cache is only touched there, so it needs no lock. All memory is taken at load time, on the game's
 * thread: the arena, and a scratch block stb_truetype allocates from while it rasterises a glyph. Its
 * CFF code doesn't check for a failed allocation (it would write through NULL), so running out of
 * scratch jumps back out of stb (jump.h) and that glyph counts as missing. When the cache or the arena
 * is nearly full, gamefont_begin_draw starts it afresh; a draw that still ran out says so
 * (gamefont_draw_complete), and the renderer draws again or falls back to gz's font. */
#include "gamefont.h"

#include <stddef.h>

#include "gamefile.h"
#include "jump.h"
#include "sys.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);                             /* from main */
size_t strlen(const char *s);                   /* from sdk */

/* --- stb_truetype, with the module's libc -------------------------------------------------- */
#define SCRATCH_SIZE (128u << 10) /* stb's working memory for one glyph (its outline and edges) */
static uint8_t *scratch;
static size_t scratch_used;
static jump_buf_t tt_jump;
static bool tt_armed; /* tt_jump is set: stb is rasterising a glyph */

static void *tt_alloc(size_t n)
{
  n = (n + 15) & ~(size_t)15;
  if (scratch_used + n > SCRATCH_SIZE || n > SCRATCH_SIZE) {
    if (tt_armed)
      JUMP_BACK(tt_jump);
    return 0; /* never: stb allocates only while it rasterises */
  }
  void *p = scratch + scratch_used;
  scratch_used += n;
  return p;
}
#define STBTT_ifloor(x) ((int)__builtin_floor(x))
#define STBTT_iceil(x) ((int)__builtin_ceil(x))
#define STBTT_sqrt(x) __builtin_sqrt(x)
#define STBTT_pow(x, y) __builtin_pow(x, y)   /* only in the SDF code, which isn't used */
#define STBTT_fmod(x, y) __builtin_fmod(x, y) /* ... */
#define STBTT_cos(x) __builtin_cos(x)         /* ... */
#define STBTT_acos(x) __builtin_acos(x)       /* ... */
#define STBTT_fabs(x) __builtin_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), tt_alloc(x))
#define STBTT_free(x, u) ((void)(u), (void)(x)) /* the scratch block is reset per glyph */
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy __builtin_memcpy
#define STBTT_memset __builtin_memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

/* --- loading --------------------------------------------------------------------------------- */

#define FONT_DIR GAME_ROM "/Font2/Scalable/"
#define FONT_SUFFIX ".Nin_Ounce_NVN2.bfotf.zs"
#define SCRAMBLE_MAGIC 0x7F9A0218u

static const char *const face_files[GF_FACES] = {
  [GF_REGULAR] = FONT_DIR "ClarimoUDPE-Regular" FONT_SUFFIX,
  [GF_BOLD] = FONT_DIR "ClarimoUDPE-DemiBold" FONT_SUFFIX,
  [GF_GLYPHS] = FONT_DIR "External" FONT_SUFFIX,
};

#define ARENA_SIZE (384u << 10) /* the glyphs' coverage: at the cache's limit a menu used up to 300 KiB */

static stbtt_fontinfo faces[GF_FACES];
static uint8_t *face_data[GF_FACES];
static uint8_t *arena;
static volatile bool ready;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* undo the scramble in place; the font then starts at data + 8 */
static bool unscramble(uint8_t *data, size_t size)
{
  if (size < 16)
    return false;
  uint32_t key = be32(data + 4) ^ (uint32_t)(size - 8);
  if ((be32(data) ^ key) != SCRAMBLE_MAGIC)
    return false;
  for (size_t i = 8; i + 4 <= size; i += 4) {
    uint32_t w = be32(data + i) ^ key;
    data[i] = (uint8_t)(w >> 24);
    data[i + 1] = (uint8_t)(w >> 16);
    data[i + 2] = (uint8_t)(w >> 8);
    data[i + 3] = (uint8_t)w;
  }
  return true;
}

static void unload(void)
{
  for (int f = 0; f < GF_FACES; f++) {
    free(face_data[f]);
    face_data[f] = 0;
  }
  free(arena);
  free(scratch);
  arena = scratch = 0;
}

bool gamefont_load(void)
{
  if (ready)
    return true;
  uint64_t start = sys_tick();
  bool ok = (arena = aligned_alloc(16, ARENA_SIZE)) != 0 && (scratch = aligned_alloc(16, SCRATCH_SIZE)) != 0;
  if (!ok)
    log_printf("game fonts: no memory for the glyphs");
  for (int f = 0; ok && f < GF_FACES; f++) {
    size_t size = 0;
    uint8_t *data = gamefile_read_zs(face_files[f], &size);
    if (!data) {
      ok = false; /* gamefile logged why */
    } else if (!unscramble(data, size) || !stbtt_InitFont(&faces[f], data + 8, stbtt_GetFontOffsetForIndex(data + 8, 0))) {
      log_printf("game fonts: %s isn't a font this module can read", face_files[f]);
      free(data);
      ok = false;
    } else {
      face_data[f] = data;
    }
  }
  gamefile_release(); /* the zstd dictionary is only needed for loading */
  if (!ok) {
    unload();
    return false;
  }
  __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
  log_printf("game fonts: Clarimo UD PE and the button glyphs loaded (%u ms)",
             (unsigned)((sys_tick() - start) * 1000 / sys_tick_freq()));
  return true;
}

bool gamefont_ready(void)
{
  return __atomic_load_n(&ready, __ATOMIC_ACQUIRE);
}

/* --- the glyph cache (render thread) -------------------------------------------------------- */

#define CACHE_SLOTS 2048 /* a power of two */
#define CACHE_FULL (CACHE_SLOTS * 3 / 4)

typedef struct {
  uint32_t codepoint;
  uint16_t em;
  uint8_t face;
  uint8_t state; /* 0 empty, 1 a glyph, 2 the face has none */
  gf_glyph_t glyph;
} slot_t;

static slot_t cache[CACHE_SLOTS];
static size_t arena_used;
static int cached;
static bool flush_next;    /* full: start afresh at the next draw */
static bool draw_short;    /* this draw asked for a glyph the cache had no room for */

static void flush(void)
{
  for (int i = 0; i < CACHE_SLOTS; i++)
    cache[i].state = 0;
  arena_used = 0;
  cached = 0;
  flush_next = false;
}

static unsigned slot_of(gf_face_t face, int em, uint32_t cp)
{
  uint32_t h = cp * 2654435761u ^ (uint32_t)em * 40503u ^ (uint32_t)face * 0x9E3779B9u;
  return (h ^ h >> 15) & (CACHE_SLOTS - 1);
}

static float scale_for(gf_face_t face, int em)
{
  return stbtt_ScaleForMappingEmToPixels(&faces[face], (float)em);
}

const gf_glyph_t *gamefont_glyph(gf_face_t face, int em, uint32_t cp)
{
  if (!gamefont_ready() || face >= GF_FACES || em <= 0 || em > 255)
    return 0;
  unsigned i = slot_of(face, em, cp);
  for (int probe = 0; probe < CACHE_SLOTS; probe++, i = (i + 1) & (CACHE_SLOTS - 1)) {
    slot_t *s = &cache[i];
    if (s->state && s->codepoint == cp && s->em == em && s->face == face)
      return s->state == 1 ? &s->glyph : 0;
    if (!s->state)
      break;
  }
  if (flush_next || cached >= CACHE_FULL) { /* full: start again with the next draw (the glyphs handed out stay valid till then) */
    flush_next = true;
    draw_short = true;
    return 0;
  }
  slot_t *s = &cache[i];
  stbtt_fontinfo *info = &faces[face];
  float scale = scale_for(face, em);
  int index = stbtt_FindGlyphIndex(info, (int)cp);
  s->codepoint = cp;
  s->em = (uint16_t)em;
  s->face = (uint8_t)face;
  s->state = 2;
  cached++;
  if (!index)
    return 0;
  int advance, lsb, x0, y0, x1, y1;
  stbtt_GetGlyphHMetrics(info, index, &advance, &lsb);
  stbtt_GetGlyphBitmapBox(info, index, scale, scale, &x0, &y0, &x1, &y1);
  int w = x1 - x0, h = y1 - y0;
  size_t bytes = ((size_t)(w > 0 ? w : 0) * (h > 0 ? h : 0) + 15) & ~(size_t)15;
  if (arena_used + bytes > ARENA_SIZE) {
    flush_next = true;
    draw_short = true;
    s->state = 0;
    cached--;
    return 0;
  }
  uint8_t *coverage = arena + arena_used;
  if (w > 0 && h > 0) {
    scratch_used = 0;
    if (JUMP_SET(tt_jump)) { /* stb ran out of scratch: a glyph this complex stays missing */
      tt_armed = false;
      static bool warned;
      if (!warned)
        log_printf("game fonts: glyph U+%04X at %d px is too complex to draw", (unsigned)cp, em);
      warned = true;
      return 0; /* the slot stays "none" */
    }
    tt_armed = true;
    stbtt_MakeGlyphBitmap(info, coverage, w, h, w, scale, scale, index);
    tt_armed = false;
  }
  arena_used += bytes;
  s->glyph = (gf_glyph_t){ coverage, (int16_t)w, (int16_t)h, (int16_t)x0, (int16_t)y0,
                           (int16_t)(advance * scale * 64.0f + 0.5f) };
  s->state = 1;
  return &s->glyph;
}

void gamefont_begin_draw(void)
{
  if (flush_next || cached >= CACHE_FULL || arena_used > ARENA_SIZE * 3 / 4)
    flush();
  draw_short = false;
}

bool gamefont_draw_complete(void)
{
  return !draw_short;
}

int gamefont_em_for_height(gf_face_t face, int height)
{
  if (!gamefont_ready() || face >= GF_FACES || height <= 0)
    return height;
  /* ScaleForPixelHeight maps ascent - descent to height; ScaleForMappingEmToPixels(1) is 1 / units per em */
  return (int)(stbtt_ScaleForPixelHeight(&faces[face], (float)height) / scale_for(face, 1));
}

int gamefont_ascent(gf_face_t face, int em)
{
  if (!gamefont_ready() || face >= GF_FACES)
    return em * 3 / 4;
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&faces[face], &ascent, &descent, &gap);
  return (int)(ascent * scale_for(face, em) + 0.5f);
}

int gamefont_kern(gf_face_t face, int em, uint32_t left, uint32_t right)
{
  if (!gamefont_ready() || face >= GF_FACES)
    return 0;
  stbtt_fontinfo *info = &faces[face];
  int kern = stbtt_GetCodepointKernAdvance(info, (int)left, (int)right);
  return kern ? (int)(kern * scale_for(face, em) * 64.0f) : 0;
}

int gamefont_measure(gf_face_t face, int em, const char *s, int n)
{
  int pen = 0;
  for (int i = 0; i < n; i++) {
    const gf_glyph_t *g = gamefont_glyph(face, em, (unsigned char)s[i]);
    if (g)
      pen += g->advance;
    if (i + 1 < n)
      pen += gamefont_kern(face, em, (unsigned char)s[i], (unsigned char)s[i + 1]);
  }
  return pen;
}
