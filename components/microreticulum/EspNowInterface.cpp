#include "EspNowInterface.h"

#include <microReticulum/Transport.h>
#include <microReticulum/Log.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "espnow_if";

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

using namespace RNS;

EspNowInterface *EspNowInterface::s_instance = nullptr;

EspNowInterface::EspNowInterface(const char *name /*= "EspNowInterface"*/)
    : RNS::InterfaceImpl(name)
{
    _IN = true;
    _OUT = true;
    _bitrate = BITRATE_GUESS;
    _HW_MTU = ESP_NOW_MAX_DATA_LEN; /* 250 bytes -- see the header's known-gap note */
}

EspNowInterface::~EspNowInterface() {
    stop();
}

bool EspNowInterface::start() {
    _online = false;

    if (s_instance != nullptr) {
        ESP_LOGE(TAG, "Only one EspNowInterface may be active at a time");
        return false;
    }

    _rxQueue = xQueueCreate(ESPNOW_QUEUE_LEN, sizeof(RxFrame));
    if (!_rxQueue) {
        ESP_LOGE(TAG, "Failed to allocate ESP-NOW rx queue");
        return false;
    }

    /* Requires esp_wifi already started in STA and/or AP mode -- true by the
     * time this runs, well after the router's own WiFi bring-up in app_main. */
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        vQueueDelete(_rxQueue);
        _rxQueue = nullptr;
        return false;
    }

    s_instance = this;
    esp_now_register_recv_cb(recvCallback);
    esp_now_register_send_cb(sendCallback);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0; /* current channel */
    peer.encrypt = false;
    peer.ifidx = WIFI_IF_AP;
    if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
        err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_add_peer (broadcast) failed: %s", esp_err_to_name(err));
            esp_now_deinit();
            s_instance = nullptr;
            vQueueDelete(_rxQueue);
            _rxQueue = nullptr;
            return false;
        }
    }

    _initialized = true;
    _online = true;
    return true;
}

void EspNowInterface::stop() {
    if (_initialized) {
        esp_now_deinit();
        _initialized = false;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (_rxQueue) {
        vQueueDelete(_rxQueue);
        _rxQueue = nullptr;
    }
    _online = false;
}

void EspNowInterface::loop() {
    if (!_online || !_rxQueue)
        return;

    RxFrame frame;
    while (xQueueReceive(_rxQueue, &frame, 0) == pdTRUE) {
        Bytes data(frame.data, frame.len);
        on_incoming(data);
    }
}

bool EspNowInterface::send_outgoing(const Bytes &data) {
    if (!_online)
        return false;

    if (data.size() > ESP_NOW_MAX_DATA_LEN) {
        WARNINGF("EspNowInterface: dropping %u-byte packet, exceeds ESP-NOW's %u-byte limit "
                 "(no fragmentation implemented)",
                 (unsigned)data.size(), (unsigned)ESP_NOW_MAX_DATA_LEN);
        return false;
    }

    esp_err_t err = esp_now_send(BROADCAST_MAC, data.data(), data.size());
    if (err != ESP_OK) {
        WARNINGF("EspNowInterface: esp_now_send failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void EspNowInterface::on_incoming(const Bytes &data) {
    InterfaceImpl::handle_incoming(data);
}

/*static*/ void EspNowInterface::recvCallback(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    (void)info;
    if (!s_instance || !s_instance->_rxQueue || len <= 0 || len > ESP_NOW_MAX_DATA_LEN)
        return;

    RxFrame frame;
    frame.len = (uint8_t)len;
    memcpy(frame.data, data, len);

    /* Called from the WiFi driver's task context -- queue and return,
     * never block here. */
    xQueueSend(s_instance->_rxQueue, &frame, 0);
}

/*static*/ void EspNowInterface::sendCallback(const uint8_t *mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "ESP-NOW send failed");
    }
}
