/* The game's pictures of its controllers (see gameimage.h).
 *
 * content:/UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs is one zstd frame (no dictionary,
 * an 8 MiB window) of a BNTX: its header, then each texture's packaged data. zstd.c's stream decodes it
 * a block at a time; the header is kept until its texture records (BRTI) are read, then only the wanted
 * textures' bytes are copied, into memory gpuui.c makes a pool of, and the decoding stops after the last
 * of them (about half way). The pictures are found by name, as the game finds them. BNTX, little-endian,
 * offsets from the file's start:
 *   +0x20 the platform ("NX  ", or "Ounc" on the Switch 2), +0x24 the texture count, +0x28 where the records' offsets are (u64 each), +0x30 the data
 *     block's offset (everything before it is header)
 *   a record ("BRTI"): +0x1c the image format (0x2d01 / 0x2d06: ASTC 4x4 unorm / sRGB; 0x1d01: BC4), +0x24
 *     its width, +0x28 its height, +0x34 its packaged layout (8 bytes), +0x50 its data's size, +0x58 its
 *     channels' sources (as NVN's swizzle: 0 zero, 1 one, 2..5 r, g, b, a), +0x60 its name's offset (a u16
 *     length, then the characters), +0x70 where its levels' data offsets are (u64 each).
 * A picture that isn't there (another version of the game) is left out; the controllers are needed.
 *
 * The items' icons come after: UI/Tex/Icon/<name>.bntx.zs, each one frame (no dictionary) of a BNTX with
 * one ASTC 4x4 texture of that name (341 to 1366 pixels square, about 13 MB in all). Kept as they are they'd
 * take more of the game's heap than it can spare, so each is decoded as it's read (astc.c), its blocks put
 * in their places (the packaged layout: GOBs of 64 bytes x 8 rows, stacked 2^layout[0] high in each column
 * of them; on the Switch 2 a GOB's 16-byte sectors go row 0-1 / column / row 2 / column 1, found against
 * the GPU's own drawing of one), and shrunk to GPU_ICON_PX square into gpuui's icon atlas (1.6 MB). */
#include "gameimage.h"

#include <stddef.h>
#include <stdint.h>

#include "gamefile.h"
#include "gpuui.h"
#include "sdfs.h"
#include "sys.h"
#include "astc.h"
#include "zstd.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);                             /* from main */

#define ARCHIVE GAME_ROM "/UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"
#define ICON_DIR GAME_ROM "/UI/Tex/Icon/"
#define HEADER_MAX (4u << 20)    /* the most header kept */
#define IMAGE_MAX (4u << 20)     /* a picture's most data */
#define IMAGE_ALIGN 0x10000      /* each picture's place in the memory: NVN's strictest texture alignment */
#define PAGE 0x1000
#define STEP_BYTES (64u << 10)   /* decoded between looks at the clock */
#define ICON_STEP_BYTES (8u << 10) /* an icon's (its blocks decoded too) */
#define STEP_MS 2                /* a frame's time for it */
/* NVN's ASTC 4x4 (the data shown as stored, no sRGB decode, as everything else is drawn) and BC4 */
#define NVN_FORMAT_ASTC_4X4 0x79
#define NVN_FORMAT_BC4 0x49
#define BNTX_ASTC_4X4_UNORM 0x2d01
#define BNTX_ASTC_4X4_SRGB 0x2d06
#define BNTX_BC4_UNORM 0x1d01

