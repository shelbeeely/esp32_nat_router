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
 *
 * On-device settings menu: pressing CONFIRM on the status screen switches to
 * a button-driven menu (UP/DOWN to move, CONFIRM to toggle, BACK to exit),
 * scoped for now to the boolean features that already have a plain enable/
 * disable C API and an existing "changes apply after reboot" convention
 * (eink display itself, Reticulum, battery/power monitoring), plus a reboot
 * action. Free-text settings (WiFi SSID/password, static IP, etc.) stay on
 * the web UI/console -- a 6-button on-screen keyboard isn't worth building
 * for those. Button input comes from freeink_hw_poll_button_edges(), which
 * polls unconditionally regardless of freeink_hw's own battery/power-off
 * enable flag (see freeink_hw.h).
 *
 * Status screen is paged (LEFT/RIGHT cycles Overview/Clients/System, mirrors
 * Meshtastic InkHUD's applet-tiling idea) rather than one screen with
 * everything crammed on -- see StatusPage below. Auto-show (also from
 * InkHUD) jumps to the relevant page when a client connects/disconnects or
 * the uplink changes state, rate-limited so a flapping client can't trigger
 * a refresh storm; toggle it from the settings menu.
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
#include "freeink_hw.h"
#include "microreticulum.h"
#include "router_config.h"
#include "dhcp_reservations.h"

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

/* Auto-show (Meshtastic InkHUD calls this the same thing): jump to the page
 * relevant to a state change -- a client connecting/disconnecting, or the
 * uplink going up/down -- instead of only surfacing it once the periodic
 * refresh happens to redraw whatever page is currently showing. Read once at
 * task start, same "changes apply after reboot" convention as the other menu
 * toggles (see kMenuItems below). */
#define AUTO_SHOW_NVS_KEY "eink_autoshow"

static void get_auto_show_config(bool *enabled)
{
    nvs_handle_t nvs;
    int32_t val;

    *enabled = true; /* on by default */

    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_i32(nvs, AUTO_SHOW_NVS_KEY, &val) == ESP_OK)
        *enabled = (val != 0);

    nvs_close(nvs);
}

static void set_auto_show_config(bool enabled)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, AUTO_SHOW_NVS_KEY, enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}
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

enum StatusPage {
    STATUS_PAGE_OVERVIEW,
    STATUS_PAGE_CLIENTS,
    STATUS_PAGE_SYSTEM,
    STATUS_PAGE_COUNT,
};

static const char *kStatusPageTitles[STATUS_PAGE_COUNT] = {
    "ESP32 NAT Router",
    "Connected Clients",
    "System",
};

/* Title + "page/count" indicator (font is fixed-width, so the indicator's
 * pixel width is computable without a dry-run draw) + separator rule.
 * Returns the y to start page content at. */
static int draw_page_header(uint8_t *fb, int wbytes, int w, int h, const char *title, int page, int pageCount)
{
    const int margin = 24;
    int y = 24;

    fb_draw_string(fb, wbytes, w, h, margin, y, title, 6);

    /* Sized for the compiler's worst-case %d width (a full int), not just the
     * single-digit page counts this actually ever sees -- GCC's
     * -Wformat-truncation can't know the runtime range is tiny. */
    char pageStr[24];
    snprintf(pageStr, sizeof(pageStr), "%d/%d", page + 1, pageCount);
    const int indicatorScale = 3;
    int indicatorW = (int)strlen(pageStr) * 6 * indicatorScale;
    fb_draw_string(fb, wbytes, w, h, w - margin - indicatorW, y, pageStr, indicatorScale);

    y += 7 * 6 + 18;
    fb_draw_hline(fb, wbytes, w, h, margin, w - margin, y, 2);
    y += 26;
    return y;
}

static void render_status_overview(uint8_t *fb, int wbytes, int w, int h, int y)
{
    char line[64];
    char ipbuf[20];
    const int margin = 24;
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
}

static void render_status_clients(uint8_t *fb, int wbytes, int w, int h, int y)
{
    const int margin = 24;
    const int scale = 3;
    const int line_h = 7 * scale + 14;

    connected_client_t clients[AP_MAX_CONNECTIONS];
    int n = get_connected_clients(clients, AP_MAX_CONNECTIONS);

    if (n <= 0) {
        fb_draw_string(fb, wbytes, w, h, margin, y, "No clients connected", scale);
        return;
    }

    for (int i = 0; i < n; i++) {
        char line[64];
        char ipbuf[20];
        const char *name = clients[i].name[0] ? clients[i].name : "(unknown)";
        if (clients[i].has_ip)
            format_ip(ipbuf, sizeof(ipbuf), clients[i].ip);
        else
            snprintf(ipbuf, sizeof(ipbuf), "no IP");
        snprintf(line, sizeof(line), "%-16.16s %s", name, ipbuf);
        fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
        y += line_h;
    }
}

static void render_status_system(uint8_t *fb, int wbytes, int w, int h, int y)
{
    char line[64];
    const int margin = 24;
    const int scale = 3;
    const int line_h = 7 * scale + 16;

    char uptimeBuf[32];
    format_uptime(get_uptime_seconds(), uptimeBuf, sizeof(uptimeBuf));
    snprintf(line, sizeof(line), "Uptime:  %s", uptimeBuf);
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    snprintf(line, sizeof(line), "Heap:    %lu KB free", (unsigned long)(esp_get_free_heap_size() / 1024));
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
    y += line_h;

    bool hwEnabled = false;
    freeink_hw_get_config(&hwEnabled);
    if (hwEnabled) {
        snprintf(line, sizeof(line), "Battery: %u%%%s", (unsigned)freeink_hw_get_battery_percent(),
                 freeink_hw_is_charging() ? " (charging)" : "");
    } else {
        snprintf(line, sizeof(line), "Battery: monitoring disabled");
    }
    fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
}

static void render_status(freeink::FreeInkDisplay &display, int page)
{
    uint8_t *fb = display.getFrameBuffer();
    if (!fb)
        return;

    const int w = display.getDisplayWidth();
    const int h = display.getDisplayHeight();
    const int wbytes = display.getDisplayWidthBytes();

    display.clearScreen(0xFF);

    int y = draw_page_header(fb, wbytes, w, h, kStatusPageTitles[page], page, STATUS_PAGE_COUNT);

    switch (page) {
        case STATUS_PAGE_OVERVIEW: render_status_overview(fb, wbytes, w, h, y); break;
        case STATUS_PAGE_CLIENTS:  render_status_clients(fb, wbytes, w, h, y); break;
        case STATUS_PAGE_SYSTEM:   render_status_system(fb, wbytes, w, h, y); break;
        default: break;
    }

    const int margin = 24;
    int footerY = h - 40;
    fb_draw_hline(fb, wbytes, w, h, margin, w - margin, footerY - 14, 1);
    fb_draw_string(fb, wbytes, w, h, margin, footerY, "LEFT/RIGHT page   CONFIRM menu", 2);
}

/* ---- On-device settings menu ---- */

enum MenuAction {
    MENU_TOGGLE_EINK,
    MENU_TOGGLE_RETICULUM,
    MENU_TOGGLE_FREEINK_HW,
    MENU_TOGGLE_AUTOSHOW,
    MENU_REBOOT,
};

struct MenuItem {
    const char *label;
    MenuAction action;
    bool isToggle;
};

static const MenuItem kMenuItems[] = {
    { "E-Ink Display",      MENU_TOGGLE_EINK,       true },
    { "Reticulum Mesh",     MENU_TOGGLE_RETICULUM,  true },
    { "Battery/Power Mon.", MENU_TOGGLE_FREEINK_HW, true },
    { "Auto-Show Events",   MENU_TOGGLE_AUTOSHOW,   true },
    { "Reboot Now",         MENU_REBOOT,            false },
};
static const int kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);

