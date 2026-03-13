#pragma once

/**
 * @file networking.h
 * @brief WiFi + SNTP networking stub
 *
 * This component is a placeholder.  When implemented it will:
 *   1. Connect to WiFi using credentials stored in NVS.
 *   2. Determine the local timezone via an IP-geolocation API or NVS config.
 *   3. Call ClockManager::set_timezone() with the resolved TZ string.
 *   4. Start SNTP and call ClockManager::on_time_synced() once the first
 *      synchronisation completes.
 *   5. Maintain the connection and handle reconnect on drop.
 */

#include "clock_manager.h"
#include "esp_event.h"   // esp_event_base_t

class Networking {
public:
    /**
     * @brief Construct with a reference to the shared ClockManager.
     */
    explicit Networking(ClockManager& clock_mgr);

    /**
     * @brief Initialise NVS, netif, event loop, and begin WiFi connection.
     *        Non-blocking; connection result delivered via event callback.
     */
    void begin();

    /**
     * @brief Configure WiFi credentials.
     *        In the full implementation these are stored in NVS.
     */
    void set_wifi_credentials(const char* ssid, const char* password);

    /**
     * @brief Override timezone string (bypasses geolocation lookup).
     *        Must be a POSIX TZ string, e.g. "CST6CDT,M3.2.0,M11.1.0".
     */
    void set_timezone_override(const char* tz);

    bool is_connected() const { return connected_; }
    bool is_time_synced() const { return time_synced_; }

private:
    static void wifi_event_handler(void* arg, esp_event_base_t base,
                                   int32_t event_id, void* event_data);
    static void sntp_sync_callback(struct timeval* tv);

    ClockManager& clock_mgr_;
    bool connected_  = false;
    bool time_synced_ = false;
    char tz_override_[64] = {};
};