static const char *const names[GAMEIMAGES] = {
  [GAMEIMAGE_PRO] = "Controller_Pro", [GAMEIMAGE_GRIP] = "Controller_Grip", [GAMEIMAGE_CONSOLE] = "Controller_Console",
  [GAMEIMAGE_CURSOR_LINE] = "OptionBtnThinCursorLine_00", [GAMEIMAGE_CURSOR_GLOW] = "OptionBtnThinCursorGlow_00",
  [GAMEIMAGE_LINE] = "SystemLine_00", [GAMEIMAGE_SCROLL_BAR] = "ScrollBar_00",
  [GAMEIMAGE_SCROLL_BASE] = "ScrollBarBase_00", [GAMEPIC_HEART] = "CollectHeartBase_00",
  [GAMEIMAGE_GAUGE_BASE] = "MpGaugeBase_00", [GAMEIMAGE_GAUGE_FRAME] = "MpGaugeBaseFrame_00",
  [GAMEIMAGE_GAUGE_FILL] = "MpGaugeCenter_00", [GAMEIMAGE_GAUGE_SHADOW] = "MpGaugeSh_00",
  [GAMEIMAGE_PLATE] = "NumBaseMiniGame_00", [GAMEIMAGE_PLATE_RIM] = "NumBaseMiniGameDeco_00",
  /* the icons: their files' names, and their textures' */
  [GAMEPIC_RUPEE] = "Obj_Rupee_Green", [GAMEPIC_MAGIC] = "Item_FairyBless_MP", [GAMEPIC_STICK] = "Item_DekuStick",
  [GAMEPIC_NUT] = "Item_DekuNut", [GAMEPIC_SEED] = "Item_DekuSeed", [GAMEPIC_BOMB] = "Item_Bomb",
  [GAMEPIC_ARROW] = "Item_Arrow_Normal", [GAMEPIC_BOMBCHU] = "Item_Bombchu", [GAMEPIC_BEAN] = "Item_MagicBean",
  [GAMEPIC_BOOMERANG] = "Item_Boomerang", [GAMEPIC_SLINGSHOT] = "Weapon_Range_SlingShot",
  [GAMEPIC_BOW] = "Weapon_Range_Bow", [GAMEPIC_HAMMER] = "Item_Hammer", [GAMEPIC_ARROW_FIRE] = "Item_Arrow_Fire",
  [GAMEPIC_ARROW_ICE] = "Item_Arrow_Ice", [GAMEPIC_ARROW_LIGHT] = "Item_Arrow_Light",
  [GAMEPIC_HOOKSHOT] = "Item_Hookshot", [GAMEPIC_DIN] = "Item_DinFire", [GAMEPIC_FARORE] = "Item_FaroreWind",
  [GAMEPIC_NAYRU] = "Item_NayruLove", [GAMEPIC_OCARINA_FAIRY] = "Item_Ocarina_Fairy",
  [GAMEPIC_OCARINA_TIME] = "Item_Ocarina_Time", [GAMEPIC_BOTTLE] = "Item_Bottle",
  [GAMEPIC_SWORD_KOKIRI] = "Weapon_Sword_Kokiri", [GAMEPIC_SWORD_MASTER] = "Weapon_Sword_Master",
  [GAMEPIC_SWORD_GIANT] = "Weapon_Sword_Giant", [GAMEPIC_SHIELD_DEKU] = "Weapon_Shield_Deku",
  [GAMEPIC_SHIELD_HYLIAN] = "Weapon_Shield_Hylian", [GAMEPIC_SHIELD_MIRROR] = "Weapon_Shield_Mirror",
  [GAMEPIC_TUNIC_GORON] = "Armor_Adult_Goron", [GAMEPIC_TUNIC_ZORA] = "Armor_Adult_Zora",
  [GAMEPIC_BOOTS_IRON] = "Armor_Adult_HeavyBoot", [GAMEPIC_BOOTS_HOVER] = "Armor_Adult_HoverBoot",
  [GAMEPIC_LENS] = "Armor_GlassTruth", [GAMEPIC_AGONY] = "Item_AgonyStone", [GAMEPIC_GERUDO_CARD] = "Item_Token_Gerudo",
  [GAMEPIC_BRACELET] = "Armor_Child_Gauntlet_Goron", [GAMEPIC_GAUNTLETS] = "Armor_Adult_Gauntlet_Silver",
  [GAMEPIC_SCALE] = "Item_Scale_Silver", [GAMEPIC_SKULLTULA] = "Item_Token_Gold",
  [GAMEPIC_DEFENSE] = "Item_FairyBless_DefenseDouble", [GAMEPIC_WALLET] = "Item_Wallet_Small",
  [GAMEPIC_BOMB_BAG] = "Item_BombBag_Small", [GAMEPIC_QUIVER] = "Item_Quiver_Small",
  [GAMEPIC_BULLET_BAG] = "Item_BulletBag_Small", [GAMEPIC_STICK_BAG] = "Item_StickBag_Small",
  [GAMEPIC_NUT_BAG] = "Item_NutBag_Small", [GAMEPIC_LONGSHOT] = "Item_Longshot",
  [GAMEPIC_SWORD_BROKEN] = "Weapon_Sword_BrokenGiant", [GAMEPIC_SWORD_BIGGORON] = "Weapon_Sword_Biggoron",
  [GAMEPIC_GAUNTLETS_GOLD] = "Armor_Adult_Gauntlet_Gold", [GAMEPIC_SCALE_GOLD] = "Item_Scale_Gold",
  [GAMEPIC_MAGIC_DOUBLE] = "Item_FairyBless_MPDouble", [GAMEPIC_POTION_GREEN] = "Item_Bottle_GreenPotion",
};

