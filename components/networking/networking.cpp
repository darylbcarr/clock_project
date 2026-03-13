/**
 * @file networking.cpp
 * @brief WiFi STA, SNTP, IP geolocation, network status
 */

#include "networking.h"
#include "tz_lookup.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "lwip/dns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "networking";

// ── Module-static instance pointer for C-linkage callbacks ───────────────────
Networking* Networking::s_instance_ = nullptr;

// ── HTTP receive buffer ───────────────────────────────────────────────────────
static constexpr int HTTP_BUF_SIZE = 2048;

// ── Constructor / Destructor ──────────────────────────────────────────────────

Networking::Networking(ClockManager& clock_mgr)
    : clock_mgr_(clock_mgr)
{
    s_instance_ = this;
}

Networking::~Networking()
{
    esp_sntp_stop();
    s_instance_ = nullptr;
}

// ── Configuration ─────────────────────────────────────────────────────────────

void Networking::set_wifi_credentials(const char* ssid, const char* password)
{
    strncpy(ssid_,     ssid,     sizeof(ssid_)     - 1);
    strncpy(password_, password, sizeof(password_) - 1);
    ESP_LOGI(TAG, "WiFi credentials stored for SSID: %s", ssid_);
}

void Networking::set_timezone_override(const char* tz)
{
    strncpy(tz_override_, tz, sizeof(tz_override_) - 1);
    ESP_LOGI(TAG, "Timezone override: %s", tz_override_);
}

// ── begin() ───────────────────────────────────────────────────────────────────

void Networking::begin()
{
    ESP_LOGI(TAG, "Networking::begin()");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. TCP/IP stack + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Default STA netif
    netif_ = esp_netif_create_default_wifi_sta();

    // 4. WiFi driver
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    // 5. Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        s_wifi_event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        s_ip_event_handler, this, nullptr));

    // 6. Configure WiFi
    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid,     ssid_,     sizeof(cfg.sta.ssid)     - 1);
    strncpy((char*)cfg.sta.password, password_, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 7. SNTP mode (safe now; stack is up — just configure, don't init yet)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    sntp_set_time_sync_notification_cb(s_sntp_sync_cb);

    // 8. Apply timezone override immediately if set
    if (tz_override_[0] != '\0') {
        clock_mgr_.set_timezone(tz_override_);
        strncpy(status_.posix_tz, tz_override_, sizeof(status_.posix_tz) - 1);
    }

    ESP_LOGI(TAG, "WiFi started — connecting to '%s'...", ssid_);
}

// ── WiFi event handler ────────────────────────────────────────────────────────

void Networking::s_wifi_event_handler(void* arg, esp_event_base_t /*base*/,
                                      int32_t id, void* /*data*/)
{
    Networking* self = static_cast<Networking*>(arg);

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        self->retry_count_ = 0;
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strncpy(self->status_.ssid, (char*)ap.ssid,
                    sizeof(self->status_.ssid) - 1);
            self->status_.rssi = ap.rssi;
        }
        ESP_LOGI(TAG, "WiFi associated with '%s'  RSSI=%d dBm",
                 self->status_.ssid, self->status_.rssi);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        self->on_wifi_disconnected();
    }
}

// ── IP event handler ──────────────────────────────────────────────────────────

void Networking::s_ip_event_handler(void* arg, esp_event_base_t /*base*/,
                                    int32_t id, void* data)
{
    Networking* self = static_cast<Networking*>(arg);
    if (id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        self->on_got_ip(&event->ip_info);
    }
}

// ── SNTP sync callback (C linkage) ────────────────────────────────────────────

void Networking::s_sntp_sync_cb(struct timeval* /*tv*/)
{
    if (s_instance_) {
        s_instance_->status_.sntp_synced = true;
        s_instance_->clock_mgr_.on_time_synced();
        ESP_LOGI(TAG, "SNTP synchronised");
    }
}

// ── Connection events ─────────────────────────────────────────────────────────

void Networking::on_got_ip(esp_netif_ip_info_t* ip_info)
{
    status_.wifi_connected = true;

    snprintf(status_.local_ip,  sizeof(status_.local_ip),
             IPSTR, IP2STR(&ip_info->ip));
    snprintf(status_.gateway,   sizeof(status_.gateway),
             IPSTR, IP2STR(&ip_info->gw));
    snprintf(status_.netmask,   sizeof(status_.netmask),
             IPSTR, IP2STR(&ip_info->netmask));

    populate_dns();

    ESP_LOGI(TAG, "Got IP  local=%s  gw=%s",
             status_.local_ip, status_.gateway);

    // Start SNTP now that we have IP
    start_sntp();

    // Launch geolocation in a separate task (HTTP blocks)
    xTaskCreate(geo_task, "geo", 6144, this, 4, nullptr);
}

