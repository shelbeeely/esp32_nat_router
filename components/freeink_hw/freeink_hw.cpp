#include "freeink_hw.h"

#if defined(CONFIG_IDF_TARGET_ESP32C3)

#include <Arduino.h>
#include <BoardConfig.h>
#include <InputManager.h>
#include <BatteryMonitor.h>
#include <PowerManager.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "freeink_hw";

#define PARAM_NAMESPACE "esp32_nat"

/* Hold the power button this long to trigger a deep sleep shutdown --
 * matches the typical e-reader/phone "hold to power off" gesture. */
#define POWER_OFF_HOLD_MS 3000
#define POLL_INTERVAL_MS 50
#define BATTERY_SAMPLE_EVERY (10000 / POLL_INTERVAL_MS) /* ~10s */

static volatile uint16_t s_batteryPercent = 0;
static volatile bool s_charging = false;

static void freeink_hw_task(void *arg)
{
    (void)arg;

    /* Safe to call even if eink_display's task has already called this. */
    initArduino();

    InputManager input;
    input.begin();

    BatteryMonitor battery;

    ESP_LOGI(TAG, "Input/battery polling running");

    int sampleCounter = 0;

    while (true) {
        input.update();

        if (input.isPowerButtonPressed() &&
            input.getPowerButtonHeldTime() >= POWER_OFF_HOLD_MS) {
            ESP_LOGW(TAG, "Power button held %dms - shutting down", POWER_OFF_HOLD_MS);
            /* Does not return: waits for release, arms wake-on-power-button,
             * and enters deep sleep. */
            freeink::PowerManager::deepSleepUntilPowerButton();
        }

        if (++sampleCounter >= BATTERY_SAMPLE_EVERY) {
            sampleCounter = 0;
            uint16_t pct;
            if (battery.readPercentageChecked(pct)) {
                s_batteryPercent = pct;
            }
            s_charging = battery.isCharging();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void freeink_hw_init(void)
{
    bool enabled = false;
    freeink_hw_get_config(&enabled);

    if (!enabled) {
        ESP_LOGI(TAG, "FreeInk hardware support disabled");
        return;
    }

    xTaskCreate(freeink_hw_task, "freeink_hw", 4096, NULL, 2, NULL);
}

void freeink_hw_enable(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "fkhw_en", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void freeink_hw_disable(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "fkhw_en", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void freeink_hw_get_config(bool *enabled)
{
    nvs_handle_t nvs;
    int32_t val;

    *enabled = false;

    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_i32(nvs, "fkhw_en", &val) == ESP_OK)
        *enabled = (val != 0);

    nvs_close(nvs);
}

uint16_t freeink_hw_get_battery_percent(void)
{
    return s_batteryPercent;
}

bool freeink_hw_is_charging(void)
{
    return s_charging;
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */
