/*
 * A no-op mDNS backend, for Android.
 *
 * Two reasons it exists. Advertisement can be done outside the daemon -- on
 * Android the natural place is NsdManager from the controlling service, which
 * is the platform's own responder rather than a second one inside this
 * process. And the emulator has no IPv4 at all, so the multicast join that
 * tinysvcmdns needs cannot succeed there; without this the daemon exits before
 * it ever reaches the audio path, which makes the rest of the port untestable.
 *
 * Selected with "-m none". tinysvcmdns remains the default.
 *
 * This file is part of the Android port and is not in upstream Shairport Sync.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mdns.h"
#include "common.h"

static int mdns_none_register(__attribute__((unused)) char *ap1name,
                              __attribute__((unused)) char *ap2name,
                              __attribute__((unused)) int port,
                              __attribute__((unused)) char **txt_records,
                              __attribute__((unused)) char **secondary_txt_records) {
  inform("mdns: advertisement is disabled (\"-m none\"); this receiver will not be "
         "discoverable unless something else advertises it.");
  return 0;
}

static int mdns_none_update(__attribute__((unused)) char **txt_records,
                            __attribute__((unused)) char **secondary_txt_records) {
  return 0;
}

static void mdns_none_unregister(void) {}

mdns_backend mdns_none = {.name = "none",
                          .mdns_register = &mdns_none_register,
                          .mdns_update = &mdns_none_update,
                          .mdns_unregister = &mdns_none_unregister,
                          .mdns_dacp_monitor_start = NULL,
                          .mdns_dacp_monitor_set_id = NULL,
                          .mdns_dacp_monitor_stop = NULL};
