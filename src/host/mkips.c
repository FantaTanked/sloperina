/* mkips: writes ootgz's IPS32 patch for main (build host tool).
 *
 *   mkips <out.ips>
 *
 * The patch (offsets are main offsets + 0x100, the NSO header, as Atmosphère and Lichii/Eden
 * expect; see FINDINGS §3):
 *   CAVE_START          "OOTGZ\0\0\0" + u32 version + u32 revision (the module checks this)
 *   CAVE_START + 0x40   stub:  adrp x16, slot ; ldr x16, [x16, #slot_lo]
 *                              cbz  x16, 1f   ; br  x16
 *                          1:  b    unlock               (original behaviour until hooked)
 *   FRAME_SITE          b stub                            (was: b unlock, a tail call)
 *   STORES_OFF          the X+Y store's stub (builds with stores): saves x16/x17, tests the X and Y bits at
 *                       lock_slot and runs the original stp, one single-value store (Y held: str d0 at +0x10,
 *                       X held: str d1 at +0x18) or nothing (both held). Then one stub per single store (Z,
 *                       collision): skips the store while its bit is set, else runs it. Each site becomes `b`.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ootgz/ootgz_version.h"
#include "../ootgz/z64hd_builds.h"

#define NSO_HEADER 0x100

#define X16         16 /* the stub's scratch register (IP0: free to clobber at a branch) */
#define X17         17 /* the store stubs' second scratch register, saved around the test */
#define STUB_INSNS  5
#define STUB_SIZE   (STUB_INSNS * 4)
#define STORE_INSNS 9
#define STORE_SIZE  (STORE_INSNS * 4)
#define PAIR_INSNS  17 /* the X+Y store's stub: four ways to run it, one per lock state */
#define PAIR_SIZE   (PAIR_INSNS * 4)
#define MARKER_SIZE 16 /* OOTGZ_MARKER (8), the version (4), the revision (4) */

#define STP_X16_X17_PRE  0xA9BF47F0u /* stp x16, x17, [sp, #-16]! */
#define LDP_X16_X17_POST 0xA8C147F0u /* ldp x16, x17, [sp], #16 */

/* --- AArch64 instruction encodings --------------------------------------------------------- */

/* b to: a 26-bit word offset */
static uint32_t insn_b(uint32_t from, uint32_t to)
{
  return 0x14000000u | ((uint32_t)((int32_t)(to - from) >> 2) & 0x3FFFFFFu);
}

/* adrp rd, to's 4 KiB page: a 21-bit page offset split into immlo (2 bits) and immhi (19 bits) */
static uint32_t insn_adrp(int rd, uint32_t from, uint32_t to)
{
  int32_t pages = (int32_t)((to & ~0xFFFu) - (from & ~0xFFFu)) >> 12;
  uint32_t immlo = (uint32_t)pages & 3;
  uint32_t immhi = ((uint32_t)pages >> 2) & 0x7FFFF;
  return 0x90000000u | immlo << 29 | immhi << 5 | (uint32_t)rd;
}

/* ldr xt, [xn, #off]: off a multiple of 8 */
static uint32_t insn_ldr_x(int rt, int rn, uint32_t off)
{
  return 0xF9400000u | (off / 8) << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}

/* cbz xt, to: a 19-bit word offset */
static uint32_t insn_cbz_x(int rt, uint32_t from, uint32_t to)
{
  return 0xB4000000u | (((uint32_t)((int32_t)(to - from) >> 2) & 0x7FFFF) << 5) | (uint32_t)rt;
}

static uint32_t insn_br(int rn) { return 0xD61F0000u | (uint32_t)rn << 5; }

static uint32_t insn_tbnz(int rt, uint32_t bit, uint32_t from, uint32_t to)
{
  return 0x37000000u | (bit & 31) << 19 | (((uint32_t)((int32_t)(to - from) >> 2)) & 0x3FFFu) << 5 |
         (uint32_t)rt;
}

/* --- writing the patch -------------------------------------------------------------------- */

static void put_le32(uint8_t *p, uint32_t value)
{
  p[0] = value;
  p[1] = value >> 8;
  p[2] = value >> 16;
  p[3] = value >> 24;
}

