/* Handing a buffer from the game thread to the render thread without a lock. Three buffers: one
 * published, one the reader may still hold from an earlier publish, one free for the writer. */
#ifndef PUBLISH_H
#define PUBLISH_H

#define PUBLISH_BUFFERS 3

typedef struct { volatile int current, reading; } publish_t;
#define PUBLISH_INIT { -1, -1 }

/* Writer: the buffer to fill next. */
static inline int publish_next(const publish_t *p)
{
  int current = p->current, reading = __atomic_load_n(&p->reading, __ATOMIC_SEQ_CST);
  for (int i = 0; i < PUBLISH_BUFFERS; i++)
    if (i != current && i != reading)
      return i;
  return 0;
}

/* Writer: show buffer i (-1: nothing). */
static inline void publish_set(publish_t *p, int i)
{
  __atomic_store_n(&p->current, i, __ATOMIC_SEQ_CST);
}

/* Reader: the buffer to draw (-1: nothing), held until publish_release. */
static inline int publish_take(publish_t *p)
{
  for (;;) {
    int i = __atomic_load_n(&p->current, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->reading, i, __ATOMIC_SEQ_CST);
    /* the writer may have published in between and picked this buffer as free */
    if (__atomic_load_n(&p->current, __ATOMIC_SEQ_CST) == i)
      return i;
  }
}

static inline void publish_release(publish_t *p)
{
  __atomic_store_n(&p->reading, -1, __ATOMIC_SEQ_CST);
}

#endif
