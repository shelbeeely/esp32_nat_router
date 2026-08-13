#pragma once

/* Mesh uplink discovery (Phase 1): routers announce whether they currently
 * have real WAN uplink over Reticulum, and every router keeps a live table
 * of announced peers so a human can see who's reachable and has internet.
 *
 * Deliberately NOT included here: an actual IP tunnel over the mesh, NAT
 * bridging on the gateway side, or automatic default-route failover. Every
 * router in this mesh can already forward IP traffic between its own AP
 * clients and its own WAN uplink (that's this firmware's whole job) --
 * routing a *different* router's client traffic through this one over
 * Reticulum needs a virtual tunnel netif pumped over an RNS Link, with NAT
 * on the gateway side reusing this project's existing ip_napt_enable()
 * mechanism (see main/esp32_nat_router.c's ip_napt_enable(my_ap_ip, 1) calls
 * for the pattern -- NAPT here is scoped by local IP/subnet, not netif, so a
 * tunnel netif representing the leaf's subnet should npt the same way).
 * That's real, novel lwIP surgery on the router's core forwarding path with
 * zero ability to build/test it here, so it's being staged as a deliberate
 * next increment instead of written blind alongside this. This module's
 * gateway table (hash, label, uplink state, hop count) is exactly the input
 * that increment's gateway selection/failover would consume.
 */

/* Needed for CONFIG_IDF_TARGET_ESP32C3 below -- see eink_display.h's copy of
 * this comment for why it can't be left to transitive inclusion. */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)

/**
 * @brief Start mesh uplink announce + discovery. Call after
 * microreticulum_init() has brought up Reticulum. No-op if Reticulum itself
 * is disabled.
 */
void mesh_uplink_init(void);

/**
 * @brief Call once per Reticulum task loop iteration (alongside
 * reticulum.loop()) to re-announce on a timer/state change and sweep stale
 * gateway table entries. Cheap no-op most calls.
 */
void mesh_uplink_tick(void);

#define MESH_UPLINK_MAX_GATEWAYS 16
#define MESH_UPLINK_LABEL_MAXLEN 32
#define MESH_UPLINK_HASH_HEX_LEN 33 /* 16 raw hash bytes, hex-encoded + NUL */

typedef struct {
    char hash_hex[MESH_UPLINK_HASH_HEX_LEN];
    char label[MESH_UPLINK_LABEL_MAXLEN];
    bool uplink_available;
    uint8_t hops;       /* 255 if no path currently known */
    uint32_t age_secs;  /* seconds since this peer's last announce */
} mesh_uplink_gateway_t;

/**
 * @brief Snapshot the known-gateway table (peers seen announcing the
 * "nat_router.uplink" aspect), newest-seen first.
 * @return Number of entries written into `out` (up to `max`).
 */
int mesh_uplink_list_gateways(mesh_uplink_gateway_t *out, int max);

/**
 * @brief This router's own mesh uplink destination hash (hex), and whether
 * it is currently announcing itself as having real WAN uplink.
 */
void mesh_uplink_get_self(char *hash_hex_out, size_t hash_hex_out_len, bool *uplink_available);

#else /* !CONFIG_IDF_TARGET_ESP32C3 */

static inline void mesh_uplink_init(void) {}
static inline void mesh_uplink_tick(void) {}
static inline int mesh_uplink_list_gateways(void *out, int max) { (void)out; (void)max; return 0; }
static inline void mesh_uplink_get_self(char *hash_hex_out, size_t hash_hex_out_len, bool *uplink_available) {
    if (hash_hex_out && hash_hex_out_len) hash_hex_out[0] = '\0';
    if (uplink_available) *uplink_available = false;
}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */

#ifdef __cplusplus
}
#endif
