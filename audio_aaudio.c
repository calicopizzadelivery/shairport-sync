/*
 * AAudio output driver for Shairport Sync on Android.
 *
 * Deliberately AAudio rather than ALSA or tinyalsa. Writing to the hardware
 * directly would bypass AudioFlinger, which means no audio policy: no system
 * volume, no routing, and no cooperation with whatever else the box is playing.
 * Through AAudio the stream is an ordinary media stream, so it follows the
 * output the user has chosen -- including a Bluetooth speaker -- and ducks and
 * mixes like anything else.
 *
 * This file is part of the Android port and is not in upstream Shairport Sync.
 *
 * SPDX-License-Identifier: MIT
 */

#include "audio.h"
#include "common.h"

#include <aaudio/AAudio.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static AAudioStream *stream = NULL;
static unsigned int bytes_per_frame = 0;

/*
 * Our own buffer between the player and AAudio, and the reason this back end
 * is callback-driven rather than a blocking AAudioStream_write().
 *
 * Before the first frame plays, the player fills the output with up to the
 * whole stream latency -- two seconds -- of silence, 100 ms at a time, and it
 * does so from inside buffer_get_frame() while holding ab_mutex. The RTP
 * receiver thread needs that mutex to file every incoming packet. ALSA's
 * hardware buffer absorbs the silence without blocking. AAudio's buffer here
 * is ~32 ms, so each write blocked ~100 ms with the mutex held; the receiver
 * filed about one packet per write while twelve arrived, and the socket's
 * receive buffer overflowed within a second. The kernel dropped them
 * (RcvbufErrors in /proc/net/snmp), and they were precisely the packets due
 * to play first: 60-600 ms of silence two seconds into every stream, and a
 * resync each time.
 *
 * Simply asking AAudio for a bigger buffer does not work: an AudioTrack does
 * not start pulling until its start threshold -- by default the whole buffer
 * -- is filled, and the NDK does not expose the threshold. With a three
 * second buffer and the player keeping two seconds queued, it never started
 * at all.
 *
 * So play() copies into this ring and returns, and AAudio drains it from its
 * own callback thread with its normal small buffer. Three seconds holds the
 * worst case -- a full lead-in plus the player's desired second of backlog --
 * so play() never waits under the player's mutex.
 */
#define RING_SECONDS 3
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_space = PTHREAD_COND_INITIALIZER;
static uint8_t *ring = NULL;
static size_t ring_frames = 0; /* capacity */
static size_t ring_read = 0;   /* index of the next frame out */
static size_t ring_fill = 0;   /* frames queued */
static size_t ring_frame_bytes = 0;

/* Written once by configure() and read by start(); AAudio needs them at stream
   construction, which is later. */
static int32_t cfg_channels = 2;
static int32_t cfg_rate = 44100;
static aaudio_format_t cfg_format = AAUDIO_FORMAT_PCM_I16;

static aaudio_format_t aaudio_format_for(unsigned int sps_format) {
  switch (sps_format) {
  case SPS_FORMAT_S16_LE:
  case SPS_FORMAT_S16:
    return AAUDIO_FORMAT_PCM_I16;
  case SPS_FORMAT_S32_LE:
  case SPS_FORMAT_S32:
    return AAUDIO_FORMAT_PCM_I32;
  default:
    return AAUDIO_FORMAT_UNSPECIFIED;
  }
}

static void close_stream(void) {
  if (stream != NULL) {
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream); /* returns once the callback can no longer run */
    stream = NULL;
  }
  pthread_mutex_lock(&ring_lock);
  free(ring);
  ring = NULL;
  ring_frames = ring_read = ring_fill = 0;
  pthread_cond_broadcast(&ring_space);
  pthread_mutex_unlock(&ring_lock);
}

/*
 * AAudio's pull. Anything the ring cannot supply is silence -- an underrun is
 * better heard as a gap than as the stream stopping.
 */
