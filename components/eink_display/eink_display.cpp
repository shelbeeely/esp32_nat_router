/* E-ink status display driver for the Xteink X4 (ESP32-C3, SSD1677, 800x480).
 *
 * Wraps the FreeInk SDK's FreeInkDisplay facade (vendored as the
 * third_party/freeink-sdk git submodule) the same way oled_display wraps the
 * SSD1306: NVS-persisted enable flag, a FreeRTOS task that redraws the
 * router's status periodically, and a plain-C API for main/cmd_router.
 *
 * FreeInk is an Arduino-framework SDK; it is pulled in here via arduino-esp32
 * used as an ESP-IDF component (see idf_component.yml + sdkconfig.defaults.esp32c3
 * for the required CONFIG_FREERTOS_HZ/CONFIG_AUTOSTART_ARDUINO settings).
 * initArduino() is called once, from this task, before touching any FreeInk
 * API -- nothing else in this firmware uses the Arduino layer.
 *
 * FreeInkDisplay itself only exposes clearScreen/drawImage/setFramebuffer --
 * no text primitives -- so this file draws status text directly into the
 * 1bpp framebuffer it owns (MSB-first, row-major, 1=white/0=black; see
 * FreeInkDisplay::blitImage), the same approach oled_display uses for the
 * SSD1306's framebuffer.
 */

#include "eink_display.h"

#if defined(CONFIG_IDF_TARGET_ESP32C3)

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <BoardConfig.h>
#include <FreeInkDisplay.h>

#include "font5x7.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"

/* Extern router globals (avoid including router_globals.h to prevent circular
 * deps, same approach as oled_display.c). These are defined in C files, so
 * they need an explicit C-linkage block in this C++ translation unit. */
extern "C" {
extern bool ap_connect;
extern uint16_t connect_count;
extern uint32_t my_ip;
extern uint32_t my_ap_ip;
extern char *ssid;
extern char *ap_ssid;
extern uint64_t sta_bytes_sent;
extern uint64_t sta_bytes_received;
extern void resync_connect_count(void);
extern bool vpn_is_connected(void);
}

static const char *TAG = "eink";

#define PARAM_NAMESPACE "esp32_nat"

/* Redraw + refresh cadence. E-paper full/half refreshes are ~0.3-2s and flash
 * visibly, so we redraw content every 5s with a cheap FAST_REFRESH, and only
 * pay for a ghost-clearing HALF_REFRESH every ~2 minutes. */
#define EINK_UPDATE_INTERVAL_MS   5000
#define EINK_HALF_REFRESH_EVERY   24
#define EINK_RESYNC_EVERY         6   /* ~30s, matches oled_display's cadence */

/* ---- Minimal direct-framebuffer text renderer ---- */

static inline void fb_set_pixel_black(uint8_t *fb, int width_bytes, int width, int height, int x, int y)
{
    if (x < 0 || x >= width || y < 0 || y >= height)
        return;
    fb[y * width_bytes + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
}

static void fb_draw_char(uint8_t *fb, int wbytes, int w, int h, int x, int y, char c, int scale)
{
    if (c < 0x20 || c > 0x7E)
        c = '?';
    const uint8_t *glyph = &font5x7[(c - 0x20) * 5];

    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row)))
                continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                for (int sx = 0; sx < scale; sx++) {
                    fb_set_pixel_black(fb, wbytes, w, h, x + col * scale + sx, py);
                }
            }
        }
    }
}

static int fb_draw_string(uint8_t *fb, int wbytes, int w, int h, int x, int y, const char *s, int scale)
{
    int cx = x;
    int char_w = 6 * scale; /* 5px glyph + 1px spacing, scaled */
    for (int i = 0; s[i]; i++) {
        fb_draw_char(fb, wbytes, w, h, cx, y, s[i], scale);
        cx += char_w;
    }
    return cx;
}

static void fb_draw_hline(uint8_t *fb, int wbytes, int w, int h, int x0, int x1, int y, int thickness)
{
    for (int t = 0; t < thickness; t++)
        for (int x = x0; x <= x1; x++)
            fb_set_pixel_black(fb, wbytes, w, h, x, y + t);
}

/* ---- Status rendering ---- */

static void format_ip(char *out, size_t out_sz, uint32_t ip)
{
    if (ip == 0) {
        snprintf(out, out_sz, "No IP");
        return;
    }
    ip4_addr_t addr;
    addr.addr = ip;
    snprintf(out, out_sz, IPSTR, IP2STR(&addr));
}

