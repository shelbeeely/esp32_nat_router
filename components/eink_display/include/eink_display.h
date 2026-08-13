#pragma once

/* E-ink status display driver for the Xteink X4 (ESP32-C3, SSD1677, 800x480).
 *
 * Uses the FreeInk SDK (github.com/Free-Ink/freeink-sdk, vendored as the
 * third_party/freeink-sdk git submodule) for the low-level panel bring-up,
 * wrapped here the same way oled_display wraps the SSD1306: NVS-persisted
 * enable flag, a FreeRTOS task, and a plain C API for main/cmd_router.
 *
 * X4 wiring (fixed by the board, not user-configurable like the OLED's I2C
 * pins): SCLK=8, MOSI=10, CS=21, DC=4, RST=5, BUSY=6. GPIO5/6 collide with
 * oled_display's default I2C pins -- never enable both on the same board.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)

/**
 * @brief Initialize the e-ink display from NVS config.
 * If enabled, starts the display update task. Enabled by default (unlike the
 * OLED, this is the X4's own screen, not an optional peripheral) --
 * 'set_eink disable' opts out.
 */
void eink_display_init(void);

/**
 * @brief Enable the e-ink display (persisted to NVS, requires reboot).
 */
void eink_display_enable(void);

/**
 * @brief Disable the e-ink display (persisted to NVS, requires reboot).
 */
void eink_display_disable(void);

/**
 * @brief Get current e-ink display config.
 * @param[out] enabled  Whether the display is enabled
 */
void eink_display_get_config(bool *enabled);

#else /* !CONFIG_IDF_TARGET_ESP32C3 */

static inline void eink_display_init(void) {}
static inline void eink_display_enable(void) {}
static inline void eink_display_disable(void) {}
static inline void eink_display_get_config(bool *enabled) {
    if (enabled) *enabled = false;
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */

#ifdef __cplusplus
}
#endif