/* One IPS32 record: a big-endian u32 file offset and u16 length, then the bytes. */
static void write_record(FILE *f, uint32_t main_off, const uint8_t *data, uint16_t len)
{
  uint32_t offset = main_off + NSO_HEADER;
  uint8_t header[6] = { offset >> 24, offset >> 16, offset >> 8, offset, len >> 8, len };
  fwrite(header, 1, 6, f);
  fwrite(data, 1, len, f);
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "usage: mkips <out.ips>\n");
    return 1;
  }
  const char *name = argv[1];
  for (const char *p = name; *p; p++)
    if (*p == '/' || *p == '\\')
      name = p + 1;
  const struct z64hd_build *build = NULL;
  for (size_t i = 0; i < sizeof(z64hd_builds) / sizeof(*z64hd_builds); i++) {
    const char *id = z64hd_builds[i].build_id;
    size_t len = strlen(id);
    if (strncmp(name, id, len) == 0 && strcmp(name + len, ".ips") == 0) {
      build = &z64hd_builds[i];
      break;
    }
  }
  if (!build) {
    fprintf(stderr, "unknown game build ID in patch filename: %s\n", name);
    return 1;
  }
  if ((build->hook_slot & 7) || build->cave_start + OOTGZ_STUB_OFF + STUB_SIZE > build->cave_end) {
    fprintf(stderr, "invalid hook slot or code cave for %s\n", build->version);
    return 1;
  }
  uint32_t stores_at = build->cave_start + OOTGZ_STORES_OFF;
  uint32_t stores_size = (build->pair ? PAIR_SIZE : 0) + build->store_count * STORE_SIZE;
  if ((build->pair || build->store_count) &&
      ((build->lock_slot & 7) || stores_at + stores_size > build->cave_end)) {
    fprintf(stderr, "invalid lock slot or code cave for %s\n", build->version);
    return 1;
  }

  uint8_t marker[MARKER_SIZE] = { 0 };
  memcpy(marker, OOTGZ_MARKER, 8);
  put_le32(marker + 8, OOTGZ_PATCH_VERSION);
  put_le32(marker + 12, build->revision);

  uint32_t stub_at = build->cave_start + OOTGZ_STUB_OFF;
  uint32_t code[STUB_INSNS] = {
    insn_adrp(X16, stub_at, build->hook_slot),
    insn_ldr_x(X16, X16, build->hook_slot & 0xFFF),
    insn_cbz_x(X16, stub_at + 8, stub_at + 16), /* no hook yet: to the original branch */
    insn_br(X16),
    insn_b(stub_at + 16, build->gd_unlock),
  };
  uint8_t stub[STUB_SIZE];
  for (int i = 0; i < STUB_INSNS; i++)
    put_le32(stub + 4 * i, code[i]);

  uint8_t site[4];
  put_le32(site, insn_b(build->frame_site, stub_at));

  FILE *f = fopen(argv[1], "wb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fwrite("IPS32", 1, 5, f);
  write_record(f, build->cave_start, marker, sizeof(marker));
  write_record(f, stub_at, stub, sizeof(stub));
  write_record(f, build->frame_site, site, sizeof(site));

  uint32_t at = stores_at;
  if (build->pair) {
    const struct z64hd_pair *p = build->pair;
    uint32_t site_at = p->off;
    uint32_t back = site_at + 4;
    uint32_t pair_code[PAIR_INSNS] = {
      STP_X16_X17_PRE,                                       /* +0 */
      insn_adrp(X16, at + 4, build->lock_slot),              /* +4 */
      insn_ldr_x(X17, X16, build->lock_slot & 0xFFF),        /* +8 */
      insn_tbnz(X17, LOCK_BIT_X, at + 12, at + 32),          /* X held: to hx */
      insn_tbnz(X17, LOCK_BIT_Y, at + 16, at + 48),          /* Y held only: to hy */
      LDP_X16_X17_POST,                                      /* neither: the original store */
      p->original,
      insn_b(at + 28, back),
      insn_tbnz(X17, LOCK_BIT_Y, at + 32, at + 60),          /* hx: both held: skip */
      LDP_X16_X17_POST,                                      /* X held: write Y only */
      p->x_held,
      insn_b(at + 44, back),
      LDP_X16_X17_POST,                                      /* hy: write X only */
      p->y_held,
      insn_b(at + 56, back),
      LDP_X16_X17_POST,                                      /* skip */
      insn_b(at + 64, back),
    };
    uint8_t bytes[PAIR_SIZE];
    for (int k = 0; k < PAIR_INSNS; k++)
      put_le32(bytes + 4 * k, pair_code[k]);
    write_record(f, at, bytes, sizeof(bytes));
    uint8_t branch[4];
    put_le32(branch, insn_b(site_at, at));
    write_record(f, site_at, branch, sizeof(branch));
    at += PAIR_SIZE;
  }
  for (uint32_t i = 0; i < build->store_count; i++) {
    const struct z64hd_store *store = &build->stores[i];
    uint32_t site_at = store->off;
    uint32_t skip = at + 7 * 4;
    uint32_t back = site_at + 4;
    uint32_t store_code[STORE_INSNS] = {
      STP_X16_X17_PRE,
      insn_adrp(X16, at + 4, build->lock_slot),
      insn_ldr_x(X17, X16, build->lock_slot & 0xFFF),
      insn_tbnz(X17, store->bit, at + 12, skip),
      LDP_X16_X17_POST,
      store->original,
      insn_b(at + 24, back),
      LDP_X16_X17_POST,
      insn_b(at + 32, back),
    };
    uint8_t bytes[STORE_SIZE];
    for (int k = 0; k < STORE_INSNS; k++)
      put_le32(bytes + 4 * k, store_code[k]);
    write_record(f, at, bytes, sizeof(bytes));

    uint8_t branch[4];
    put_le32(branch, insn_b(site_at, at));
    write_record(f, site_at, branch, sizeof(branch));
    at += STORE_SIZE;
  }
  fwrite("EEOF", 1, 4, f);
  if (fclose(f) != 0) {
    perror(argv[1]);
    return 1;
  }
  return 0;
}
