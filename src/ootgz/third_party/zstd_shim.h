/* What zstd_decompress.c (zstd's educational decoder) needs from libc, for the module: allocation
 * through zstd.c (the game's aligned_alloc, tracked so an error frees everything) and an error that
 * returns to zstd.c instead of exiting. Included only by zstd_decompress.c. */
#ifndef ZSTD_SHIM_H
#define ZSTD_SHIM_H

#include <stddef.h>
#include <stdint.h>

#define ZDEC_NO_MESSAGE

void *zstd_shim_alloc(size_t size);
void *zstd_shim_calloc(size_t count, size_t size);
void zstd_shim_free(void *p);
__attribute__((noreturn)) void zstd_shim_fail(const char *why);

#define malloc(n) zstd_shim_alloc(n)
#define calloc(n, s) zstd_shim_calloc((n), (s))
#define free(p) zstd_shim_free(p)

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#endif