/* the shared archive (its pictures [0, GAMEIMAGES_SHARED)), then the icons one file each */
static enum { IDLE, SHARED, ICONS, DONE, FAILED } state;
static int lo, hi;    /* the pictures the file being read has: [lo, hi) */
static int icon;      /* ICONS: the icon being read */
static fs_file_t file;
static bool file_open;
static zstd_stream_t *stream;
static uint8_t *head; /* the file's start, until the records are read */
static uint64_t head_len, head_need;
static bool parsed;
static const char *sink_why; /* the sink gave up: why */
static struct {
  uint64_t at, got; /* where its data is in the file, how much of it is here */
  gpu_image_t gpu;
} want[GAMEIMAGES];
static uint8_t *mem; /* the shared pictures' */
static size_t mem_size;
static int icons_made;
static bool switch2;  /* the file's platform: the Switch 2's ("Ounc") GOB layout */

/* the icon being read: its texture's blocks, a block split between two reads, its shrunk sums */
#define ICON_SLOTS (GPU_ICON_COLS * GPU_ICON_ROWS)
#define ICONS_MEM (2 * ((size_t)GPU_ICON_PX * GPU_ICON_PX * ICON_SLOTS * 4) + 0x20000)
_Static_assert(GAMEIMAGES - GAMEIMAGES_SHARED <= ICON_SLOTS, "an icon slot each");
_Static_assert(GAMEIMAGES <= GPU_ICONS_IMAGE, "the icons' atlas after the pictures");
static uint8_t *icons_mem;
static int tex_w, tex_h, blocks_w, blocks_h, gobs_w, gob_h;
static uint8_t carry[16];
static int carried;
static uint32_t sums[GPU_ICON_PX * GPU_ICON_PX][4];
static uint16_t counts[GPU_ICON_PX * GPU_ICON_PX];
static uint8_t boxes[ICON_SLOTS][4]; /* each icon's covered part (gameimage_icon_box), in its slot's pixels */
#define COVERED_ALPHA 24             /* a pixel at least this opaque is part of the picture */
static int frames;
static uint64_t ticks, decoded;

static uint64_t rd(const uint8_t *p, int bytes)
{
  uint64_t v = 0;
  for (int i = 0; i < bytes; i++)
    v |= (uint64_t)p[i] << (8 * i);
  return v;
}

static size_t round_up(size_t n, size_t to) { return (n + to - 1) / to * to; }

/* n bytes at `at` are in the header (written so a damaged offset can't wrap around) */
static bool in_head(uint64_t at, uint64_t n)
{
  return at <= head_len && n <= head_len - at;
}

/* the length-prefixed name at `at` of the header is `name` */
static bool name_is(uint64_t at, const char *name)
{
  if (!in_head(at, 2))
    return false;
  uint64_t n = rd(head + at, 2);
  if (!in_head(at + 2, n))
    return false;
  for (uint64_t i = 0; i < n; i++)
    if (!name[i] || head[at + 2 + i] != (uint8_t)name[i])
      return false;
  return !name[n];
}

/* the wanted pictures' places from the header's records, and memory for them; false: not the file this
 * knows (sink_why says why) */
static bool parse(void)
{
  sink_why = "its header isn't the one this module knows"; /* not menu text */
  uint32_t platform = head_len >= 0x48 ? (uint32_t)rd(head + 0x20, 4) : 0;
  if (platform != 0x2020584Eu /* "NX  " */ && platform != 0x636E754Fu /* "Ounc": the Switch 2's */)
    return false;
  uint64_t count = rd(head + 0x24, 4), table = rd(head + 0x28, 8);
  if (!in_head(table, 8 * count))
    return false;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t r = rd(head + table + 8 * i, 8);
    if (!in_head(r, 0x78) || rd(head + r, 4) != 0x49545242u /* "BRTI" */)
      return false;
    for (int k = lo; k < hi; k++) {
      if (want[k].gpu.size || !name_is(rd(head + r + 0x60, 8), names[k]))
        continue;
      uint32_t format = (uint32_t)rd(head + r + 0x1c, 4);
      uint64_t levels = rd(head + r + 0x70, 8);
      int width = (int32_t)rd(head + r + 0x24, 4), height = (int32_t)rd(head + r + 0x28, 4);
      size_t size = (size_t)rd(head + r + 0x50, 4);
      int nvn = format == BNTX_BC4_UNORM ? NVN_FORMAT_BC4
                : format == BNTX_ASTC_4X4_UNORM || format == BNTX_ASTC_4X4_SRGB ? NVN_FORMAT_ASTC_4X4 : 0;
      if (!nvn || !in_head(levels, 8) || width <= 0 || height <= 0 || !size || size > IMAGE_MAX)
        return false;
      want[k].at = rd(head + levels, 8);
      if (want[k].at < head_need || want[k].at > UINT64_MAX - size)
        return false;
      want[k].gpu = (gpu_image_t){ 0, size, width, height, nvn, { 0 }, { 0 } };
      __builtin_memcpy(want[k].gpu.layout, head + r + 0x34, 8);
      for (int c = 0; c < 4; c++) {
        uint8_t source = head[r + 0x58 + c];
        want[k].gpu.swizzle[c] = source <= 5 ? source : (uint8_t)(c + 2);
      }
      if (k == GAMEIMAGE_GAUGE_FILL) { /* the magic meter's green as the health's red */
        uint8_t red = want[k].gpu.swizzle[0];
        want[k].gpu.swizzle[0] = want[k].gpu.swizzle[1];
        want[k].gpu.swizzle[1] = red;
      }
    }
  }
  if (state == ICONS) { /* an icon: decoded as it comes (icon_bytes) */
    const gpu_image_t *im = &want[icon].gpu;
    if (!im->size || im->format != NVN_FORMAT_ASTC_4X4) {
      sink_why = im->size ? "not ASTC 4x4" : "no picture of that name in it"; /* not menu text */
      return false;
    }
    tex_w = im->width, tex_h = im->height;
    blocks_w = (tex_w + 3) / 4, blocks_h = (tex_h + 3) / 4;
    gobs_w = (blocks_w * 16 + 63) / 64;
    gob_h = 1 << (im->layout[0] & 7);
    switch2 = platform == 0x636E754Fu;
    carried = 0;
    __builtin_memset(sums, 0, sizeof(sums));
    __builtin_memset(counts, 0, sizeof(counts));
    sink_why = 0;
    return true;
  }
  if (!want[GAMEIMAGE_PRO].gpu.size || !want[GAMEIMAGE_GRIP].gpu.size || !want[GAMEIMAGE_CONSOLE].gpu.size) {
    sink_why = "the controllers' pictures aren't in it"; /* not menu text */
    return false;
  }
  for (int k = lo; k < hi; k++)
    if (!want[k].gpu.size)
      log_printf("game pictures: no %s in this version; left out", names[k]);
  size_t at = 0;
  for (int k = lo; k < hi; k++) {
    want[k].gpu.offset = at;
    at = round_up(at + want[k].gpu.size, IMAGE_ALIGN);
  }
  mem_size = round_up(at, PAGE);
  mem = aligned_alloc(PAGE, mem_size);
  if (!mem) {
    sink_why = "out of memory"; /* not menu text */
    return false;
  }
  __builtin_memset(mem, 0, mem_size);
  sink_why = 0;
  return true;
}

/* every picture of the file found is here (one not found isn't waited for) */
static bool all_here(void)
{
  for (int k = lo; k < hi; k++)
    if (want[k].got < want[k].gpu.size)
      return false;
  return parsed && (state == ICONS || want[GAMEIMAGE_PRO].gpu.size);
}

/* one of the icon's blocks, `off` bytes into its texture: in its place, its texels added to the sums of the
 * shrunk icon's pixels they fall in */
static void icon_block(const uint8_t *block, uint64_t off)
{
  uint64_t per = 512ull * (uint64_t)gob_h, column = off / per % (uint64_t)gobs_w, row = off / per / (uint64_t)gobs_w;
  uint64_t gob = off % per / 512, k = off % 512 / 16; /* the GOB in the column, the 16-byte sector in it */
  int x16 = switch2 ? (int)((k >> 2 & 1) | (k >> 4 & 1) << 1) : (int)((k >> 1 & 1) | (k >> 4 & 1) << 1);
  int y = switch2 ? (int)((k & 3) | (k >> 3 & 1) << 2) : (int)((k & 1) | (k >> 2 & 3) << 1);
  int bx = (int)column * 4 + x16, by = (int)(row * (uint64_t)gob_h + gob) * 8 + y;
  if (bx >= blocks_w || by >= blocks_h)
    return; /* padding */
  uint8_t texels[16][4];
  astc_block(block, texels);
  for (int i = 0; i < 16; i++) {
    int x = bx * 4 + i % 4, yy = by * 4 + i / 4;
    if (x >= tex_w || yy >= tex_h)
      continue;
    int at = yy * GPU_ICON_PX / tex_h * GPU_ICON_PX + x * GPU_ICON_PX / tex_w;
    uint32_t a = texels[i][3];
    sums[at][0] += texels[i][0] * a, sums[at][1] += texels[i][1] * a, sums[at][2] += texels[i][2] * a;
    sums[at][3] += a;
    counts[at]++;
  }
}

/* n bytes of the icon's texture from `off`, in order */
static void icon_bytes(const uint8_t *p, uint64_t off, size_t n)
{
  while (n) {
    if (carried || n < 16) { /* a block split between reads */
      size_t take = 16 - (size_t)carried < n ? 16 - (size_t)carried : n;
      __builtin_memcpy(carry + carried, p, take);
      carried += (int)take, p += take, off += take, n -= take;
      if (carried == 16) {
        icon_block(carry, off - 16);
        carried = 0;
      }
      continue;
    }
    icon_block(p, off);
    p += 16, off += 16, n -= 16;
  }
}

/* the shrunk icon into its slot of the atlas (its colour the average of its texels', weighted by their
 * alpha), and the part of it the picture covers */
static void icon_finish(int slot)
{
  int ax = slot % GPU_ICON_COLS * GPU_ICON_PX, ay = slot / GPU_ICON_COLS * GPU_ICON_PX;
  int x0 = GPU_ICON_PX, y0 = GPU_ICON_PX, x1 = 0, y1 = 0;
  for (int i = 0; i < GPU_ICON_PX * GPU_ICON_PX; i++) {
    uint8_t *px = icons_mem + ((size_t)(ay + i / GPU_ICON_PX) * GPU_ICON_PX * GPU_ICON_COLS + ax + i % GPU_ICON_PX) * 4;
    uint32_t a = sums[i][3];
    for (int c = 0; c < 3; c++)
      px[c] = a ? (uint8_t)(sums[i][c] / a) : 0;
    px[3] = counts[i] ? (uint8_t)(a / counts[i]) : 0;
    if (px[3] >= COVERED_ALPHA) {
      int x = i % GPU_ICON_PX, y = i / GPU_ICON_PX;
      x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0;
      x1 = x + 1 > x1 ? x + 1 : x1, y1 = y + 1 > y1 ? y + 1 : y1;
    }
  }
  if (x1 <= x0 || y1 <= y0) /* nothing covered: all of it */
    x0 = 0, y0 = 0, x1 = GPU_ICON_PX, y1 = GPU_ICON_PX;
  boxes[slot][0] = (uint8_t)x0, boxes[slot][1] = (uint8_t)y0, boxes[slot][2] = (uint8_t)x1, boxes[slot][3] = (uint8_t)y1;
}

void gameimage_icon_box(int slot, float box[4])
{
  for (int i = 0; i < 4; i++)
    box[i] = slot >= 0 && slot < ICON_SLOTS ? (float)boxes[slot][i] / GPU_ICON_PX : (float)(i >= 2);
}

/* the file's bytes [at, at + n), in order: the header kept, then the pictures' data copied (the shared
 * archive's) or decoded (an icon's) */
static bool sink(void *user, uint64_t at, const uint8_t *data, size_t n)
{
  (void)user;
  decoded = at + n;
  if (!parsed) {
    if (!head) { /* the first block: the header's size is in it */
      head_need = at == 0 && n >= 0x38 ? rd(data + 0x30, 8) : 0;
      if (head_need < 0x48 || head_need > HEADER_MAX || !(head = aligned_alloc(16, (size_t)head_need))) {
        sink_why = "its header isn't the one this module knows"; /* not menu text */
        return false;
      }
    }
    if (at < head_need) {
      size_t k = head_need - at < n ? (size_t)(head_need - at) : n;
      __builtin_memcpy(head + at, data, k);
      head_len = at + k;
    }
    if (head_len < head_need)
      return true;
    if (!(parsed = parse()))
      return false;
  }
  for (int k = lo; k < hi; k++) {
    uint64_t from = at > want[k].at ? at : want[k].at;
    uint64_t to = at + n < want[k].at + want[k].gpu.size ? at + n : want[k].at + want[k].gpu.size;
    if (from < to) {
      if (state == ICONS)
        icon_bytes(data + (from - at), from - want[k].at, (size_t)(to - from));
      else
        __builtin_memcpy(mem + want[k].gpu.offset + (from - want[k].at), data + (from - at), (size_t)(to - from));
      want[k].got += to - from;
    }
  }
  return !all_here();
}

static size_t read_file(void *user, uint64_t at, void *buf, size_t n)
{
  (void)user;
  size_t got = 0;
  return fs_read(&got, file, (int64_t)at, buf, n) == 0 ? got : 0;
}

static void close_all(void)
{
  zstd_stream_free(stream);
  stream = 0;
  if (file_open)
    fs_close(file);
  file_open = false;
  free(head);
  head = 0;
  head_len = head_need = 0;
  parsed = false;
}

static void fail(const char *why)
{
  log_printf("game pictures: %s: %s (after %u KiB); the input display keeps its glyphs", ARCHIVE,
             why ? why : "error", (unsigned)(decoded >> 10));
  close_all();
  free(mem);
  mem = 0;
  state = FAILED;
}

/* an icon's file: content:/UI/Tex/Icon/<name>.bntx.zs */
static void icon_path(int k, char *buf, size_t len)
{
  size_t at = 0;
  const char *parts[3] = { ICON_DIR, names[k], ".bntx.zs" };
  for (int p = 0; p < 3; p++)
    for (const char *c = parts[p]; *c && at + 1 < len; c++)
      buf[at++] = *c;
  buf[at] = 0;
}

/* opens a file to read with a stream; false (logged) if it can't */
static bool open_stream(const char *path)
{
  const char *why = 0;
  if (fs_open(&file, path, FS_READ) != 0)
    return false;
  file_open = true;
  stream = zstd_stream_new(read_file, 0, &why);
  if (!stream) {
    log_printf("game pictures: %s: %s", path, why ? why : "can't read it");
    close_all();
    return false;
  }
  return true;
}

/* the next icon from `from` whose file is there, open to read; GAMEIMAGES when there are no more */
static int next_icon(int from)
{
  char path[128];
  for (int k = from; k < GAMEIMAGES; k++) {
    icon_path(k, path, sizeof(path));
    if (open_stream(path)) {
      lo = k, hi = k + 1;
      return k;
    }
    log_printf("game pictures: no icon %s in this version; left out", names[k]);
  }
  return GAMEIMAGES;
}

void gameimage_frame(void)
{
  if (state == DONE || state == FAILED || !gpuui_ready())
    return;
  const char *why = 0;
  if (state == IDLE) {
    state = SHARED;
    lo = 0, hi = GAMEIMAGES_SHARED;
    if (fs_open(&file, ARCHIVE, FS_READ) != 0) {
      fail("can't open it"); /* not menu text */
      return;
    }
    file_open = true;
    stream = zstd_stream_new(read_file, 0, &why);
    if (!stream) {
      fail(why);
      return;
    }
  }
  uint64_t start = sys_tick(), limit = sys_tick_freq() * STEP_MS / 1000;
  int more = 1;
  while (more == 1 && sys_tick() - start < limit)
    more = zstd_stream_step(stream, state == ICONS ? ICON_STEP_BYTES : STEP_BYTES, sink, 0, &why);
  ticks += sys_tick() - start;
  frames++;
  if (more == 1)
    return;
  if (state == SHARED) {
    if (more < 0 || !all_here()) {
      fail(more < 0 ? why : sink_why ? sink_why : "it ended before the pictures"); /* not menu text */
      return;
    }
    close_all();
    gpu_image_t images[GAMEIMAGES_SHARED];
    for (int k = 0; k < GAMEIMAGES_SHARED; k++)
      images[k] = want[k].gpu;
    gpuui_images_publish(0, 0, mem, mem_size, images, GAMEIMAGES_SHARED);
    log_printf("game pictures: loaded in %u ms over %d frames", (unsigned)(ticks * 1000 / sys_tick_freq()), frames);
    if (!(icons_mem = aligned_alloc(PAGE, round_up(ICONS_MEM, PAGE)))) {
      log_printf("game pictures: no memory for the icons");
      state = DONE;
      return;
    }
    __builtin_memset(icons_mem, 0, round_up(ICONS_MEM, PAGE));
    gpuui_icons_publish(icons_mem, round_up(ICONS_MEM, PAGE));
    state = ICONS; /* (parse() and sink() go by it) */
    if ((icon = next_icon(GAMEIMAGES_SHARED)) >= GAMEIMAGES)
      state = DONE;
    return;
  }
  /* an icon read: into its slot, handed over at once (or left out) */
  if (more < 0 || !all_here() || !want[icon].gpu.size) {
    log_printf("game pictures: icon %s: %s; left out", names[icon],
               more < 0 && why ? why : sink_why ? sink_why : "no picture of that name in it");
  } else {
    icon_finish(icon - GAMEIMAGES_SHARED);
    gpuui_icon_filled(icon - GAMEIMAGES_SHARED);
    icons_made++;
  }
  close_all();
  if ((icon = next_icon(icon + 1)) >= GAMEIMAGES) {
    state = DONE;
    log_printf("game pictures: %d icons; in all %u ms over %d frames", icons_made,
               (unsigned)(ticks * 1000 / sys_tick_freq()), frames);
  }
}
