#pragma once

/* UDP interface for the router's Reticulum transport node.
 *
 * Modeled closely on microReticulum's own examples/common/udp_interface
 * (UDPInterface), which is the only Arduino-ESP32 interface implementation
 * upstream actually exercises. The difference: that example owns its own
 * WiFi connection (calls WiFi.begin() itself, for a single-purpose LoRa/WiFi
 * board); this one does not -- the router already brings up and manages its
 * AP/STA interfaces, so this just opens a WiFiUDP socket and broadcasts on
 * whatever's already connected, matching real Reticulum's UDP interface
 * (auto peer discovery via broadcast).
 */

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#include <WiFi.h>
#include <WiFiUdp.h>

#include <stdint.h>
#include <string>

#define RNS_UDP_PORT 4242

class RouterUdpInterface : public RNS::InterfaceImpl {
public:
    static const uint32_t BITRATE_GUESS = 10 * 1000 * 1000;

    explicit RouterUdpInterface(const char *name = "RouterUdpInterface");
    virtual ~RouterUdpInterface();

    virtual bool start();
    virtual void stop();
    virtual void loop();

    virtual inline std::string toString() const {
        return "RouterUdpInterface[" + _name + "/broadcast:" + std::to_string(RNS_UDP_PORT) + "]";
    }

protected:
    virtual bool send_outgoing(const RNS::Bytes &data);
    void on_incoming(const RNS::Bytes &data);

private:
    RNS::Bytes _buffer;
    WiFiUDP udp;
};