void Networking::on_wifi_disconnected()
{
    status_.wifi_connected = false;
    status_.rssi           = 0;
    ESP_LOGW(TAG, "WiFi disconnected (retry %d/%d)",
             retry_count_, MAX_RETRY);

    if (retry_count_ < MAX_RETRY) {
        ++retry_count_;
        // Simple exponential back-off: 1s, 2s, 4s … capped at 30s
        uint32_t delay_ms = std::min(1000u << (retry_count_ - 1), 30000u);
        ESP_LOGI(TAG, "Reconnecting in %lu ms...", (unsigned long)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else {
        ESP_LOGE(TAG, "Max retries reached. WiFi giving up.");
    }
}

// ── SNTP ──────────────────────────────────────────────────────────────────────

void Networking::start_sntp()
{
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (servers: %s, %s)", NTP_SERVER_1, NTP_SERVER_2);
}

// ── Geolocation ───────────────────────────────────────────────────────────────

void Networking::geo_task(void* arg)
{
    static_cast<Networking*>(arg)->do_geolocation();
    vTaskDelete(nullptr);
}

void Networking::do_geolocation()
{
    // If user supplied a manual override, skip geolocation but still log
    if (tz_override_[0] != '\0') {
        ESP_LOGI(TAG, "Timezone override active (%s) — skipping geolocation",
                 tz_override_);
        return;
    }
    if (!fetch_geolocation()) {
        ESP_LOGW(TAG, "Geolocation failed — timezone may be incorrect");
    }
}

bool Networking::fetch_geolocation()
{
    // ip-api.com returns JSON with all fields we need, free, no key.
    // Fields: status,city,regionName,country,countryCode,
    //         lat,lon,isp,query,timezone
    static const char* GEO_URL =
        "http://ip-api.com/json"
        "?fields=status,city,regionName,country,countryCode,"
        "lat,lon,isp,query,timezone";

    ESP_LOGI(TAG, "Geolocation: querying %s", GEO_URL);

    // Heap-allocate receive buffer
    char* buf = static_cast<char*>(malloc(HTTP_BUF_SIZE));
    if (!buf) {
        ESP_LOGE(TAG, "Geolocation: out of memory");
        return false;
    }
    int buf_pos = 0;
    memset(buf, 0, HTTP_BUF_SIZE);

    esp_http_client_config_t cfg = {};
    cfg.url             = GEO_URL;
    cfg.timeout_ms      = GEO_HTTP_TIMEOUT_MS;
    cfg.method          = HTTP_METHOD_GET;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Geolocation: http client init failed");
        free(buf);
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Geolocation: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        ESP_LOGW(TAG, "Geolocation: unknown content length, reading anyway");
    }

    int read_len = esp_http_client_read(client,
                                        buf + buf_pos,
                                        HTTP_BUF_SIZE - buf_pos - 1);
    if (read_len > 0) buf_pos += read_len;
    buf[buf_pos] = '\0';

    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (http_status != 200) {
        ESP_LOGE(TAG, "Geolocation: HTTP %d", http_status);
        free(buf);
        return false;
    }

    ESP_LOGD(TAG, "Geolocation response: %s", buf);

    // ── Parse JSON ────────────────────────────────────────────────────────────
    cJSON* root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGE(TAG, "Geolocation: JSON parse failed");
        return false;
    }

    auto str = [&](const char* key, char* dest, size_t dsize) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(item) && item->valuestring) {
            strncpy(dest, item->valuestring, dsize - 1);
        }
    };

    cJSON* status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsString(status_item) ||
        strcmp(status_item->valuestring, "success") != 0) {
        ESP_LOGE(TAG, "Geolocation: API returned non-success status");
        cJSON_Delete(root);
        return false;
    }

    str("query",       status_.external_ip,  sizeof(status_.external_ip));
    str("city",        status_.city,          sizeof(status_.city));
    str("regionName",  status_.region,        sizeof(status_.region));
    str("country",     status_.country,       sizeof(status_.country));
    str("countryCode", status_.country_code,  sizeof(status_.country_code));
    str("isp",         status_.isp,           sizeof(status_.isp));
    str("timezone",    status_.iana_tz,       sizeof(status_.iana_tz));

    cJSON* lat = cJSON_GetObjectItemCaseSensitive(root, "lat");
    cJSON* lon = cJSON_GetObjectItemCaseSensitive(root, "lon");
    if (cJSON_IsNumber(lat)) status_.latitude  = (float)lat->valuedouble;
    if (cJSON_IsNumber(lon)) status_.longitude = (float)lon->valuedouble;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Geolocation: %s, %s, %s (%s)  lat=%.2f lon=%.2f",
             status_.city, status_.region, status_.country,
             status_.country_code, status_.latitude, status_.longitude);
    ESP_LOGI(TAG, "Geolocation: external IP=%s  ISP=%s",
             status_.external_ip, status_.isp);
    ESP_LOGI(TAG, "Geolocation: IANA timezone=%s", status_.iana_tz);

    // ── Resolve IANA → POSIX TZ ───────────────────────────────────────────────
    apply_timezone(status_.iana_tz);
    return true;
}

void Networking::apply_timezone(const char* iana_tz)
{
    const char* posix = tz_lookup(iana_tz);
    if (posix && posix[0] != '\0') {
        strncpy(status_.posix_tz, posix, sizeof(status_.posix_tz) - 1);
        ESP_LOGI(TAG, "Timezone resolved: %s → %s", iana_tz, posix);
        clock_mgr_.set_timezone(posix);
    } else {
        ESP_LOGW(TAG, "Timezone lookup failed for '%s' — using UTC", iana_tz);
        clock_mgr_.set_timezone("UTC0");
        strncpy(status_.posix_tz, "UTC0", sizeof(status_.posix_tz) - 1);
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void Networking::populate_dns()
{
    const ip_addr_t* dns = dns_getserver(0);
    if (dns && !ip_addr_isany(dns)) {
        snprintf(status_.dns_primary, sizeof(status_.dns_primary),
                 IPSTR, IP2STR(&dns->u_addr.ip4));
    }
}

void Networking::refresh_rssi()
{
    if (!status_.wifi_connected) return;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        status_.rssi = ap.rssi;
    }
}