static void render_menu(freeink::FreeInkDisplay &display, int selected)
{
    uint8_t *fb = display.getFrameBuffer();
    if (!fb)
        return;

    const int w = display.getDisplayWidth();
    const int h = display.getDisplayHeight();
    const int wbytes = display.getDisplayWidthBytes();

    display.clearScreen(0xFF);

    const int margin = 24;
    int y = 24;

    fb_draw_string(fb, wbytes, w, h, margin, y, "Settings", 6);
    y += 7 * 6 + 18;
    fb_draw_hline(fb, wbytes, w, h, margin, w - margin, y, 2);
    y += 26;

    const int scale = 3;
    const int line_h = 7 * scale + 16;

    bool einkEn = false, rnsEn = false, fkhwEn = false, autoShowEn = false;
    eink_display_get_config(&einkEn);
    microreticulum_get_config(&rnsEn);
    freeink_hw_get_config(&fkhwEn);
    get_auto_show_config(&autoShowEn);

    char line[64];
    for (int i = 0; i < kMenuItemCount; i++) {
        const char *cursor = (i == selected) ? "> " : "  ";
        if (kMenuItems[i].isToggle) {
            bool val = false;
            switch (kMenuItems[i].action) {
                case MENU_TOGGLE_EINK:       val = einkEn; break;
                case MENU_TOGGLE_RETICULUM:  val = rnsEn; break;
                case MENU_TOGGLE_FREEINK_HW: val = fkhwEn; break;
                case MENU_TOGGLE_AUTOSHOW:   val = autoShowEn; break;
                default: break;
            }
            snprintf(line, sizeof(line), "%s%-18s [%s]", cursor, kMenuItems[i].label, val ? "ON" : "OFF");
        } else {
            snprintf(line, sizeof(line), "%s%s", cursor, kMenuItems[i].label);
        }
        fb_draw_string(fb, wbytes, w, h, margin, y, line, scale);
        y += line_h;
    }

    y += line_h / 2;
    fb_draw_hline(fb, wbytes, w, h, margin, w - margin, y, 1);
    y += 20;
    fb_draw_string(fb, wbytes, w, h, margin, y, "UP/DOWN move   CONFIRM select   BACK exit", 2);
    y += 7 * 2 + 12;
    fb_draw_string(fb, wbytes, w, h, margin, y, "Toggles take effect after reboot.", 2);
}

/* Applies edges to menu navigation/selection. Returns true if the menu
 * should close (BACK pressed at the top level -- there are no submenus yet
 * to back out of one level at a time). */
static bool handle_menu_edges(uint8_t edges, int &selected)
{
    if (edges & FREEINK_HW_BTN_BACK)
        return true;

    if (edges & FREEINK_HW_BTN_UP)
        selected = (selected - 1 + kMenuItemCount) % kMenuItemCount;
    if (edges & FREEINK_HW_BTN_DOWN)
        selected = (selected + 1) % kMenuItemCount;

    if (edges & FREEINK_HW_BTN_CONFIRM) {
        switch (kMenuItems[selected].action) {
            case MENU_TOGGLE_EINK: {
                bool en = false;
                eink_display_get_config(&en);
                en ? eink_display_disable() : eink_display_enable();
                break;
            }
            case MENU_TOGGLE_RETICULUM: {
                bool en = false;
                microreticulum_get_config(&en);
                en ? microreticulum_disable() : microreticulum_enable();
                break;
            }
            case MENU_TOGGLE_FREEINK_HW: {
                bool en = false;
                freeink_hw_get_config(&en);
                en ? freeink_hw_disable() : freeink_hw_enable();
                break;
            }
            case MENU_TOGGLE_AUTOSHOW: {
                bool en = false;
                get_auto_show_config(&en);
                set_auto_show_config(!en);
                break;
            }
            case MENU_REBOOT:
                esp_restart();
                break;
        }
    }

    return false;
}

/* ---- FreeRTOS task ---- */

/* Buttons are polled every tick regardless of display mode (status vs menu)
 * so pressing CONFIRM on the status screen is picked up promptly; the
 * e-paper itself is only ever repainted when something actually changed
 * (periodic status refresh, or a menu edge), not on every tick. */
#define EINK_TICK_MS                 100
#define EINK_STATUS_REFRESH_TICKS    (EINK_UPDATE_INTERVAL_MS / EINK_TICK_MS)
#define EINK_MENU_IDLE_TIMEOUT_TICKS (20000 / EINK_TICK_MS) /* back to status after 20s idle */
/* Minimum gap between auto-show page jumps -- without this, a flapping
 * client (weak signal, reconnecting repeatedly) would trigger a flash-y
 * e-paper refresh storm. The periodic EINK_STATUS_REFRESH_TICKS redraw
 * separately still shows the change within 5s regardless of this cooldown. */
#define EINK_AUTO_SHOW_COOLDOWN_TICKS (5000 / EINK_TICK_MS)

