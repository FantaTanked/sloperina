/* zstd decompression in the module, for the game's RomFS files (.zs), with or without a dictionary
 * (the game's are in content:/Pack/ZsDic.pack.zs). Uses zstd's educational decoder
 * (third_party/zstd_decompress.c): small and slow, enough for a few font files at start-up. */
#ifndef ZSTD_H
#define ZSTD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct zstd_dict zstd_dict_t;

/* the frame's decompressed size from its header; 0 when the header doesn't say or src isn't zstd */
size_t zstd_content_size(const void *src, size_t len);
/* the dictionary ID a frame asks for; 0 = none */
uint32_t zstd_frame_dict_id(const void *src, size_t len);
/* a dictionary from a .zsdic (zstd dictionary format); NULL if it's damaged */
zstd_dict_t *zstd_dict_load(const void *data, size_t len);
void zstd_dict_free(zstd_dict_t *dict);
/* decompress src into dst (cap bytes) with dict (NULL: none). Returns the bytes written, or 0 with
 * *why set (damaged input, too small, out of memory); nothing is left allocated either way. */
size_t zstd_decompress(void *dst, size_t cap, const void *src, size_t len, const zstd_dict_t *dict,
                       const char **why);

/* A frame too big to hold, decoded a block at a time (no dictionary): its output passes through a
 * window of the frame's Window_Size (at most 16 MiB) and goes to the sink as it comes. read: up to n
 * bytes of the file from `at`, 0 at its end (or on an error). sink: the output's bytes [at, at + n);
 * false when nothing more is wanted. One thread at a time (zstd.c's error return is one static). */
typedef struct zstd_stream zstd_stream_t;
typedef size_t (*zstd_read_t)(void *user, uint64_t at, void *buf, size_t n);
typedef bool (*zstd_sink_t)(void *user, uint64_t at, const uint8_t *data, size_t n);
zstd_stream_t *zstd_stream_new(zstd_read_t read, void *user, const char **why);
/* decodes blocks until about `budget` more bytes are out: 1 more to come, 0 the end (of the frame or of
 * what the sink wanted), -1 an error (*why) */
int zstd_stream_step(zstd_stream_t *s, size_t budget, zstd_sink_t sink, void *user, const char **why);
void zstd_stream_free(zstd_stream_t *s);

#endif
