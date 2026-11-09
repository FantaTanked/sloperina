/* Horizon system calls, logging and safe memory access. */
#ifndef SYS_H
#define SYS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* imported from the game's sdk module (resolved by rtld) */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);

typedef struct {
  uint64_t addr;
  uint64_t size;
  uint32_t type;
  uint32_t attr;
  uint32_t perm;   /* bit 0 R, 1 W, 2 X */
  uint32_t ipc_refcount;
  uint32_t device_refcount;
  uint32_t padding;
} sys_meminfo_t;

#define SYS_PERM_R 1u
#define SYS_PERM_W 2u

uint32_t sys_query_memory(sys_meminfo_t *info, uint64_t addr);
/* svcGetInfo for this process: 4/5 heap region address/size, 6 total memory, 7 used memory */
bool sys_get_info(uint32_t type, uint64_t sub, uint64_t *out);
void sys_sleep_ns(int64_t ns); /* svcSleepThread */
uint64_t sys_tick(void);
uint64_t sys_tick_freq(void);

/* True if [addr, addr + size) is mapped with at least `perm`. Caches the last region. */
bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm);

/* Read a pointer at addr if addr is readable; 0 otherwise. */
uintptr_t sys_ptr(uintptr_t addr);
/* Read a u32 if readable; returns false otherwise. */
bool sys_read32(uintptr_t addr, uint32_t *out);

/* Call fn(region_start, region_size, ctx) for every mapped region with `perm`. */
void sys_regions(uint32_t perm, void (*fn)(uintptr_t, size_t, void *), void *ctx);

/* Logging: svcOutputDebugString (Lichii/Eden log, Atmosphère debug log) plus a ring buffer
 * the menu can show. */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Unformatted; safe before the sdk is initialised (module init). */
void log_raw(const char *s);
#define LOG_LINES 128
#define LOG_LINE_LEN 96
const char *log_line(int i); /* 0 = newest; NULL past the end */
/* the lines logged since the last call ("[seconds] text\n" each), for the log file (settings.c) */
size_t log_take_pending(char *out, size_t cap);
void *memmove(void *dst, const void *src, size_t n);

#endif
