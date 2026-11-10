#include "sys.h"

#define PREFIX "sloperinaGZ: " /* on each line of the emulator's / Atmosphère's log */
#define PREFIX_LEN (sizeof(PREFIX) - 1)

/* --- system calls ---------------------------------------------------------------------- */

uint32_t sys_query_memory(sys_meminfo_t *info, uint64_t addr)
{
  register uint64_t x0 __asm__("x0") = (uint64_t)info;
  register uint64_t x1 __asm__("x1");
  register uint64_t x2 __asm__("x2") = addr;
  __asm__ volatile("svc 0x6" : "+r"(x0), "=r"(x1), "+r"(x2) : : "memory", "x3", "x4", "x5",
                   "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                   "x17", "x18");
  return (uint32_t)x0;
}

void sys_sleep_ns(int64_t ns)
{
  register uint64_t x0 __asm__("x0") = (uint64_t)ns;
  __asm__ volatile("svc 0xB" : "+r"(x0) : : "memory", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                   "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18");
}

#define CURRENT_PROCESS_HANDLE 0xFFFF8001 /* pseudo-handle for this process */

bool sys_get_info(uint32_t type, uint64_t sub, uint64_t *out)
{
  register uint64_t x0 __asm__("x0");
  register uint64_t x1 __asm__("x1") = type;
  register uint64_t x2 __asm__("x2") = CURRENT_PROCESS_HANDLE;
  register uint64_t x3 __asm__("x3") = sub;
  __asm__ volatile("svc 0x29" : "=r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", "x4", "x5", "x6", "x7",
                   "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18");
  if ((uint32_t)x0 != 0)
    return false;
  *out = x1;
  return true;
}

static void sys_output_debug_string(const char *text, size_t len)
{
  register uint64_t x0 __asm__("x0") = (uint64_t)text;
  register uint64_t x1 __asm__("x1") = len;
  __asm__ volatile("svc 0x27" : "+r"(x0), "+r"(x1) : : "memory", "x2", "x3", "x4", "x5", "x6",
                   "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                   "x18");
}

uint64_t sys_tick(void)
{
  uint64_t ticks;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
  return ticks;
}

uint64_t sys_tick_freq(void)
{
  uint64_t ticks_per_second;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(ticks_per_second));
  return ticks_per_second;
}

/* --- memory ---------------------------------------------------------------------------- */

#define NULL_PAGE_END    0x1000 /* never mapped: anything below is a bad pointer */
#define MEM_TYPE_UNMAPPED 0
#define MEM_ATTR_LOCKED   0x1    /* locked/borrowed */
#define MAX_REGIONS      100000 /* stops a region walk that never ends */

static sys_meminfo_t last_region; /* the region last queried; size 0 = none */

bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm)
{
  if (addr < NULL_PAGE_END || addr + size < addr)
    return false;
  sys_meminfo_t *region = &last_region;
  bool in_cached_region =
    region->size && addr >= region->addr && addr + size <= region->addr + region->size;
  if (!in_cached_region) {
    if (sys_query_memory(region, addr) != 0) {
      region->size = 0;
      return false;
    }
  }
  if (region->type == MEM_TYPE_UNMAPPED || (region->perm & perm) != perm)
    return false;
  return addr + size <= region->addr + region->size;
}

void sys_frame(void)
{
  last_region.size = 0;
}

uintptr_t sys_ptr(uintptr_t addr)
{
  if ((addr & 7) || !sys_mapped(addr, 8, SYS_PERM_R))
    return 0;
  return *(volatile uintptr_t *)addr;
}

bool sys_read32(uintptr_t addr, uint32_t *out)
{
  if ((addr & 3) || !sys_mapped(addr, 4, SYS_PERM_R))
    return false;
  *out = *(volatile uint32_t *)addr;
  return true;
}

void sys_regions(uint32_t perm, void (*fn)(uintptr_t, size_t, void *), void *ctx)
{
  sys_meminfo_t region;
  uint64_t addr = 0;
  for (int guard = 0; guard < MAX_REGIONS; guard++) {
    if (sys_query_memory(&region, addr) != 0 || region.size == 0)
      break;
    if (region.type != MEM_TYPE_UNMAPPED && (region.perm & perm) == perm && !(region.attr & MEM_ATTR_LOCKED))
      fn((uintptr_t)region.addr, (size_t)region.size, ctx);
    uint64_t next = region.addr + region.size;
    if (next <= addr)
      break;
    addr = next;
  }
}

/* --- logging --------------------------------------------------------------------------- */

/* the last LOG_LINES lines, for the menu's log view; log_head is the newest */
static char log_ring[LOG_LINES][LOG_LINE_LEN];
static int log_head;

/* Lines waiting to be written to the log file (settings.c, on the game's thread): log_printf can be
 * called from the drawing thread too, so the buffer has a small lock. Kept from the start, so the lines
 * before the SD card is mounted get written too; if it fills up before then, the newest are dropped. */
#define FILE_PENDING_SIZE 32768
static char file_pending[FILE_PENDING_SIZE];
static size_t file_pending_len;
static volatile int file_lock;

static void file_lock_take(void)
{
  while (__atomic_test_and_set(&file_lock, __ATOMIC_ACQUIRE))
    ;
}

static void file_lock_release(void)
{
  __atomic_clear(&file_lock, __ATOMIC_RELEASE);
}

/* Queue one line for the log file, with a "[seconds.ms] " stamp. */
static void file_pending_add(const char *text, size_t len)
{
  file_lock_take();
  uint64_t ms = sys_tick() * 1000 / sys_tick_freq();
  char stamp[24];
  int stamp_len = snprintf(stamp, sizeof(stamp), "[%5u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
  if (stamp_len > 0 && file_pending_len + (size_t)stamp_len + len + 1 < FILE_PENDING_SIZE) {
    memcpy(file_pending + file_pending_len, stamp, (size_t)stamp_len);
    memcpy(file_pending + file_pending_len + stamp_len, text, len);
    file_pending_len += (size_t)stamp_len + len;
    file_pending[file_pending_len++] = '\n';
  }
  file_lock_release();
}

size_t log_take_pending(char *out, size_t cap)
{
  file_lock_take();
  size_t taken = file_pending_len < cap ? file_pending_len : cap;
  memcpy(out, file_pending, taken);
  memmove(file_pending, file_pending + taken, file_pending_len - taken);
  file_pending_len -= taken;
  file_lock_release();
  return taken;
}

void log_printf(const char *fmt, ...)
{
  char buf[256];
  char *message = buf + PREFIX_LEN;
  const size_t message_max = sizeof(buf) - PREFIX_LEN - 1;
  va_list ap;
  va_start(ap, fmt);
  int formatted_len = vsnprintf(message, sizeof(buf) - PREFIX_LEN, fmt, ap);
  va_end(ap);
  memcpy(buf, PREFIX, PREFIX_LEN);
  if (formatted_len < 0)
    return;
  size_t message_len = (size_t)formatted_len < message_max ? (size_t)formatted_len : message_max;
  sys_output_debug_string(buf, PREFIX_LEN + message_len);

  /* the menu's copy (without the prefix), cut to a line */
  log_head = (log_head + 1) % LOG_LINES;
  size_t ring_len = message_len < LOG_LINE_LEN - 1 ? message_len : LOG_LINE_LEN - 1;
  memcpy(log_ring[log_head], message, ring_len);
  log_ring[log_head][ring_len] = 0;
  file_pending_add(message, message_len);
}

void log_raw(const char *text)
{
  char buf[160] = PREFIX;
  size_t len = PREFIX_LEN;
  while (*text && len < sizeof(buf) - 1)
    buf[len++] = *text++;
  sys_output_debug_string(buf, len);
}

const char *log_line(int i)
{
  if (i < 0 || i >= LOG_LINES)
    return 0;
  const char *line = log_ring[(log_head - i + LOG_LINES) % LOG_LINES];
  return line[0] ? line : 0;
}
