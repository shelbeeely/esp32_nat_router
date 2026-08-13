#pragma once

/* Xteink X4 hardware support beyond the display: buttons (InputManager),
 * battery (BatteryMonitor), and power-button deep sleep (PowerManager), from
 * the FreeInk SDK (vendored as the third_party/freeink-sdk git submodule --
 * same submodule eink_display already uses for the display driver).
 *
 * EXPERIMENTAL, same caveat as eink_display/microreticulum: no ESP-IDF
 * toolchain was available to build or run this where it was written.
 *
 * Deliberately NOT wired here (present in the freeink-sdk submodule, but
 * inert or out of scope for a stock X4):
 *  - FrontlightManager, AudioManager, LedManager, Rtc, EnvironmentSensor,
 *    Imu, Microphone, Buzzer, BleKeyboardHost, touch (InputManager's touch
 *    API) -- the X4's own BoardConfig profile sets NO_FRONTLIGHT, NO_AUDIO,
 *    NO_LEDS, NO_TOUCH, NO_MIC, NO_SENSORS, so this hardware plainly isn't on
 *    the board; wiring these would compile code with no way to activate it
 *    unless external peripherals are added later.
 *  - FreeInkUI, FreeInkBook, ContentProtection -- an e-reader UI/DRM stack
 *    with no obvious role in a router.
 *  - UsbMassStorage -- real conflict, not just unused: it needs native
 *    USB-OTG mode, but this firmware's ESP32-C3 config already uses the
 *    same USB peripheral for the console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG).
 *    Those are mutually exclusive on one USB peripheral; enabling MSC would
 *    mean giving up USB console access (or making MSC an exclusive toggle
 *    mode), which needs a product decision, not a default-on wire-up.
 *  - MemoryManager, XteinkDetect -- MemoryManager's cache-sink model has no
 *    integration point in a router (no rebuildable render caches to evict);
 *    XteinkDetect distinguishes X3 vs X4, moot since this build only compiles
 *    in FREEINK_DEVICE_X4.
 *  - SDCardManager -- tracked separately as microreticulum's Phase 2 storage
 *    backend, not yet implemented.
 */

/* Needed for CONFIG_IDF_TARGET_ESP32C3 below -- see eink_display.h's copy of
 * this comment for why it can't be left to transitive inclusion. */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)

/**
 * @brief Initialize FreeInk hardware support from NVS config.
 * If enabled, starts the input/battery polling task. Disabled by default.
 */
void freeink_hw_init(void);

void freeink_hw_enable(void);
void freeink_hw_disable(void);
void freeink_hw_get_config(bool *enabled);

/**
 * @brief Last-sampled battery percentage (0-100). 0 if unavailable
 * (feature disabled, or not yet sampled since boot).
 */
uint16_t freeink_hw_get_battery_percent(void);

/**
 * @brief Whether the battery is currently charging, per the last sample.
 */
bool freeink_hw_is_charging(void);

#else /* !CONFIG_IDF_TARGET_ESP32C3 */

static inline void freeink_hw_init(void) {}
static inline void freeink_hw_enable(void) {}
static inline void freeink_hw_disable(void) {}
static inline void freeink_hw_get_config(bool *enabled) {
    if (enabled) *enabled = false;
}
static inline uint16_t freeink_hw_get_battery_percent(void) { return 0; }
static inline bool freeink_hw_is_charging(void) { return false; }

#endif /* CONFIG_IDF_TARGET_ESP32C3 */

#ifdef __cplusplus
}
#endif
