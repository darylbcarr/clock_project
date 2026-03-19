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
#include "mdns.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "lwip/dns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_coexist.h"

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

void Networking::set_mdns_hostname_hint(const char* name)
{
    if (name) strncpy(mdns_hostname_hint_, name, sizeof(mdns_hostname_hint_) - 1);
}

// ── begin() ───────────────────────────────────────────────────────────────────

void Networking::begin()
{
    if (begun_) {
        ESP_LOGI(TAG, "Networking::begin() already called — skipping");
        return;
    }
    begun_ = true;

    ESP_LOGI(TAG, "Networking::begin()");

    // NVS is initialised by main.cpp before begin() is called.

    // 1. TCP/IP stack
    // esp_netif_init() is idempotent in ESP-IDF 5.x.
    ESP_ERROR_CHECK(esp_netif_init());

    // 2. Default event loop
    // ESP_ERR_INVALID_STATE = already created by esp_matter::start() — use it.
    {
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    }

    // 3. Default STA netif
    // esp_matter::start() creates "WIFI_STA_DEF" before we reach here; reuse it.
    netif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif_) {
        netif_ = esp_netif_create_default_wifi_sta();
    }

    // 4. WiFi driver
    // Skip silently if Matter already called esp_wifi_init().
    {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t e = esp_wifi_init(&wifi_cfg);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_init: %s (already initialised by Matter)",
                     esp_err_to_name(e));
        }
    }

    // 4b. Pre-populate mDNS hostname in status so /api/status and the WebSocket
    //     always return the real name, even before WiFi connects and mdns_task runs.
    //     Works on both WiFi and Matter paths — MAC is readable after esp_wifi_init.
    {
        char hostname[32] = {};
        if (mdns_hostname_hint_[0] != '\0') {
            strncpy(hostname, mdns_hostname_hint_, sizeof(hostname) - 1);
        } else {
            uint8_t mac[6] = {};
            if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
                snprintf(hostname, sizeof(hostname), "clock_%02x%02x", mac[4], mac[5]);
            }
        }
        if (hostname[0] != '\0') {
            strncpy(status_.mdns_hostname, hostname, sizeof(status_.mdns_hostname) - 1);
            ESP_LOGI(TAG, "mDNS hostname (pre-start): %s", hostname);
        }
    }

    // 5. Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        s_wifi_event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        s_ip_event_handler, this, nullptr));

    // 6. Configure SNTP before any path that might call start_sntp().
    //    Guard against double-init: Matter or a prior begin() may have already
    //    started SNTP (esp_sntp_setoperatingmode asserts if called while running).
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, NTP_SERVER_1);
        esp_sntp_setservername(1, NTP_SERVER_2);
        sntp_set_time_sync_notification_cb(s_sntp_sync_cb);
    }

    // 7. Connect WiFi or bootstrap from Matter's existing connection.
    //    If SSID is empty, Matter manages WiFi; we skip connect but still
    //    watch for the IP event so SNTP can start when Matter gets an address.
    if (ssid_[0] != '\0') {
        wifi_config_t cfg = {};
        strncpy((char*)cfg.sta.ssid,     ssid_,     sizeof(cfg.sta.ssid)     - 1);
        strncpy((char*)cfg.sta.password, password_, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.sta.pmf_cfg.capable    = true;
        cfg.sta.pmf_cfg.required   = false;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

        // If Matter already started WiFi, WIFI_EVENT_STA_START won't fire
        // again — call connect() directly in that case.
        esp_err_t e = esp_wifi_start();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_start: %s (already started by Matter) — connecting directly",
                     esp_err_to_name(e));
            esp_wifi_connect();
        }
        // On ESP_OK, WIFI_EVENT_STA_START fires → s_wifi_event_handler → connect()
    } else {
        ESP_LOGI(TAG, "No SSID configured — Matter manages WiFi");
        // Give WiFi higher coex priority so it can complete the 4-way
        // association handshake without timing out due to BLE occupying the
        // radio during BLE commissioning.  BLE still works with lower priority
        // (its indications are retried automatically), just at higher latency.
        // Restored to ESP_COEX_PREFER_BALANCE once WiFi has an IP.
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        ESP_LOGI(TAG, "Coex preference → WIFI (Matter BLE commissioning path)");

        // Ensure WiFi STA is in the right mode and started so CHIP's
        // NetworkCommissioning driver can use it during BLE commissioning.
        // esp_matter::start() should have done this, but being explicit here
        // avoids a race where our begin() is called before CHIP's async WiFi
        // init task has completed.
        {
            esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
            if (e != ESP_OK)
                ESP_LOGW(TAG, "WiFi set_mode STA (Matter): %s", esp_err_to_name(e));
            e = esp_wifi_start();
            if (e != ESP_OK)
                ESP_LOGW(TAG, "WiFi start (Matter): %s (may already be started)",
                         esp_err_to_name(e));
        }
        // If Matter already obtained an IP before we registered our event handler,
        // bootstrap SNTP now instead of waiting for an event that already fired.
        esp_netif_ip_info_t ip_info = {};
        if (netif_ && esp_netif_get_ip_info(netif_, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "Matter already has IP — bootstrapping SNTP");
            on_got_ip(&ip_info);
        }
    }

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
        if (self->ssid_[0] != '\0') {
            esp_wifi_connect();
        }
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

    // WiFi connected — restore balanced coex so BLE/Matter-over-IP coexist fairly
    if (ssid_[0] == '\0') {
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        ESP_LOGI(TAG, "Coex preference → BALANCE (WiFi connected)");
    }

    // Start SNTP now that we have IP
    start_sntp();

    // Launch geolocation and mDNS in separate tasks — both must not run
    // inside the event handler callback (mdns_init registers its own event
    // handlers and blocks briefly; doing so here can deadlock the event loop).
    xTaskCreate(geo_task,  "geo",      6144, this, 4, nullptr);
    xTaskCreate(mdns_task, "net_mdns", 4096, this, 3, nullptr);
}

