/* mkips: writes ootgz's IPS32 patch for main (build host tool).
 *
 *   mkips <out.ips>
 *
 * The patch (offsets are main offsets + 0x100, the NSO header, as Atmosphère and Lichii/Eden
 * expect; see FINDINGS §3):
 *   CAVE_START          "OOTGZ\0\0\0" + u32 version + 4 zero bytes (the module checks this)
 *   CAVE_START + 0x40   stub:  adrp x16, slot ; ldr x16, [x16, #slot_lo]
 *                              cbz  x16, 1f   ; br  x16
 *                          1:  b    unlock               (original behaviour until hooked)
 *   FRAME_SITE          b stub                            (was: b unlock, a tail call)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ootgz/ootgz_version.h"
#include "../ootgz/z64hd_v131072.h"

#define NSO_HEADER 0x100

#define X16         16 /* the stub's scratch register (IP0: free to clobber at a branch) */
#define STUB_INSNS  5
#define STUB_SIZE   (STUB_INSNS * 4)
#define MARKER_SIZE 16 /* OOTGZ_MARKER (8), the version (4), 4 zero bytes */

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
  _Static_assert((Z64HD_HOOK_SLOT & 7) == 0, "slot must be 8-aligned");
  _Static_assert(Z64HD_CAVE_START + OOTGZ_STUB_OFF + STUB_SIZE <= Z64HD_CAVE_END, "stub outside cave");

  uint8_t marker[MARKER_SIZE] = { 0 };
  memcpy(marker, OOTGZ_MARKER, 8);
  put_le32(marker + 8, OOTGZ_PATCH_VERSION);

  uint32_t stub_at = Z64HD_CAVE_START + OOTGZ_STUB_OFF;
  uint32_t code[STUB_INSNS] = {
    insn_adrp(X16, stub_at, Z64HD_HOOK_SLOT),
    insn_ldr_x(X16, X16, Z64HD_HOOK_SLOT & 0xFFF),
    insn_cbz_x(X16, stub_at + 8, stub_at + 16), /* no hook yet: to the original branch */
    insn_br(X16),
    insn_b(stub_at + 16, Z64HD_GD_UNLOCK),
  };
  uint8_t stub[STUB_SIZE];
  for (int i = 0; i < STUB_INSNS; i++)
    put_le32(stub + 4 * i, code[i]);

  uint8_t site[4];
  put_le32(site, insn_b(Z64HD_FRAME_SITE, stub_at));

  FILE *f = fopen(argv[1], "wb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fwrite("IPS32", 1, 5, f);
  write_record(f, Z64HD_CAVE_START, marker, sizeof(marker));
  write_record(f, stub_at, stub, sizeof(stub));
  write_record(f, Z64HD_FRAME_SITE, site, sizeof(site));
  fwrite("EEOF", 1, 4, f);
  if (fclose(f) != 0) {
    perror(argv[1]);
    return 1;
  }
  return 0;
}