static void render_status(freeink::FreeInkDisplay &display)
{
    uint8_t *fb = display.getFrameBuffer();
    if (!fb)
        return;

    const int w = display.getDisplayWidth();
    const int h = display.getDisplayHeight();
    const int wbytes = display.getDisplayWidthBytes();

    display.clearScreen(0xFF);

    char line[64];
    char ipbuf[20];
    const int margin = 24;
    int y = 24;

    fb_draw_string(fb, wbytes, w, h, margin, y, "ESP32 NAT Router", 6);
    y += 7 * 6 + 18;
    fb_draw_hline(fb, wbytes, w, h, margin, w - margin, y, 2);
    y += 26;

    const int scale = 3;
    const int line_h = 7 * scale + 16;

    snprintf(line, sizeof(line), "AP:      %s", (ap_ssid && ap_ssid[0]) ? ap_ssid : "NO AP");
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    if (ap_connect) {
        const char *status = vpn_is_connected() ? "VPN" : "UP";
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            snprintf(line, sizeof(line), "Uplink:  %s (%s, %ddBm)", ssid ? ssid : "?", status, ap_info.rssi);
        } else {
            snprintf(line, sizeof(line), "Uplink:  %s (%s)", ssid ? ssid : "?", status);
        }
    } else {
        snprintf(line, sizeof(line), "Uplink:  DOWN");
    }
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    format_ip(ipbuf, sizeof(ipbuf), my_ap_ip);
    snprintf(line, sizeof(line), "AP IP:   %s", ipbuf);
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    if (ap_connect && my_ip != 0) {
        format_ip(ipbuf, sizeof(ipbuf), my_ip);
    } else {
        snprintf(ipbuf, sizeof(ipbuf), "No IP");
    }
    snprintf(line, sizeof(line), "STA IP:  %s", ipbuf);
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    snprintf(line, sizeof(line), "Clients: %u", (unsigned)connect_count);
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    snprintf(line, sizeof(line), "TX/RX:   %.1f / %.1f MB",
             sta_bytes_sent / (1024.0 * 1024.0),
             sta_bytes_received / (1024.0 * 1024.0));
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    snprintf(line, sizeof(line), "Heap:    %lu KB free", (unsigned long)(esp_get_free_heap_size() / 1024));
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
}

/* ---- FreeRTOS task ---- */

static void eink_task(void *arg)
{
    (void)arg;

    /* Required once before any Arduino/FreeInk API call; nothing else in this
     * firmware runs on the Arduino layer (CONFIG_AUTOSTART_ARDUINO=n keeps
     * Arduino from installing its own app_main). */
    initArduino();

    /* Pins are informational here -- begin() actually sources wiring from
     * BoardConfig::ACTIVE, which defaults to XTEINK_X4 since this build only
     * compiles in FREEINK_DEVICE_X4. */
    const auto &pins = BoardConfig::XTEINK_X4.display;
    freeink::FreeInkDisplay display(pins.sclk, pins.mosi, pins.cs, pins.dc, pins.rst, pins.busy);
    display.begin();

    if (!display.framebufferReady()) {
        ESP_LOGE(TAG, "Framebuffer allocation failed (out of RAM) - stopping");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Xteink X4 e-ink display running (%dx%d)",
             display.getDisplayWidth(), display.getDisplayHeight());

    int resync_counter = 0;
    int refresh_counter = 0;
    bool first = true;

    while (true) {
        if (++resync_counter >= EINK_RESYNC_EVERY) {
            resync_connect_count();
            resync_counter = 0;
        }

        render_status(display);

        freeink::FreeInkDisplay::RefreshMode mode = freeink::FreeInkDisplay::FAST_REFRESH;
        if (first) {
            mode = freeink::FreeInkDisplay::FULL_REFRESH;
            first = false;
        } else if (++refresh_counter >= EINK_HALF_REFRESH_EVERY) {
            mode = freeink::FreeInkDisplay::HALF_REFRESH;
            refresh_counter = 0;
        }

        display.displayBuffer(mode);

        vTaskDelay(pdMS_TO_TICKS(EINK_UPDATE_INTERVAL_MS));
    }
}

/* ---- Public API ---- */

void eink_display_init(void)
{
    bool enabled = false;
    eink_display_get_config(&enabled);

    if (!enabled) {
        ESP_LOGI(TAG, "E-ink display disabled");
        return;
    }

    /* FreeInk's framebuffer alone is ~48KB (single-buffer mode) plus the
     * Arduino/SPI runtime -- keep this in mind alongside other RAM-hungry
     * features (VPN, PCAP, many clients) on the C3's tight heap. */
    xTaskCreate(eink_task, "eink", 6144, NULL, 2, NULL);
}

void eink_display_enable(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "eink_en", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void eink_display_disable(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "eink_en", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void eink_display_get_config(bool *enabled)
{
    nvs_handle_t nvs;
    int32_t val;

    *enabled = false;

    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_i32(nvs, "eink_en", &val) == ESP_OK)
        *enabled = (val != 0);

    nvs_close(nvs);
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */
