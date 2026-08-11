#pragma once

/* Reticulum Network Stack transport node, via the microReticulum SDK
 * (github.com/attermann/microReticulum, vendored as third_party git
 * submodules). Runs the router as a Reticulum transport/relay node reachable
 * over UDP broadcast on the AP LAN (port 4242, matching the reference
 * implementation's UDP interface convention) -- fitting, since that's
 * already this firmware's job for IP traffic.
 *
 * EXPERIMENTAL. This has not been compiled or run on real hardware: no
 * ESP-IDF toolchain was available to build it where it was written. See
 * components/microreticulum/README.md for the dependency graph, the known
 * gaps (crypto library provenance, RAM budget, untuned allocator pool size),
 * and what to check on a first real build. Disabled by default; ESP32-C3 only.
 *
 * No application Destination is created -- this mirrors microReticulum's own
 * udp_transport example (a pure relay node, no local endpoint), which keeps
 * the initial integration to the smallest slice of the API that's actually
 * exercised by an upstream example.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)

/*
 * Known gaps as of this writing (no ESP-IDF toolchain was available to build
 * and iterate on this, so these are unverified rather than fixed):
 *  - Crypto (attermann/Crypto, pinned by commit) is a static, unmaintained
 *    mirror of the old rweather Arduino Crypto library -- no upstream
 *    security-fix tracking. It's what microReticulum's own build fetches by
 *    default; no actively-maintained drop-in replacement with a matching API
 *    is known. Worth reassessing before relying on this for anything real.
 *  - RNS_HEAP_POOL_BUFFER_SIZE (TLSF pool allocator) is left at its dynamic
 *    default -- unsized, unlike upstream's own tuning guidance for
 *    constrained boards. Needs real on-device measurement.
 *  - The UDP interface (RouterUdpInterface) assumes global broadcast
 *    (255.255.255.255) reaches the AP LAN; not verified against ESP-IDF's
 *    lwIP broadcast/SO_BROADCAST behavior on real hardware.
 *  - Storage rides the router's existing on-flash FATFS mount via a custom
 *    adapter (FatFsFileSystem) instead of microStore's built-in
 *    UniversalFileSystem, which wants its own LittleFS partition on ESP32 --
 *    see FatFsFileSystem.h for why. Not exercised against real Identity/path
 *    table persistence yet.
 *  - No console command or web UI exposure yet -- NVS enable/disable only.
 *  - Storage requires the router's on-flash FATFS mount (main/esp32_nat_router.c's
 *    initialize_filesystem(), MOUNT_PATH "/data") to already be up, which only
 *    happens when CONFIG_STORE_HISTORY is enabled. If it's off, this fails
 *    closed (fs.init() error, task exits, logged) rather than mounting its
 *    own filesystem -- avoids a second, possibly conflicting FATFS mount.
 */

/**
 * @brief Initialize Reticulum from NVS config.
 * If enabled, starts the transport task. Disabled by default.
 */
void microreticulum_init(void);

/**
 * @brief Enable Reticulum (persisted to NVS, requires reboot).
 */
void microreticulum_enable(void);

/**
 * @brief Disable Reticulum (persisted to NVS, requires reboot).
 */
void microreticulum_disable(void);

/**
 * @brief Get current Reticulum config.
 * @param[out] enabled  Whether Reticulum is enabled
 */
void microreticulum_get_config(bool *enabled);

#else /* !CONFIG_IDF_TARGET_ESP32C3 */

static inline void microreticulum_init(void) {}
static inline void microreticulum_enable(void) {}
static inline void microreticulum_disable(void) {}
static inline void microreticulum_get_config(bool *enabled) {
    if (enabled) *enabled = false;
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */

#ifdef __cplusplus
}
#endif
