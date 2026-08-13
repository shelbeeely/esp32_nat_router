#include "MeshUplink.h"

#if defined(CONFIG_IDF_TARGET_ESP32C3)

#include <microReticulum.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <map>
#include <string>
#include <stdio.h>
#include <string.h>

/* Router uplink state, defined in main/esp32_nat_router.c (same globals
 * eink_display.cpp already reads for its status screen). */
extern "C" {
extern bool ap_connect;
extern uint32_t my_ip;
extern char *ap_ssid;
}

static const char *TAG = "mesh_uplink";

#define RNS_STORAGE_PATH "/data/reticulum" /* matches microreticulum.cpp */
#define UPLINK_APP_NAME "nat_router"
#define UPLINK_ASPECT "uplink"
#define ANNOUNCE_INTERVAL_SECS 60
#define GATEWAY_STALE_SECS 600 /* drop peers not heard from in 10 minutes */

namespace {

struct GatewayEntry {
    std::string label;
    bool uplinkAvailable = false;
    double lastSeen = 0;
};

/* Keyed by hex destination hash. Guarded by a mutex: received_announce() is
 * called from the Reticulum task's own loop() (see microreticulum.cpp), and
 * the console (a different task) reads it via mesh_uplink_list_gateways(). */
std::map<std::string, GatewayEntry> g_gateways;
SemaphoreHandle_t g_gatewaysMutex = nullptr;

RNS::Identity g_identity({RNS::Type::NONE});
RNS::Destination g_destination({RNS::Type::NONE});
double g_lastAnnounce = 0;
bool g_lastAnnouncedUplink = false;

class UplinkAnnounceHandler : public RNS::AnnounceHandler {
public:
    UplinkAnnounceHandler() : RNS::AnnounceHandler(UPLINK_APP_NAME "." UPLINK_ASPECT) {}

    void received_announce(const RNS::Bytes &destination_hash, const RNS::Identity &announced_identity,
                            const RNS::Bytes &app_data) override {
        (void)announced_identity;

        if (app_data.size() < 1)
            return;

        const uint8_t *raw = app_data.data();
        bool uplinkAvailable = raw[0] != 0;
        std::string label;
        if (app_data.size() > 1) {
            label.assign((const char *)raw + 1, app_data.size() - 1);
        }

        std::string hashHex = destination_hash.toHex();

        if (g_gatewaysMutex && xSemaphoreTake(g_gatewaysMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            GatewayEntry &entry = g_gateways[hashHex];
            entry.label = label;
            entry.uplinkAvailable = uplinkAvailable;
            entry.lastSeen = RNS::Utilities::OS::time();
            xSemaphoreGive(g_gatewaysMutex);
        }

        ESP_LOGI(TAG, "Mesh peer %s: uplink=%s label=%s", hashHex.c_str(),
                 uplinkAvailable ? "yes" : "no", label.c_str());
    }
};

UplinkAnnounceHandler *g_handler = nullptr;

} // namespace

void mesh_uplink_init(void)
{
    g_gatewaysMutex = xSemaphoreCreateMutex();

    /* Stable identity (and therefore stable destination hash) across
     * reboots, persisted alongside the rest of Reticulum's storage. */
    const char *identityPath = RNS_STORAGE_PATH "/mesh_uplink_identity";
    microStore::FileSystem &fs = RNS::Utilities::OS::get_filesystem();
    if (fs && fs.exists(identityPath)) {
        g_identity = RNS::Identity::from_file(identityPath);
    }
    if (!g_identity) {
        g_identity = RNS::Identity();
        if (fs) {
            g_identity.to_file(identityPath);
        }
    }

    g_destination = RNS::Destination(g_identity, RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE,
                                      UPLINK_APP_NAME, UPLINK_ASPECT);

    g_handler = new UplinkAnnounceHandler();
    RNS::Transport::register_announce_handler(RNS::HAnnounceHandler(g_handler));

    ESP_LOGI(TAG, "Mesh uplink discovery running (destination %s)", g_destination.hash().toHex().c_str());
}

/* Called every iteration of the Reticulum task's loop (see
 * microreticulum.cpp) -- checks whether this router's own uplink state
 * changed or the announce interval elapsed, and re-announces if so. Also
 * sweeps stale peers out of the gateway table. Cheap no-op most ticks. */
void mesh_uplink_tick(void)
{
    if (!g_destination)
        return;

    bool uplinkAvailable = ap_connect && my_ip != 0;
    double now = RNS::Utilities::OS::time();

    bool dueForAnnounce = (now - g_lastAnnounce) >= ANNOUNCE_INTERVAL_SECS;
    bool stateChanged = uplinkAvailable != g_lastAnnouncedUplink;

    if (dueForAnnounce || stateChanged) {
        RNS::Bytes appData;
        appData.append((uint8_t)(uplinkAvailable ? 1 : 0));
        const char *ssid = (ap_ssid && ap_ssid[0]) ? ap_ssid : "nat_router";
        appData.append(ssid);

        g_destination.announce(appData);
        g_lastAnnounce = now;
        g_lastAnnouncedUplink = uplinkAvailable;
    }

    static double lastSweep = 0;
    if (now - lastSweep >= 30 && g_gatewaysMutex && xSemaphoreTake(g_gatewaysMutex, 0) == pdTRUE) {
        lastSweep = now;
        for (auto it = g_gateways.begin(); it != g_gateways.end();) {
            if (now - it->second.lastSeen > GATEWAY_STALE_SECS) {
                it = g_gateways.erase(it);
            } else {
                ++it;
            }
        }
        xSemaphoreGive(g_gatewaysMutex);
    }
}

int mesh_uplink_list_gateways(mesh_uplink_gateway_t *out, int max)
{
    if (!out || max <= 0 || !g_gatewaysMutex)
        return 0;

    int count = 0;
    if (xSemaphoreTake(g_gatewaysMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        double now = RNS::Utilities::OS::time();
        for (const auto &kv : g_gateways) {
            if (count >= max)
                break;

            mesh_uplink_gateway_t &g = out[count];
            snprintf(g.hash_hex, sizeof(g.hash_hex), "%s", kv.first.c_str());
            snprintf(g.label, sizeof(g.label), "%s", kv.second.label.c_str());
            g.uplink_available = kv.second.uplinkAvailable;
            g.age_secs = (uint32_t)(now - kv.second.lastSeen);

            RNS::Bytes hashBytes;
            hashBytes.appendHex(kv.first.c_str());
            g.hops = RNS::Transport::has_path(hashBytes) ? RNS::Transport::hops_to(hashBytes) : 255;

            count++;
        }
        xSemaphoreGive(g_gatewaysMutex);
    }
    return count;
}

void mesh_uplink_get_self(char *hash_hex_out, size_t hash_hex_out_len, bool *uplink_available)
{
    if (hash_hex_out && hash_hex_out_len) {
        if (g_destination) {
            snprintf(hash_hex_out, hash_hex_out_len, "%s", g_destination.hash().toHex().c_str());
        } else {
            hash_hex_out[0] = '\0';
        }
    }
    if (uplink_available) {
        *uplink_available = g_lastAnnouncedUplink;
    }
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */
