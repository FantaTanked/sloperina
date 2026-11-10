/* The game's own files, read at run time from its RomFS (mounted by the game as "content:"), so the
 * module can use the game's fonts and UI resources without shipping them. */
#ifndef GAMEFILE_H
#define GAMEFILE_H

#include <stddef.h>
#include <stdint.h>

#define GAME_ROM "content:"

/* the whole file, in a block from aligned_alloc (free it with free()); NULL if it can't be read */
uint8_t *gamefile_read(const char *path, size_t *size_out);
/* a .zs file decompressed (zstd, with the game's dictionary when the frame asks for one); NULL on error */
uint8_t *gamefile_read_zs(const char *path, size_t *size_out);
/* free what gamefile_read_zs keeps between calls (the zstd dictionaries) */
void gamefile_release(void);
/* a member of a SARC archive (by its name in the archive); NULL if it has none of that name */
const uint8_t *gamefile_sarc_member(const uint8_t *sarc, size_t size, const char *name, size_t *size_out);

#endif
