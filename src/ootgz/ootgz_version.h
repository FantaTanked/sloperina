/* Shared by the module and the IPS generator: the module only installs its frame hook when
 * the patch in the game is the one it was built with. Bump when the stub or site changes. */
#ifndef OOTGZ_VERSION_H
#define OOTGZ_VERSION_H

#define OOTGZ_PATCH_VERSION 4
#define OOTGZ_MARKER        "OOTGZ\0\0\0"  /* at cave_start, followed by u32 patch version and game revision */
#define OOTGZ_STUB_OFF      0x40           /* stub at CAVE_START + this */
#define OOTGZ_STORES_OFF    0x80           /* the lock stubs from here: the X+Y stub, then one per z64hd_store */

#endif
