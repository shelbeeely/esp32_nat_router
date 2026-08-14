#include "RouterUdpInterface.h"

#include <microReticulum/Transport.h>
#include <microReticulum/Log.h>

using namespace RNS;

RouterUdpInterface::RouterUdpInterface(const char *name /*= "RouterUdpInterface"*/)
    : RNS::InterfaceImpl(name)
{
    _IN = true;
    _OUT = true;
    _bitrate = BITRATE_GUESS;
    _HW_MTU = 1064;
}

RouterUdpInterface::~RouterUdpInterface() {
    stop();
}

bool RouterUdpInterface::start() {
    _online = false;

    /* The router's own WiFi (AP + optional STA uplink) is already up by the
     * time microreticulum_init() runs -- unlike upstream's UDPInterface,
     * this does not call WiFi.begin() itself. */
    if (udp.begin(RNS_UDP_PORT) == 0) {
        ERRORF("RouterUdpInterface: udp.begin(%d) failed", RNS_UDP_PORT);
        return false;
    }

    _online = true;
    return true;
}

void RouterUdpInterface::stop() {
    udp.stop();
    _online = false;
}

void RouterUdpInterface::loop() {
    if (!_online)
        return;

    udp.parsePacket();
    size_t len = udp.read(_buffer.writable(Type::Reticulum::MTU), Type::Reticulum::MTU);
    if (len > 0) {
        _buffer.resize(len);
        on_incoming(_buffer);
    }
}

bool RouterUdpInterface::send_outgoing(const Bytes &data) {
    if (!_online)
        return false;

    /* Global broadcast, matching real Reticulum's UDP interface convention
     * (DEFAULT_UDP_REMOTE_HOST in upstream's example). Reaches the AP LAN;
     * an STA-side uplink is a routed WAN hop away and out of scope here. */
    udp.beginPacket(IPAddress(255, 255, 255, 255), RNS_UDP_PORT);
    udp.write(data.data(), data.size());
    udp.endPacket();
    return true;
}

void RouterUdpInterface::on_incoming(const Bytes &data) {
    InterfaceImpl::handle_incoming(data);
}