enum EinkMode { EINK_MODE_STATUS, EINK_MODE_MENU };

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

    bool autoShowEnabled = false;
    get_auto_show_config(&autoShowEnabled);
    uint16_t lastConnectCount = connect_count;
    bool lastApConnect = ap_connect;
    int ticksSinceAutoShow = EINK_AUTO_SHOW_COOLDOWN_TICKS; /* allow one immediately */

    EinkMode mode = EINK_MODE_STATUS;
    int statusPage = STATUS_PAGE_OVERVIEW;
    int menuSelected = 0;
    int statusTicks = 0;
    int menuIdleTicks = 0;
    int resync_counter = 0;
    int refresh_counter = 0;
    bool needRedraw = true;
    bool first = true;

    while (true) {
        uint8_t edges = freeink_hw_poll_button_edges();

        if (mode == EINK_MODE_STATUS) {
            if (edges & FREEINK_HW_BTN_CONFIRM) {
                mode = EINK_MODE_MENU;
                menuSelected = 0;
                menuIdleTicks = 0;
                needRedraw = true;
            } else if (edges & (FREEINK_HW_BTN_LEFT | FREEINK_HW_BTN_RIGHT)) {
                if (edges & FREEINK_HW_BTN_RIGHT)
                    statusPage = (statusPage + 1) % STATUS_PAGE_COUNT;
                if (edges & FREEINK_HW_BTN_LEFT)
                    statusPage = (statusPage - 1 + STATUS_PAGE_COUNT) % STATUS_PAGE_COUNT;
                statusTicks = 0;
                needRedraw = true;
            } else if (++statusTicks >= EINK_STATUS_REFRESH_TICKS) {
                statusTicks = 0;
                if (++resync_counter >= EINK_RESYNC_EVERY) {
                    resync_connect_count();
                    resync_counter = 0;
                }
                needRedraw = true;
            }

            /* Auto-show: jump to the page relevant to a state change instead
             * of only surfacing it once the periodic refresh above happens
             * to redraw whatever page is already showing. */
            if (autoShowEnabled) {
                ticksSinceAutoShow++;

                uint16_t curConnectCount = connect_count;
                bool curApConnect = ap_connect;
                bool clientsChanged = (curConnectCount != lastConnectCount);
                bool uplinkChanged = (curApConnect != lastApConnect);
                lastConnectCount = curConnectCount;
                lastApConnect = curApConnect;

                if (ticksSinceAutoShow >= EINK_AUTO_SHOW_COOLDOWN_TICKS) {
                    int targetPage = -1;
                    if (uplinkChanged)
                        targetPage = STATUS_PAGE_OVERVIEW;
                    else if (clientsChanged)
                        targetPage = STATUS_PAGE_CLIENTS;

                    if (targetPage >= 0 && targetPage != statusPage) {
                        statusPage = targetPage;
                        statusTicks = 0;
                        needRedraw = true;
                        ticksSinceAutoShow = 0;
                    }
                }
            }
        } else { /* EINK_MODE_MENU */
            if (edges) {
                menuIdleTicks = 0;
                bool exitMenu = handle_menu_edges(edges, menuSelected);
                needRedraw = true;
                if (exitMenu) {
                    mode = EINK_MODE_STATUS;
                    statusTicks = 0;
                }
            } else if (++menuIdleTicks >= EINK_MENU_IDLE_TIMEOUT_TICKS) {
                mode = EINK_MODE_STATUS;
                statusTicks = 0;
                needRedraw = true;
            }
        }

        if (needRedraw || first) {
            if (mode == EINK_MODE_STATUS)
                render_status(display, statusPage);
            else
                render_menu(display, menuSelected);

            freeink::FreeInkDisplay::RefreshMode refresh = freeink::FreeInkDisplay::FAST_REFRESH;
            if (first) {
                refresh = freeink::FreeInkDisplay::FULL_REFRESH;
                first = false;
            } else if (++refresh_counter >= EINK_HALF_REFRESH_EVERY) {
                refresh = freeink::FreeInkDisplay::HALF_REFRESH;
                refresh_counter = 0;
            }

            display.displayBuffer(refresh);
            needRedraw = false;
        }

        vTaskDelay(pdMS_TO_TICKS(EINK_TICK_MS));
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

    /* Enabled by default -- this is the X4's own screen, not an optional
     * add-on peripheral like the OLED, so it should just work out of the
     * box. 'set_eink disable' opts out and persists that choice; absence of
     * the NVS key (first boot, or after nvs_flash_erase) means "on". */
    *enabled = true;

    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_i32(nvs, "eink_en", &val) == ESP_OK)
        *enabled = (val != 0);

    nvs_close(nvs);
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */
