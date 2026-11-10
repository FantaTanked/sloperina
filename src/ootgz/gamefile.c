/* Reading the game's RomFS files (see gamefile.h). The game mounts its RomFS as "content:" (main has
 * "content://" paths); nn::fs reads it for us like the SD card. Its .zs files are zstd frames, most
 * with a dictionary from Pack/ZsDic.pack.zs (a plain zstd frame holding a SARC of *.zsdic: zs.zsdic
 * is ID 1, the one the fonts and the UI shader use). */
#include "gamefile.h"

#include <stdbool.h>

#include "sdfs.h"
#include "sys.h"
#include "zstd.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);                             /* from main */

#define FILE_ALIGN 16
#define MAX_FILE (64u << 20) /* nothing we read is this big; a larger size is a bad read */
#define ZSDIC_PATH GAME_ROM "/Pack/ZsDic.pack.zs"
#define MAX_DICTS 8

uint8_t *gamefile_read(const char *path, size_t *size_out)
{
  fs_file_t file;
  if (fs_open(&file, path, FS_READ) != 0)
    return 0;
  int64_t size = 0;
  uint8_t *data = 0;
  if (fs_size(&size, file) == 0 && size > 0 && (uint64_t)size <= MAX_FILE) {
    data = aligned_alloc(FILE_ALIGN, ((size_t)size + FILE_ALIGN - 1) & ~(size_t)(FILE_ALIGN - 1));
    size_t got = 0;
    if (data && (fs_read(&got, file, 0, data, (size_t)size) != 0 || got != (size_t)size)) {
      free(data);
      data = 0;
    }
  }
  fs_close(file);
  if (data && size_out)
    *size_out = (size_t)size;
  return data;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool same_name(const char *a, const char *b, size_t room)
{
  for (size_t i = 0; i < room; i++) {
    if (a[i] != b[i])
      return false;
    if (!a[i])
      return true;
  }
  return false;
}

/* SARC (little endian): header 0x14, SFAT (0xC + 0x10 per node: hash, name attributes, start, end),
 * SFNT (8, then the names, each at attributes' low 16 bits x 4), data from the header's offset */
const uint8_t *gamefile_sarc_member(const uint8_t *sarc, size_t size, const char *name, size_t *size_out)
{
  if (size < 0x20 || rd32(sarc) != 0x43524153 /* "SARC" */ || rd16(sarc + 6) != 0xFEFF)
    return 0;
  uint32_t data_at = rd32(sarc + 0xC);
  uint16_t nodes = rd16(sarc + 0x14 + 6);
  size_t names_at = 0x20 + (size_t)nodes * 0x10 + 8;
  if (names_at > size || data_at > size)
    return 0;
  for (unsigned i = 0; i < nodes; i++) {
    const uint8_t *node = sarc + 0x20 + i * 0x10;
    size_t at = names_at + (size_t)(rd32(node + 4) & 0xFFFF) * 4;
    uint32_t start = rd32(node + 8), end = rd32(node + 12);
    if (at >= size || end < start || data_at + (size_t)end > size)
      continue;
    if (same_name((const char *)sarc + at, name, size - at)) {
      if (size_out)
        *size_out = end - start;
      return sarc + data_at + start;
    }
  }
  return 0;
}

/* the game's dictionaries: one is loaded the first time a frame asks for its ID, and kept until
 * gamefile_release (the fonts need only zs.zsdic, ID 1, and only at start-up) */
static struct { uint32_t id; zstd_dict_t *dict; } dicts[MAX_DICTS];
static int dict_count;

static size_t padded(size_t n) { return (n + FILE_ALIGN - 1) & ~(size_t)(FILE_ALIGN - 1); }

static const zstd_dict_t *load_dict(uint32_t id)
{
  if (dict_count == MAX_DICTS)
    return 0;
  size_t packed_size = 0;
  uint8_t *packed = gamefile_read(ZSDIC_PATH, &packed_size);
  if (!packed) {
    log_printf("game files: can't read " ZSDIC_PATH);
    return 0;
  }
  size_t size = zstd_content_size(packed, packed_size);
  const char *why = "no size in its frame header"; /* not menu text */
  uint8_t *sarc = 0;
  if (size > MAX_FILE)
    why = "its frame header claims too much"; /* not menu text */
  else if (size && (sarc = aligned_alloc(FILE_ALIGN, padded(size))) == 0)
    why = "out of memory";
  else if (sarc && zstd_decompress(sarc, size, packed, packed_size, 0, &why) != size) {
    free(sarc);
    sarc = 0;
  }
  free(packed);
  if (!sarc) {
    log_printf("game files: " ZSDIC_PATH ": %s", why);
    return 0;
  }
  zstd_dict_t *dict = 0;
  static const char *const names[] = { "zs.zsdic", "pack.zsdic", "bcett.byml.zsdic", "bkprb.zsdic" };
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names) && !dict; i++) {
    size_t n = 0;
    const uint8_t *member = gamefile_sarc_member(sarc, size, names[i], &n);
    if (member && n >= 8 && rd32(member) == 0xEC30A437 /* the zstd dictionary magic */ && rd32(member + 4) == id)
      dict = zstd_dict_load(member, n); /* it copies what it keeps */
  }
  free(sarc);
  if (!dict) {
    log_printf("game files: no zstd dictionary %u in " ZSDIC_PATH, (unsigned)id);
    return 0;
  }
  dicts[dict_count].id = id;
  dicts[dict_count].dict = dict;
  dict_count++;
  return dict;
}

static const zstd_dict_t *dict_for(uint32_t id)
{
  for (int i = 0; i < dict_count; i++)
    if (dicts[i].id == id)
      return dicts[i].dict;
  return load_dict(id);
}

void gamefile_release(void)
{
  while (dict_count)
    zstd_dict_free(dicts[--dict_count].dict);
}

uint8_t *gamefile_read_zs(const char *path, size_t *size_out)
{
  size_t packed_size = 0;
  uint8_t *packed = gamefile_read(path, &packed_size);
  if (!packed) {
    log_printf("game files: can't read %s", path);
    return 0;
  }
  const char *why = 0;
  uint8_t *out = 0;
  size_t size = zstd_content_size(packed, packed_size);
  uint32_t id = zstd_frame_dict_id(packed, packed_size);
  const zstd_dict_t *dict = 0;
  if (!size)
    why = "no size in its frame header"; /* not menu text */
  else if (size > MAX_FILE)
    why = "its frame header claims too much"; /* not menu text */
  else if (id && (dict = dict_for(id)) == 0)
    why = "its zstd dictionary didn't load"; /* not menu text */
  else if ((out = aligned_alloc(FILE_ALIGN, padded(size))) == 0)
    why = "out of memory";
  else if (zstd_decompress(out, size, packed, packed_size, dict, &why) != size) {
    free(out);
    out = 0;
  }
  free(packed);
  if (!out) {
    log_printf("game files: %s: %s", path, why ? why : "error");
    return 0;
  }
  if (size_out)
    *size_out = size;
  return out;
}