void Networking::on_wifi_disconnected()
{
    status_.wifi_connected = false;
    status_.rssi           = 0;

    // When SSID is empty, Matter's NetworkCommissioning driver manages WiFi.
    // Let CHIP handle its own reconnect cycle (it retries after
    // CONFIG_WIFI_STATION_RECONNECT_INTERVAL = 1000 ms, which is long enough
    // for the coex module's internal reconnect lock to clear naturally).
    // Do NOT call esp_wifi_connect() here and do NOT restart the driver:
    // stop/start clears the WiFi AP scan cache, forcing a fresh channel scan
    // on every connect attempt — that scan takes 1–3 s and consumes the
    // post-AddNOC BLE quiet windows before authentication can start.
    if (ssid_[0] == '\0') {
        ESP_LOGD(TAG, "WiFi disconnected — Matter manages reconnect");
        return;
    }

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

// ── mDNS ──────────────────────────────────────────────────────────────────────

void Networking::start_mdns()
{
    // On the Matter WiFi path (ssid_ is empty), only start mDNS when the device
    // is already commissioned.  During first-time BLE commissioning, CHIP's minimal
    // mDNS and our mdns_init() both bind to 224.0.0.251:5353, exhausting CHIP's
    // packet buffers and killing the BLE handshake (PacketBuffer: pool EMPTY).
    // After commissioning, BLE is inactive on reboot so both stacks coexist safely.
    if (ssid_[0] == '\0' && !matter_commissioned_) {
        ESP_LOGI(TAG, "mDNS: skipped (Matter first-time commissioning — BLE active)");
        return;
    }

    // Derive hostname: saved hint → fall back to "clock_XXXX" from last 2 MAC bytes.
    char hostname[32] = {};
    if (mdns_hostname_hint_[0] != '\0') {
        strncpy(hostname, mdns_hostname_hint_, sizeof(hostname) - 1);
    } else {
        uint8_t mac[6] = {};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(hostname, sizeof(hostname), "clock_%02x%02x", mac[4], mac[5]);
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    // mdns_init() is called after IP_EVENT_STA_GOT_IP already fired, so the
    // mDNS task's PCB is still disabled (it missed the IP event).  Enable it
    // explicitly BEFORE setting the hostname so that mdns_hostname_set()
    // triggers the probe+announce cycle on the now-active STA interface.
    if (netif_) {
        mdns_netif_action(netif_, MDNS_EVENT_ENABLE_IP4);
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set(hostname);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);

    strncpy(status_.mdns_hostname, hostname, sizeof(status_.mdns_hostname) - 1);
    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
}

void Networking::set_mdns_hostname(const char* name)
{
    if (!name || name[0] == '\0') return;
    strncpy(mdns_hostname_hint_, name, sizeof(mdns_hostname_hint_) - 1);  // survive reconnect
    mdns_hostname_set(name);
    strncpy(status_.mdns_hostname, name, sizeof(status_.mdns_hostname) - 1);
    ESP_LOGI(TAG, "mDNS hostname updated: %s.local", name);
}

// ── SNTP ──────────────────────────────────────────────────────────────────────

void Networking::start_sntp()
{
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already running — skipping init");
        return;
    }
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (servers: %s, %s)", NTP_SERVER_1, NTP_SERVER_2);
}

// ── Geolocation ───────────────────────────────────────────────────────────────

void Networking::geo_task(void* arg)
{
    static_cast<Networking*>(arg)->do_geolocation();
    vTaskDelete(nullptr);
}

void Networking::mdns_task(void* arg)
{
    static_cast<Networking*>(arg)->start_mdns();
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
