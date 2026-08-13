/* Reticulum Network Stack transport node -- see include/microreticulum.h for
 * the overview and the EXPERIMENTAL/untested-hardware caveat.
 *
 * Modeled closely on microReticulum's own examples/common/udp_transport
 * example (reticulum_setup()/loop()): register a filesystem, register and
 * start an interface, construct+start Reticulum with transport enabled, then
 * call reticulum.loop() repeatedly. No application Destination is created --
 * this runs the router purely as mesh infrastructure (a relay/transport
 * node), matching what that example itself does.
 */

#include "microreticulum.h"

#if defined(CONFIG_IDF_TARGET_ESP32C3)

#include <Arduino.h>
#include <microReticulum.h>
#include <microStore/FileSystem.h>

#include "RouterUdpInterface.h"
#include "EspNowInterface.h"
#include "FatFsFileSystem.h"
#include "SdCardFileSystem.h"
#include "MeshUplink.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "microrns";

#define PARAM_NAMESPACE "esp32_nat"

/* Subdirectory of the router's existing on-flash FATFS mount (MOUNT_PATH
 * "/data" in main/esp32_nat_router.c, already used for console history) --
 * see FatFsFileSystem.h for why this isn't microStore's own UniversalFileSystem
 * adapter (that one wants a dedicated LittleFS partition on ESP32, not a path
 * prefix into whatever's already mounted). Used only as a fallback when no SD
 * card is present -- much less room than the card, but Reticulum still runs. */
#define RNS_STORAGE_PATH "/data/reticulum"

/* Preferred storage: the X4's SD card, when present -- far more room for the
 * path table and any future LXMF-style message history than the internal
 * flash mount above. */
#define RNS_SD_STORAGE_PATH "/reticulum"

static RNS::Reticulum reticulum({RNS::Type::NONE});
static RNS::Interface udp_interface(RNS::Type::NONE);
static RNS::Interface espnow_interface(RNS::Type::NONE);

static void reticulum_task(void *arg)
{
    (void)arg;

    /* Required once before any Arduino/microReticulum API call -- nothing
     * else in this firmware runs on the Arduino layer beyond what
     * eink_display already initializes (CONFIG_AUTOSTART_ARDUINO=n keeps
     * Arduino from installing its own app_main). Safe to call more than
     * once if eink_display has already done so. */
    initArduino();

    RNS::loglevel(RNS::LOG_INFO);

    ESP_LOGI(TAG, "Starting Reticulum transport node...");

    /* Prefer the SD card (far more room) and fall back to the internal flash
     * mount if no card is present or it fails to mount. */
    static reticulum_bridge::SdCardFileSystem sdFilesystem(RNS_SD_STORAGE_PATH);
    static reticulum_bridge::FatFsFileSystem flashFilesystem(RNS_STORAGE_PATH);
    microStore::FileSystem fs;

    if (sdFilesystem.init()) {
        ESP_LOGI(TAG, "Reticulum storage: SD card (" RNS_SD_STORAGE_PATH ")");
        fs = sdFilesystem;
    } else {
        ESP_LOGW(TAG, "No SD card (or mount failed) - falling back to internal flash storage");
        if (!flashFilesystem.init()) {
            ESP_LOGE(TAG, "Failed to initialize Reticulum storage at " RNS_STORAGE_PATH);
            vTaskDelete(NULL);
            return;
        }
        fs = flashFilesystem;
    }
    RNS::Utilities::OS::register_filesystem(fs);

    udp_interface = new RouterUdpInterface();
    udp_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(udp_interface);
    if (!udp_interface.start()) {
        ESP_LOGE(TAG, "Failed to start Reticulum UDP interface");
        vTaskDelete(NULL);
        return;
    }

    /* Router-to-router mesh discovery, independent of either unit's WiFi
     * AP/STA/uplink state. Non-fatal if it fails to start (e.g. esp_now_init
     * issue) -- the node still runs with the UDP interface alone. */
    espnow_interface = new EspNowInterface();
    espnow_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(espnow_interface);
    if (!espnow_interface.start()) {
        ESP_LOGW(TAG, "Failed to start Reticulum ESP-NOW interface - continuing with UDP only");
    }

    /* Leave Reticulum::storagepath()/cachepath() at their defaults ("." /
     * "./cache") -- FatFsFileSystem's basepath (RNS_STORAGE_PATH) already
     * grounds every relative path it's handed. Overriding storagepath() here
     * too would double up the prefix (the filesystem adapter would see an
     * already-absolute path and prepend its basepath a second time). */
    reticulum = RNS::Reticulum();
    reticulum.transport_enabled(true);
    reticulum.probe_destination_enabled(true);
    reticulum.remote_management_enabled(true);
    reticulum.start();

    mesh_uplink_init();

    ESP_LOGI(TAG, "Reticulum transport node running (UDP broadcast :%d%s)", RNS_UDP_PORT,
             espnow_interface.online() ? " + ESP-NOW" : "");

    while (true) {
        reticulum.loop();
        mesh_uplink_tick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void microreticulum_init(void)
{
    bool enabled = false;
    microreticulum_get_config(&enabled);

    if (!enabled) {
        ESP_LOGI(TAG, "Reticulum disabled");
        return;
    }

    xTaskCreate(reticulum_task, "reticulum", 10240, NULL, 2, NULL);
}

void microreticulum_enable(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "rns_en", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void microreticulum_disable(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "rns_en", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void microreticulum_get_config(bool *enabled)
{
    nvs_handle_t nvs;
    int32_t val;

    *enabled = false;

    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_i32(nvs, "rns_en", &val) == ESP_OK)
        *enabled = (val != 0);

    nvs_close(nvs);
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */
