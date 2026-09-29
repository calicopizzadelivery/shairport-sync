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
#include <string.h>

static AAudioStream *stream = NULL;
static unsigned int bytes_per_frame = 0;

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
    AAudioStream_close(stream);
    stream = NULL;
  }
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
  if (stream == NULL)
    return 0; /* nothing open: drop rather than block the player thread */

  /* Blocking write with a generous timeout. Shairport's player thread expects
     play() to take roughly as long as the audio lasts. */
  aaudio_result_t written = AAudioStream_write(stream, buf, samples, 500 * 1000 * 1000L);
  if (written < 0) {
    warn("aaudio: write failed: %s", AAudio_convertResultToText((aaudio_result_t)written));
    return -1;
  }
  return (int)written;
}

static void flush(void) {
  /* AAudio only accepts a flush from the paused state, so pause, flush, and
     start again rather than tearing the stream down. */
  if (stream == NULL)
    return;
  AAudioStream_requestPause(stream);
  aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNINITIALIZED;
  AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_PAUSING, &state,
                                  100 * 1000 * 1000L);
  AAudioStream_requestFlush(stream);
  AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_FLUSHING, &state,
                                  100 * 1000 * 1000L);
  AAudioStream_requestStart(stream);
}

static int delay(long *the_delay) {
  if (stream == NULL) {
    *the_delay = 0;
    return -ENODEV;
  }
  /* Frames handed to AAudio but not yet consumed by the device. */
  int64_t written = AAudioStream_getFramesWritten(stream);
  int64_t read = AAudioStream_getFramesRead(stream);
  int64_t queued = written - read;
  if (queued < 0)
    queued = 0;
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
