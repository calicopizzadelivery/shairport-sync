/*
 * Thread cancellation for bionic, which has none.
 *
 * bionic implements pthread_cleanup_push/pop (they run on pthread_exit) but
 * not pthread_cancel, pthread_setcancelstate or pthread_testcancel. Shairport
 * uses all three across ~100 call sites, so shimming them keeps the upstream
 * sources untouched and the fork rebaseable.
 *
 * The cancel state is tracked for real rather than stubbed out. Shairport
 * brackets its critical sections in
 * pthread_setcancelstate(PTHREAD_CANCEL_DISABLE) precisely so a cancel cannot
 * land while a mutex is held; a no-op stub would throw that protection away and
 * leave locks held by dead threads. Here a cancel that arrives while disabled
 * is recorded and delivered at the next enable or testcancel, which is the
 * behaviour the code is written against.
 *
 * Force-included, so every translation unit sees the macros. This file is
 * Android-only and lives outside the upstream source tree.
 */
#ifndef SPS_PTHREAD_CANCEL_SHIM_H
#define SPS_PTHREAD_CANCEL_SHIM_H

#include <pthread.h>

#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCELED ((void *)-1)

int sps_pthread_setcancelstate(int state, int *oldstate);
void sps_pthread_testcancel(void);
int sps_pthread_cancel(pthread_t thread);

#define pthread_setcancelstate sps_pthread_setcancelstate
#define pthread_testcancel sps_pthread_testcancel
#define pthread_cancel sps_pthread_cancel

#endif /* SPS_PTHREAD_CANCEL_SHIM_H */
