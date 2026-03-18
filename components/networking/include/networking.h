#pragma once

/**
 * @file networking.h
 * @brief WiFi station, SNTP time sync, IP geolocation, and network status
 *
 * Lifecycle
 * ─────────
 *  1. begin()         — init NVS, netif, event loop, start WiFi connect
 *  2. WiFi connected  → request IP geolocation → resolve POSIX TZ string
 *                     → call ClockManager::set_timezone()
 *                     → start SNTP
 *  3. SNTP synced     → call ClockManager::on_time_synced()
 *  4. Disconnect      → auto-reconnect with exponential back-off
 *
 * Geolocation
 * ───────────
 *  Uses http://ip-api.com/json (HTTP, no API key required).
 *  Returns city, region, country, lat/lon, and an IANA timezone name
 *  (e.g. "America/Chicago").  tz_lookup converts IANA → POSIX TZ string.
 *  If tz_override_ is set it bypasses geolocation entirely.
 *
 * Network status
 * ──────────────
 *  get_status() returns a NetStatus struct with local IP, gateway, DNS,
 *  external IP (from geolocation response), RSSI, SSID, and geo fields.
 */

#include "clock_manager.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <cstdint>
#include <string>

// ── Geolocation / status struct ───────────────────────────────────────────────
struct NetStatus {
    // WiFi
    bool        wifi_connected  = false;
    int8_t      rssi            = 0;
    char        ssid[33]        = {};

    // IP layer
    char        local_ip[16]    = {};
    char        gateway[16]     = {};
    char        netmask[16]     = {};
    char        dns_primary[16] = {};

    // Geolocation (populated after first successful geo query)
    char        external_ip[46] = {};   // supports IPv6 too
    char        city[64]        = {};
    char        region[64]      = {};
    char        country[64]     = {};
    char        country_code[4] = {};
    float       latitude        = 0.0f;
    float       longitude       = 0.0f;
    char        isp[80]         = {};

    // Time
    char        iana_tz[64]     = {};   // e.g. "America/Chicago"
    char        posix_tz[80]    = {};   // e.g. "CST6CDT,M3.2.0,M11.1.0"
    bool        sntp_synced     = false;
};

// ── Networking class ──────────────────────────────────────────────────────────
class Networking {
public:
    explicit Networking(ClockManager& clock_mgr);
    ~Networking();

    // ── Configuration (call before begin()) ──────────────────────────────────
    void set_wifi_credentials(const char* ssid, const char* password);

    /**
     * @brief Skip geolocation and use this POSIX TZ string directly.
     *        e.g. "CST6CDT,M3.2.0,M11.1.0"
     */
    void set_timezone_override(const char* tz);

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void begin();

    // ── Status ────────────────────────────────────────────────────────────────
    const NetStatus& get_status() const { return status_; }
    bool is_connected()   const { return status_.wifi_connected; }
    bool is_time_synced() const { return status_.sntp_synced; }

private:
    // ── Event handlers (static trampoline → instance method) ─────────────────
    static void s_wifi_event_handler(void* arg, esp_event_base_t base,
                                     int32_t id, void* data);
    static void s_ip_event_handler(void* arg, esp_event_base_t base,
                                   int32_t id, void* data);
    static void s_sntp_sync_cb(struct timeval* tv);

    void on_wifi_connected();
    void on_wifi_disconnected();
    void on_got_ip(esp_netif_ip_info_t* ip_info);

    // ── Internal helpers ──────────────────────────────────────────────────────
    void start_sntp();
    void do_geolocation();          // runs in a short-lived task
    bool fetch_geolocation();       // HTTP GET ip-api.com, parse JSON
    void apply_timezone(const char* iana_tz);
    void refresh_rssi();
    void populate_dns();

    static void geo_task(void* arg); // FreeRTOS task wrapper for do_geolocation

    // ── State ─────────────────────────────────────────────────────────────────
    ClockManager&   clock_mgr_;
    esp_netif_t*    netif_         = nullptr;
    NetStatus       status_        = {};
    char            ssid_[33]      = {};
    char            password_[65]  = {};
    char            tz_override_[80] = {};
    int             retry_count_   = 0;

    static constexpr int MAX_RETRY = 10;
    static constexpr int GEO_HTTP_TIMEOUT_MS = 10000;

    bool begun_ = false;   // guards against double-call from Matter path

    // Module-static pointer so the SNTP callback (C linkage) can reach us
    static Networking* s_instance_;
};
