/* See pthread_cancel_shim.h for why this exists. */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>

#include "pthread_cancel_shim.h"

/* A real-time signal rather than SIGUSR1: shairport does not use these, and
   neither does anything else we link. */
#define SPS_CANCEL_SIGNAL (SIGRTMIN + 3)

struct sps_cancel_state {
  int disabled;
  int pending;
};

static pthread_key_t sps_cancel_key;
static pthread_once_t sps_cancel_once = PTHREAD_ONCE_INIT;

static void sps_cancel_state_free(void *p) { free(p); }

static void sps_cancel_handler(int sig) {
  (void)sig;
  struct sps_cancel_state *s = pthread_getspecific(sps_cancel_key);
  if (s && s->disabled) {
    /* Deferred: the thread is inside a critical section. Remember it and let
       the next enable or testcancel act on it. */
    s->pending = 1;
    return;
  }
  /* pthread_exit runs the cleanup handlers pushed with pthread_cleanup_push,
     which is the behaviour a real pthread_cancel would give us. */
  pthread_exit(PTHREAD_CANCELED);
}

static void sps_cancel_init(void) {
  pthread_key_create(&sps_cancel_key, sps_cancel_state_free);
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: a cancel should break blocking calls */
  sa.sa_handler = sps_cancel_handler;
  sigaction(SPS_CANCEL_SIGNAL, &sa, NULL);
}

static struct sps_cancel_state *sps_cancel_state(void) {
  pthread_once(&sps_cancel_once, sps_cancel_init);
  struct sps_cancel_state *s = pthread_getspecific(sps_cancel_key);
  if (!s) {
    s = calloc(1, sizeof(*s));
    if (!s)
      return NULL;
    pthread_setspecific(sps_cancel_key, s);
  }
  return s;
}

int sps_pthread_setcancelstate(int state, int *oldstate) {
  struct sps_cancel_state *s = sps_cancel_state();
  if (!s)
    return ENOMEM;
  if (oldstate)
    *oldstate = s->disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
  s->disabled = (state == PTHREAD_CANCEL_DISABLE);
  if (!s->disabled && s->pending) {
    s->pending = 0;
    pthread_exit(PTHREAD_CANCELED);
  }
  return 0;
}

void sps_pthread_testcancel(void) {
  struct sps_cancel_state *s = sps_cancel_state();
  if (s && s->pending && !s->disabled) {
    s->pending = 0;
    pthread_exit(PTHREAD_CANCELED);
  }
}

int sps_pthread_cancel(pthread_t thread) {
  pthread_once(&sps_cancel_once, sps_cancel_init);
  return pthread_kill(thread, SPS_CANCEL_SIGNAL);
}
