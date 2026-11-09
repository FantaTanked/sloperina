/* Phive's collision layer table.
 *
 * At boot the game reads PhiveConfig's LayerEntityFilterTable (Bootup pack, Phive/Config/PhiveConfig.byml:
 * a 22 x 22 table of 0, 1 or 2 per pair of LayerEntityCollection layers) into two arrays of 22 u64 masks,
 * one bit per layer it hits: one for the entries that are 1 or 2, one for the entries that are 2
 * (main+0xca326c). Both tables are symmetric. The arrays are found by their contents, a slice of the heap
 * per frame (searching it all at once held the game for 21 s). */
#include "layers.h"

#include "link.h"
#include "sys.h"

#define N_LAYERS 22
#define LAYER_PLAYER 3
#define TABLE_BYTES (N_LAYERS * 8)
#define MAX_COPIES 8
#define SCAN_PER_FRAME (8u << 20)
#define MEMORY_TYPE_MASK 0xFF
#define MEMORY_TYPE_NORMAL 0x05 /* ordinary heap memory */

/* the masks built from this version's PhiveConfig: entries 1 or 2, and entries 2 */
static const uint64_t mask_hits[N_LAYERS] = {
  0x39a37f, 0x29837f, 0x3fbf7f, 0x2983ff, 0x2983ff, 0x16b21f, 0x37be1f, 0x26bc18, 0x31a21f, 0x17bf7f, 0x0882c4,
  0x0282c4, 0x0082e4, 0x0083e5, 0x000000, 0x3f3fff, 0x00835f, 0x028ae4, 0x0482e4, 0x00841f, 0x008365, 0x0081df,
};
static const uint64_t mask_hits_2[N_LAYERS] = {
  0x38235f, 0x280347, 0x3f3f5f, 0x2803dd, 0x2803dd, 0x160000, 0x373e1f, 0x263c18, 0x31221f, 0x173f5f, 0x0802c4,
  0x0202c4, 0x0002c4, 0x0003c5, 0x000000, 0x000000, 0x000344, 0x020ae4, 0x0402e4, 0x00041f, 0x000365, 0x0001df,
};

/* GroundObject, GroundHighRes, GroundHighResSmooth, GroundHighResRough, GroundStaticTree */
static const int ground_layers[] = { 2, 6, 7, 8, 9 };

static struct {
  volatile uint64_t *masks;
  const uint64_t *original; /* mask_hits or mask_hits_2 */
} copies[MAX_COPIES];
static int n_copies;
static bool ground_off;

static enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE } scan_state;
static uintptr_t scan_pos;  /* the next address to look at */
static uintptr_t scan_end;  /* the end of the heap region scan_pos is in (0: find the next region) */
static size_t scan_bytes;
static uint64_t scan_start_tick;

static bool same_masks(uintptr_t addr, const uint64_t *masks)
{
  if (!sys_mapped(addr, TABLE_BYTES, SYS_PERM_R | SYS_PERM_W))
    return false;
  const volatile uint64_t *have = (const volatile uint64_t *)addr;
  for (int i = 0; i < N_LAYERS; i++)
    if (have[i] != masks[i])
      return false;
  return true;
}

static void note_copy(uintptr_t addr, const uint64_t *masks)
{
  if (n_copies < MAX_COPIES && same_masks(addr, masks)) {
    copies[n_copies].masks = (volatile uint64_t *)addr;
    copies[n_copies].original = masks;
    log_printf("layers: copy %d at %#lx", n_copies, (unsigned long)addr);
    n_copies++;
  }
}

/* The next ordinary read-write heap region at or after scan_pos; false at the end of the address space. */
static bool next_region(void)
{
  sys_meminfo_t info;
  while (sys_query_memory(&info, scan_pos) == 0 && info.size) {
    uintptr_t end = info.addr + info.size;
    if ((info.type & MEMORY_TYPE_MASK) == MEMORY_TYPE_NORMAL && (info.perm & 3) == 3) {
      scan_end = end;
      return true;
    }
    if (end <= scan_pos)
      break;
    scan_pos = end;
  }
  return false;
}

static void scan_finish(void)
{
  scan_state = SCAN_DONE;
  log_printf("layers: %d copies of the layer table, searched %u MiB in %llu ms", n_copies,
             (unsigned)(scan_bytes >> 20), (unsigned long long)((sys_tick() - scan_start_tick) * 1000 / sys_tick_freq()));
}

static void scan_step(void)
{
  size_t budget = SCAN_PER_FRAME;
  while (budget) {
    if (!scan_end && !next_region()) {
      scan_finish();
      return;
    }
    uintptr_t stop = scan_end - scan_pos > budget ? scan_pos + budget : scan_end;
    for (uintptr_t p = scan_pos; p + 8 <= stop; p += 8) {
      uint64_t word = *(const volatile uint64_t *)p;
      if (word == mask_hits[0])
        note_copy(p, mask_hits);
      else if (word == mask_hits_2[0])
        note_copy(p, mask_hits_2);
    }
    budget -= stop - scan_pos;
    scan_bytes += stop - scan_pos;
    scan_pos = stop;
    if (scan_pos >= scan_end)
      scan_end = 0;
  }
}

void layers_frame(bool wanted)
{
  if (wanted && scan_state == SCAN_IDLE) {
    scan_state = SCAN_RUNNING;
    scan_pos = scan_end = 0;
    scan_bytes = 0;
    n_copies = 0;
    scan_start_tick = sys_tick();
    log_printf("layers: searching the heap for the layer table");
  }
  if (scan_state == SCAN_RUNNING)
    scan_step();
}

static void apply(bool on)
{
  uint64_t ground_bits = 0;
  for (unsigned i = 0; i < sizeof(ground_layers) / sizeof(ground_layers[0]); i++)
    ground_bits |= 1ull << ground_layers[i];
  for (int c = 0; c < n_copies; c++) {
    volatile uint64_t *masks = copies[c].masks;
    const uint64_t *original = copies[c].original;
    if (!on && !same_masks((uintptr_t)masks, original)) { /* not ours to change any more */
      log_printf("layers: copy %d at %#lx changed, left alone", c, (unsigned long)masks);
      copies[c--] = copies[--n_copies];
      continue;
    }
    masks[LAYER_PLAYER] = on ? original[LAYER_PLAYER] : original[LAYER_PLAYER] & ~ground_bits;
    for (unsigned i = 0; i < sizeof(ground_layers) / sizeof(ground_layers[0]); i++) {
      int layer = ground_layers[i];
      masks[layer] = on ? original[layer] : original[layer] & ~(1ull << LAYER_PLAYER);
    }
  }
}

bool layers_player_hits_ground(bool on)
{
  if (on && !ground_off)
    return true;
  if (scan_state != SCAN_DONE || !n_copies)
    return false;
  apply(on);
  if (!n_copies)
    return false;
  ground_off = !on;
  log_printf("layers: Link %s the ground and wall layers", on ? "hits" : "no longer hits");
  return true;
}
