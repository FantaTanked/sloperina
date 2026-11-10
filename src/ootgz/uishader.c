/* The game's UI shader (see uishader.h).
 *
 * content:/Layout2Shader/MergeShader.Product.100.Nin_Ounce_NVN2.bfsha.zs is a BFSHA 9 (shading model
 * "ui", 121 programs) with one BNSH. Its offsets are file offsets (the file isn't relocated):
 *   header +0x48: the shading model. From it: +0x08 the static options (0x28-byte records: +0 name, a
 *   u16 length then the characters; +0x1a default choice; +0x20 mask; +0x24 key word; +0x25 shift),
 *   +0x88 the programs (0x40-byte records, +0x20 the BNSH variation's file offset), +0x90 the key
 *   table (static then dynamic words per program), +0xa8 the BNSH, +0xe4 the option count, +0xe8 the
 *   program count, +0xea / +0xeb the static and dynamic key lengths in words.
 *   BNSH (offsets from its start): variation +0x10 its binary program; binary program +0x08 the vertex
 *   stage, +0x28 the fragment stage; stage +0x08 control, +0x10 code, +0x18 code size, +0x1c control
 *   size. A control section starts 0x98761234 and holds its code size at +0x44.
 * A program is found by its options' values, as the game's own runtime finds it, not by offsets, so an
 * update that rebuilds the file still works. The parts kept are copied out and the file is freed. */
#include "uishader.h"

#include <stddef.h>

#include "gamefile.h"
#include "sys.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);                             /* from main */

#define SHADER_FILE GAME_ROM "/Layout2Shader/MergeShader.Product.100.Nin_Ounce_NVN2.bfsha.zs"
#define CONTROL_MAGIC 0x98761234u
#define MAX_KEY_WORDS 8

static uishader_program_t programs[UI_PROGRAMS];
static uint8_t *kept; /* the controls and code areas, copied out of the file */
static bool loaded;

typedef struct { const uint8_t *d; size_t n; bool bad; } file_t;

/* n bytes at `at` are in the file (written so a damaged offset can't wrap around) */
static bool in_file(const file_t *f, uint64_t at, uint64_t n)
{
  return at <= f->n && n <= f->n - at;
}

static uint64_t rd(file_t *f, uint64_t at, int bytes)
{
  if (!in_file(f, at, (uint64_t)bytes)) {
    f->bad = true;
    return 0;
  }
  uint64_t v = 0;
  for (int i = 0; i < bytes; i++)
    v |= (uint64_t)f->d[at + i] << (8 * i);
  return v;
}

/* whether the length-prefixed string at `at` is `name` */
static bool name_is(file_t *f, uint64_t at, const char *name)
{
  uint64_t n = rd(f, at, 2);
  if (f->bad || !in_file(f, at + 2, n))
    return false;
  for (uint64_t i = 0; i < n; i++)
    if (!name[i] || f->d[at + 2 + i] != (uint8_t)name[i])
      return false;
  return !name[n];
}

typedef struct { const char *name; int value; } option_t;

/* the program whose static key the given options make (others at their defaults), or -1 */
static int find_program(file_t *f, uint64_t sm, const option_t *want, int n_want)
{
  uint64_t options = rd(f, sm + 0x08, 8), keys = rd(f, sm + 0x90, 8);
  int n_options = (int)rd(f, sm + 0xe4, 2), n_programs = (int)rd(f, sm + 0xe8, 2);
  int static_words = (int)rd(f, sm + 0xea, 1), dynamic_words = (int)rd(f, sm + 0xeb, 1);
  if (f->bad || static_words <= 0 || static_words > MAX_KEY_WORDS)
    return -1;
  uint32_t key[MAX_KEY_WORDS] = { 0 };
  int matched = 0;
  for (int i = 0; i < n_options; i++) {
    uint64_t r = options + 0x28 * (uint64_t)i;
    int16_t choice = (int16_t)rd(f, r + 0x1a, 2);
    uint32_t mask = (uint32_t)rd(f, r + 0x20, 4);
    int word = (int)rd(f, r + 0x24, 1), shift = (int)rd(f, r + 0x25, 1);
    for (int w = 0; w < n_want; w++) {
      if (name_is(f, rd(f, r, 8), want[w].name)) {
        choice = (int16_t)want[w].value;
        matched++;
      }
    }
    if (word >= static_words || shift > 31)
      return -1;
    key[word] = (key[word] & ~mask) | (((uint32_t)(choice < 0 ? 0 : choice) << shift) & mask);
  }
  if (f->bad || matched != n_want)
    return -1;
  for (int p = 0; p < n_programs; p++) {
    bool same = true;
    for (int w = 0; w < static_words && same; w++)
      same = rd(f, keys + 4 * ((uint64_t)(static_words + dynamic_words) * p + w), 4) == key[w];
    if (same && !f->bad)
      return p;
  }
  return -1;
}

/* a program's vertex and fragment stage, as offsets into the file */
typedef struct { uint64_t control, control_size, code, code_size; } stage_at_t;

static bool stage_of(file_t *f, uint64_t bnsh, uint64_t binary, int desc_at, stage_at_t *out)
{
  uint64_t desc = bnsh + rd(f, binary + desc_at, 8);
  out->control = bnsh + rd(f, desc + 0x08, 8);
  out->code = bnsh + rd(f, desc + 0x10, 8);
  out->code_size = rd(f, desc + 0x18, 4);
  out->control_size = rd(f, desc + 0x1c, 4);
  return !f->bad && out->control_size >= 0x48 && out->code_size && in_file(f, out->control, out->control_size) &&
         in_file(f, out->code, out->code_size) && rd(f, out->control, 4) == CONTROL_MAGIC &&
         rd(f, out->control + 0x44, 4) == out->code_size;
}

static bool program_at(file_t *f, uint64_t sm, int p, stage_at_t stages[2])
{
  uint64_t variation = rd(f, rd(f, sm + 0x88, 8) + 0x40 * (uint64_t)p + 0x20, 8);
  uint64_t bnsh = rd(f, sm + 0xa8, 8);
  uint64_t binary = bnsh + rd(f, variation + 0x10, 8);
  return !f->bad && stage_of(f, bnsh, binary, 0x08, &stages[0]) && stage_of(f, bnsh, binary, 0x28, &stages[1]);
}

bool uishader_load(void)
{
  if (loaded)
    return true;
  size_t size = 0;
  uint8_t *data = gamefile_read_zs(SHADER_FILE, &size);
  if (!data)
    return false; /* gamefile logged why */
  file_t f = { data, size, false };
  static const option_t solid[] = { { "o_IsProceduralShapeBlendedWithPaneColor", 1 }, { "o_PaneCenter", 4 } };
  static const option_t texture[] = { { "o_IsProceduralShapeBlendedWithPaneColor", 1 }, { "o_PaneCenter", 4 },
                                      { "Texture_ActiveCount", 1 } };
  bool ok = size > 0x50 && rd(&f, 0, 4) == 0x41485346 /* "FSHA" */;
  uint64_t sm = ok ? rd(&f, 0x48, 8) : 0;
  int found[UI_PROGRAMS] = { ok ? find_program(&f, sm, solid, 2) : -1, ok ? find_program(&f, sm, texture, 3) : -1 };
  stage_at_t at[UI_PROGRAMS][2];
  size_t total = 0;
  for (int i = 0; i < UI_PROGRAMS && ok; i++) {
    ok = found[i] >= 0 && program_at(&f, sm, found[i], at[i]);
    for (int s = 0; s < 2 && ok; s++)
      total += at[i][s].control_size + at[i][s].code_size + 32;
  }
  if (ok)
    ok = (kept = aligned_alloc(16, (total + 15) & ~(size_t)15)) != 0;
  if (!ok) {
    log_printf("ui shader: %s isn't the shader this module knows", SHADER_FILE);
    free(data);
    return false;
  }
  size_t used = 0;
  for (int i = 0; i < UI_PROGRAMS; i++) {
    uishader_stage_t *stages[2] = { &programs[i].vertex, &programs[i].fragment };
    for (int s = 0; s < 2; s++) {
      uint8_t *control = kept + used;
      __builtin_memcpy(control, data + at[i][s].control, at[i][s].control_size);
      used += (at[i][s].control_size + 15) & ~(size_t)15;
      uint8_t *code = kept + used;
      __builtin_memcpy(code, data + at[i][s].code, at[i][s].code_size);
      used += (at[i][s].code_size + 15) & ~(size_t)15;
      *stages[s] = (uishader_stage_t){ control, (uint32_t)at[i][s].control_size, code, (uint32_t)at[i][s].code_size };
    }
  }
  free(data);
  __atomic_store_n(&loaded, true, __ATOMIC_RELEASE);
  log_printf("ui shader: programs %d and %d of the game's MergeShader", found[0], found[1]);
  return true;
}

const uishader_program_t *uishader_program(int which)
{
  if (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE) || which < 0 || which >= UI_PROGRAMS)
    return 0;
  return &programs[which];
}
