/*
 * Hand-written stand-in for the autoconf config.h, for building
 * shairport-sync with Soong.
 *
 * Scope: AirPlay 1 (RAOP) audio only.
 *
 *   crypto   BoringSSL, via AOSP's libcrypto. It is close enough to OpenSSL
 *            for everything shairport uses -- the legacy AES_cbc_encrypt, the
 *            EVP cipher API, BIO base64, and RSA_private_decrypt. mbedtls is
 *            vendored in AOSP but has no Soong build at all, so it is not an
 *            option here despite being shairport's other supported backend.
 *   mdns     the bundled tinysvcmdns. Android has neither Avahi nor D-Bus.
 *   ALAC     the bundled Hammerton decoder. AOSP defines the audio/alac MIME
 *            type but ships no decoder, so there is nothing to borrow.
 *
 * Deliberately off: AirPlay 2 (needs nqptp, libplist, ffmpeg), Avahi, soxr,
 * libdaemon, D-Bus/MPRIS, MQTT, metadata and convolution.
 */
#ifndef SPS_ANDROID_CONFIG_H
#define SPS_ANDROID_CONFIG_H

#define PACKAGE "shairport-sync"
#define PACKAGE_NAME "shairport-sync"
#define PACKAGE_VERSION "4.3.7-android"
#define VERSION PACKAGE_VERSION

#define CONFIG_OPENSSL 1
#define CONFIG_TINYSVCMDNS 1
/* Metadata over a pipe: track, artist, album, artwork, and the name of the
   sending device. The controlling service reads it and publishes a
   MediaSession, which is what puts anything on screen -- shairport itself is
   headless. */
#define CONFIG_METADATA 1
#define CONFIG_METADATA_PIPE 1

/* "-m none": advertisement handled elsewhere, or not at all. */
#define CONFIG_MDNS_NONE 1
#define CONFIG_HAMMERTON 1

/* Audio back ends. AAudio is the real one; the others stay in because they
   cost nothing and make the daemon testable without an audio device. */
#define CONFIG_AAUDIO 1
#define CONFIG_DUMMY 1
#define CONFIG_PIPE 1
#define CONFIG_STDOUT 1

#define HAVE_LIBPTHREAD 1
#define HAVE_LIBM 1

#define SYSCONFDIR "/system/etc"

#endif /* SPS_ANDROID_CONFIG_H */