static aaudio_data_callback_result_t data_callback(__attribute__((unused)) AAudioStream *s,
                                                   __attribute__((unused)) void *user,
                                                   void *audio, int32_t num_frames) {
  uint8_t *out = audio;
  size_t wanted = (size_t)num_frames;
  pthread_mutex_lock(&ring_lock);
  size_t fb = ring_frame_bytes;
  size_t n = ring_fill < wanted ? ring_fill : wanted;
  size_t done = 0;
  while (done < n) {
    size_t run = ring_frames - ring_read;
    if (run > n - done)
      run = n - done;
    memcpy(out + done * fb, ring + ring_read * fb, run * fb);
    ring_read = (ring_read + run) % ring_frames;
    done += run;
  }
  ring_fill -= n;
  if (n > 0)
    pthread_cond_signal(&ring_space);
  pthread_mutex_unlock(&ring_lock);
  if (n < wanted)
    memset(out + n * fb, 0, (wanted - n) * fb);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static int init(__attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
  /* A second of buffering. Network audio is bursty and AAudio's own buffer is
     small; this is the same default the other network-fed back ends use. */
  config.audio_backend_buffer_desired_length = 1.0;
  config.audio_backend_latency_offset = 0;

  parse_audio_options("aaudio", (1 << SPS_FORMAT_S16_LE), (1 << SPS_RATE_44100), (1 << 2));
  return 0;
}

static void deinit(void) { close_stream(); }

static int32_t get_configuration(unsigned int channels, unsigned int rate, unsigned int format) {
  return search_for_suitable_configuration(channels, rate, format, NULL);
}

static int configure(int32_t requested_encoded_format,
                     __attribute__((unused)) char **channel_map) {
  unsigned int sps_format = FORMAT_FROM_ENCODED_FORMAT(requested_encoded_format);
  unsigned int bytes_per_sample = sps_format_sample_size(sps_format);
  if (bytes_per_sample == 0) {
    debug(1, "aaudio: unknown output format.");
    return EINVAL;
  }

  cfg_format = aaudio_format_for(sps_format);
  if (cfg_format == AAUDIO_FORMAT_UNSPECIFIED) {
    debug(1, "aaudio: no AAudio equivalent for output format %u.", sps_format);
    return EINVAL;
  }

  cfg_channels = (int32_t)CHANNELS_FROM_ENCODED_FORMAT(requested_encoded_format);
  cfg_rate = (int32_t)RATE_FROM_ENCODED_FORMAT(requested_encoded_format);
  bytes_per_frame = bytes_per_sample * cfg_channels;
  return 0;
}

static void start(__attribute__((unused)) int sample_rate,
                  __attribute__((unused)) int sample_format) {
  close_stream(); /* a re-start without a stop should not leak a stream */

  AAudioStreamBuilder *builder = NULL;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    warn("aaudio: could not create a stream builder: %s", AAudio_convertResultToText(result));
    return;
  }

  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setFormat(builder, cfg_format);
  AAudioStreamBuilder_setChannelCount(builder, cfg_channels);
  AAudioStreamBuilder_setSampleRate(builder, cfg_rate);

  /* What makes this a normal media stream as far as audio policy is concerned,
     so it follows the selected output and obeys the media volume. */
  AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
  AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);

  /* Not LOW_LATENCY: this is buffered network audio, and asking for a small
     burst size only invites underruns. */
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);

  AAudioStreamBuilder_setDataCallback(builder, data_callback, NULL);

  pthread_mutex_lock(&ring_lock);
  ring_frame_bytes = (size_t)cfg_channels * (cfg_format == AAUDIO_FORMAT_PCM_I32 ? 4 : 2);
  ring_frames = (size_t)cfg_rate * RING_SECONDS;
  ring_read = ring_fill = 0;
  ring = calloc(ring_frames, ring_frame_bytes);
  pthread_mutex_unlock(&ring_lock);
  if (ring == NULL) {
    warn("aaudio: could not allocate the output ring.");
    AAudioStreamBuilder_delete(builder);
    return;
  }

  result = AAudioStreamBuilder_openStream(builder, &stream);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK) {
    warn("aaudio: could not open an output stream: %s", AAudio_convertResultToText(result));
    stream = NULL;
    return;
  }

  result = AAudioStream_requestStart(stream);
  if (result != AAUDIO_OK) {
    warn("aaudio: could not start the output stream: %s", AAudio_convertResultToText(result));
    close_stream();
    return;
  }

  debug(1, "aaudio: started %d channels at %d fps, device %d.", cfg_channels, cfg_rate,
        AAudioStream_getDeviceId(stream));
}

static void stop(void) { close_stream(); }

