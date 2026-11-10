/* The game's own UI shader (Layout2's MergeShader), loaded from its RomFS at start-up, so the module
 * can draw on the GPU with shaders the Switch 2 runs (they can't be built with public tools). Two of
 * its programs: a quad in its corner colours, and a texture times those colours. docs/NATIVE_UI.md
 * has the shader's interface. */
#ifndef UISHADER_H
#define UISHADER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  const uint8_t *control; /* the NVN control section */
  uint32_t control_size;
  const uint8_t *code;    /* the code area, to be copied into a shader memory pool */
  uint32_t code_size;
} uishader_stage_t;

typedef struct {
  uishader_stage_t vertex, fragment;
} uishader_program_t;

enum {
  UI_PROGRAM_SOLID,   /* corner colours (Texture_ActiveCount 0) */
  UI_PROGRAM_TEXTURE, /* sysTextureSampler0 x corner colours (Texture_ActiveCount 1) */
  UI_PROGRAMS
};

/* game thread, once: true when both programs were found */
bool uishader_load(void);
/* NULL until loaded */
const uishader_program_t *uishader_program(int which);

#endif
