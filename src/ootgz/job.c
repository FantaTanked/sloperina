#include "job.h"

#include "sys.h"

#define JOB_MAX 32
#define JOB_STEP_LIMIT 600 /* frames; a job still running after this is abandoned */

/* jobs[0] is the one running (if its step > 0); the rest wait in order */
static job_t jobs[JOB_MAX];
static int job_count;

bool job_submit(uint32_t key, job_fn_t fn, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3)
{
  job_t job = { .key = key, .fn = fn, .arg = { a0, a1, a2, a3 } };
  /* replace a queued (not yet started) job with the same key */
  for (int i = 1; i < job_count; i++)
    if (jobs[i].key == key) {
      jobs[i] = job;
      return true;
    }
  if (job_count && jobs[0].key == key && jobs[0].step == 0) {
    jobs[0] = job;
    return true;
  }
  if (job_count >= JOB_MAX) {
    log_printf("job queue full, dropped %08x", key);
    return false;
  }
  jobs[job_count++] = job;
  return true;
}

bool job_pending(uint32_t key)
{
  for (int i = 0; i < job_count; i++)
    if (jobs[i].key == key)
      return true;
  return false;
}

void job_frame(void)
{
  if (!job_count)
    return;
  job_t *job = &jobs[0];
  bool done = job->fn(job);
  job->step++;
  if (!done && job->step > JOB_STEP_LIMIT) {
    log_printf("job %08x abandoned after %d steps", job->key, job->step);
    done = true;
  }
  if (done) {
    for (int i = 1; i < job_count; i++)
      jobs[i - 1] = jobs[i];
    job_count--;
  }
}