static int play(void *buf, int samples, __attribute__((unused)) int sample_type,
                __attribute__((unused)) uint32_t timestamp,
                __attribute__((unused)) uint64_t playtime) {
  if (stream == NULL || samples <= 0)
    return 0; /* nothing open: drop rather than block the player thread */

  const uint8_t *in = buf;
  size_t left = (size_t)samples;
  pthread_mutex_lock(&ring_lock);
  while (left > 0 && ring != NULL) {
    /* Full means the player is more than RING_SECONDS ahead, which it never
       asks to be. Wait as ALSA would on a full buffer, but boundedly, so a
       stalled output cannot wedge the player forever. */
    while (ring != NULL && ring_fill == ring_frames) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 200 * 1000 * 1000;
      if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
      }
      if (pthread_cond_timedwait(&ring_space, &ring_lock, &until) == ETIMEDOUT) {
        pthread_mutex_unlock(&ring_lock);
        debug(1, "aaudio: output not draining; dropping %zu frames.", left);
        return samples - (int)left;
      }
    }
    if (ring == NULL)
      break;
    size_t fb = ring_frame_bytes;
    size_t write_at = (ring_read + ring_fill) % ring_frames;
    size_t run = ring_frames - write_at;
    size_t space = ring_frames - ring_fill;
    if (run > space)
      run = space;
    if (run > left)
      run = left;
    memcpy(ring + write_at * fb, in, run * fb);
    ring_fill += run;
    in += run * fb;
    left -= run;
  }
  pthread_mutex_unlock(&ring_lock);
  return samples - (int)left;
}

static void flush(void) {
  /* Most of what is queued is in the ring, so emptying it is most of a
     flush. AAudio only accepts a flush of its own buffer from the paused
     state, so pause, flush, and start again rather than tearing the stream
     down. */
  if (stream == NULL)
    return;
  pthread_mutex_lock(&ring_lock);
  ring_fill = 0;
  pthread_cond_broadcast(&ring_space);
  pthread_mutex_unlock(&ring_lock);
  AAudioStream_requestPause(stream);
  aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNINITIALIZED;
  AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_PAUSING, &state,
                                  100 * 1000 * 1000L);
  AAudioStream_requestFlush(stream);
  AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_FLUSHING, &state,
                                  100 * 1000 * 1000L);
  AAudioStream_requestStart(stream);
}

/*
 * Frames written that have not yet been *heard*, as of now.
 *
 * This is what the player synchronises against, so it has to cover the whole
 * output path. framesWritten - framesRead does not: framesRead is where
 * AudioFlinger has pulled from our buffer, and behind that sit the mixer, the
 * HAL and the HDMI link. On porg that gap is about 120 ms, which the player
 * saw as a constant sync error the moment playback began -- "-119 ms, resync
 * requested", again and again, heard as dropouts of 60-600 ms per session.
 *
 * AAudioStream_getTimestamp gives the frame that was presented at a given
 * time; projecting it forward to now is the AAudio equivalent of what the ALSA
 * backend does with snd_pcm_status and its update timestamp. The ring's
 * contents are still ahead of all that, so they are added on top.
 */
static int delay(long *the_delay) {
  if (stream == NULL) {
    *the_delay = 0;
    return -ENODEV;
  }
  /* In callback mode framesWritten counts what the callback has handed
     AAudio, so the ring's contents come on top. */
  pthread_mutex_lock(&ring_lock);
  int64_t in_ring = (int64_t)ring_fill;
  pthread_mutex_unlock(&ring_lock);
  int64_t written = AAudioStream_getFramesWritten(stream);
  int64_t queued;
  int64_t position = 0, position_ns = 0;
  if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &position_ns) == AAUDIO_OK) {
    struct timespec tn;
    clock_gettime(CLOCK_MONOTONIC, &tn);
    int64_t now_ns = (int64_t)tn.tv_sec * 1000000000LL + tn.tv_nsec;
    int64_t presented_now =
        position + (now_ns - position_ns) * AAudioStream_getSampleRate(stream) / 1000000000LL;
    queued = written - presented_now;
  } else {
    /* No presentation timestamp until the first frames reach the device, a
       few milliseconds after start. Until then, what AudioFlinger has not yet
       pulled is the best available answer, and it is only briefly wrong. */
    queued = written - AAudioStream_getFramesRead(stream);
  }
  if (queued < 0)
    queued = 0;
  queued += in_ring;
  *the_delay = (long)queued;
  return 0;
}

static int is_running(void) {
  if (stream == NULL)
    return -1;
  aaudio_stream_state_t state = AAudioStream_getState(stream);
  return (state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING) ? 0 : -1;
}

audio_output audio_aaudio = {.name = "aaudio",
                             .help = NULL,
                             .init = &init,
                             .deinit = &deinit,
                             .get_configuration = &get_configuration,
                             .configure = &configure,
                             .start = &start,
                             .stop = &stop,
                             .is_running = &is_running,
                             .flush = &flush,
                             .delay = &delay,
                             .stats = NULL,
                             .play = &play,
                             /* volume left NULL so Shairport applies its own
                                software volume; the hardware volume belongs to
                                Android's media stream, not to us. */
                             .volume = NULL,
                             .parameters = NULL,
                             .mute = NULL};
