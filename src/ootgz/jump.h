/* A jump back out of code that can't report an error itself (zstd's educational decoder, stb_truetype
 * out of memory): JUMP_SET(buf) returns 0, and later JUMP_BACK(buf) makes it return 1. The module
 * uses GCC's builtins, which need no libc; the PC build (clang, src/host/nativepreview.c) has none on
 * arm64, so it uses libc's setjmp. As with setjmp, locals changed between the two are lost. */
#ifndef JUMP_H
#define JUMP_H

#if defined(__clang__)
#include <setjmp.h>
typedef jmp_buf jump_buf_t;
#define JUMP_SET(buf) setjmp(buf)
#define JUMP_BACK(buf) longjmp(buf, 1)
#else
typedef void *jump_buf_t[5];
#define JUMP_SET(buf) __builtin_setjmp(buf)
#define JUMP_BACK(buf) __builtin_longjmp(buf, 1)
#endif

#endif
