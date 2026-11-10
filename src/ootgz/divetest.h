/* Dive test (debug > dive test): drops Link into water from a run of heights and logs how deep each went
 * (divetest.c). */
#ifndef DIVETEST_H
#define DIVETEST_H

#include <stdbool.h>

enum { DIVETEST_HEIGHTS, DIVETEST_MOVING, DIVETEST_COUNT };
void divetest_toggle(int test);    /* start a test (Link swimming at the surface), or stop the one running */
bool divetest_running(void);
void divetest_frame(bool menu_open);

#endif
