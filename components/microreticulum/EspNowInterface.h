#pragma once

/* ESP-NOW interface for Reticulum: mesh discovery/messaging directly between
 * ESP32 radios (broadcast, no WiFi association/AP needed), for router-to-
 * router links between multiple deployed X4 units -- independent of whatever
 * WiFi AP/STA state either unit's uplink is in.
 *
 * Unlike RouterUdpInterface, this is plain ESP-IDF (esp_now.h), no Arduino --
 * ESP-NOW doesn't need it, and this keeps the one genuinely Arduino-only part
 * of this component (the Reticulum library itself, on ESP32) from spreading
 * to interfaces that don't need to pay for it.
 *
 * KNOWN GAP: ESP-NOW's payload limit is 250 bytes (ESP_NOW_MAX_DATA_LEN), well
 * under Reticulum's default 500-byte packet MTU (Type::Reticulum::MTU). This
 * interface does not fragment/reassemble -- send_outgoing() drops (returns
 * false, logs) any packet over 250 bytes rather than silently truncating it.
 * Small packets (announces, link requests, short messages) fit; resource
 * transfers and anything using the full MTU will not go out this interface.
 * Not verified against real hardware -- see microreticulum.h's known-gaps note.
 */

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdint.h>

#define ESPNOW_QUEUE_LEN 16

class EspNowInterface : public RNS::InterfaceImpl {
public:
    static const uint32_t BITRATE_GUESS = 1 * 1000 * 1000; /* ESP-NOW is ~1Mbps-class */

    explicit EspNowInterface(const char *name = "EspNowInterface");
    virtual ~EspNowInterface();

    virtual bool start();
    virtual void stop();
    virtual void loop();

    virtual inline std::string toString() const { return "EspNowInterface[" + _name + "]"; }

protected:
    virtual bool send_outgoing(const RNS::Bytes &data);
    void on_incoming(const RNS::Bytes &data);

private:
    static void recvCallback(const esp_now_recv_info_t *info, const uint8_t *data, int len);
    /* esp_now_send_cb_t's signature in this ESP-IDF version takes
     * esp_now_send_info_t (carries the peer MAC plus tx info), not the
     * older bare MAC-address pointer -- caught by the real build, not
     * something documented anywhere I'd checked. */
    static void sendCallback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);

    struct RxFrame {
        uint8_t data[ESP_NOW_MAX_DATA_LEN];
        uint8_t len;
    };

    /* ESP-NOW's recv callback runs in the WiFi driver's own task context with
     * no user-data pointer, so it can't call an arbitrary instance's
     * on_incoming() directly -- it hands frames to this queue instead, and
     * loop() (called from the Reticulum task) drains it. Only one
     * EspNowInterface is expected to be active at a time. */
    static EspNowInterface *s_instance;

    QueueHandle_t _rxQueue = nullptr;
    bool _initialized = false;
};
