/**
 * @file networking.cpp
 * @brief WiFi + SNTP networking stub implementation
 *
 * TODO: Implement WiFi connection, IP geolocation, and SNTP sync.
 *       The interface in networking.h is already wired to ClockManager.
 */

#include "networking.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
// In ESP-IDF v5.x the esp_sntp API is part of lwip; no separate component needed.
#include "esp_sntp.h"

static const char* TAG = "networking";

Networking::Networking(ClockManager& clock_mgr)
    : clock_mgr_(clock_mgr)
{
    tz_override_[0] = '\0';
}

void Networking::set_wifi_credentials(const char* ssid, const char* password)
{
    // TODO: store in NVS and configure wifi_config_t
    ESP_LOGI(TAG, "WiFi credentials set for SSID: %s (stub)", ssid);
}

void Networking::set_timezone_override(const char* tz)
{
    strncpy(tz_override_, tz, sizeof(tz_override_) - 1);
    ESP_LOGI(TAG, "Timezone override: %s", tz_override_);
}

void Networking::begin()
{
    ESP_LOGI(TAG, "Networking::begin()");

    // ── 1. NVS (must be first) ────────────────────────────────────────────────
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── 2. TCP/IP stack + default event loop ─────────────────────────────────
    // These MUST exist before any SNTP call.  esp_sntp_setoperatingmode()
    // internally calls tcpip_callback() which asserts that the lwIP mbox
    // has been created — that happens inside esp_netif_init().
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ── 3. SNTP configuration (safe now that the stack is up) ─────────────────
    // Ownership: Networking configures and starts SNTP; ClockManager only
    // receives the on_time_synced() callback.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    // sntp_set_time_sync_notification_cb(sntp_sync_callback);  // wire when ready

    // ── 4. Timezone ───────────────────────────────────────────────────────────
    if (tz_override_[0] != '\0') {
        clock_mgr_.set_timezone(tz_override_);
    }

    // ── 5. WiFi (stub — implement when networking component is built) ─────────
    // TODO:
    //   a. esp_netif_create_default_wifi_sta();
    //   b. wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&cfg);
    //   c. esp_event_handler_instance_register(WIFI_EVENT, ..., wifi_event_handler, this, ...);
    //   d. esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ..., ...);
    //   e. Configure wifi_config_t with ssid_/password_, esp_wifi_set_config / start / connect.
    //   f. In the IP_EVENT_STA_GOT_IP handler: esp_sntp_init(), then when SNTP fires
    //      sntp_sync_callback → clock_mgr_.on_time_synced().
    ESP_LOGW(TAG, "WiFi not yet implemented — operating without network time");
}

void Networking::wifi_event_handler(void* arg, esp_event_base_t base,
                                    int32_t event_id, void* event_data)
{
    // TODO: handle WIFI_EVENT_STA_CONNECTED, DISCONNECTED, IP_EVENT_STA_GOT_IP
    ESP_LOGI(TAG, "WiFi event base=%s id=%" PRId32 " (stub)", base, event_id);
}

void Networking::sntp_sync_callback(struct timeval* tv)
{
    // When wired up, retrieve the Networking* from a module-static pointer
    // and call clock_mgr_.on_time_synced().
    ESP_LOGI(TAG, "SNTP sync callback (stub)");
}
