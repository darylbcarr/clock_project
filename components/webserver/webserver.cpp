#include "webserver.h"
#include "web_ui.h"
#include "led_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <cstring>
#include <cstdlib>

static const char* TAG = "webserver";

WebServer* WebServer::s_instance_ = nullptr;

// ── Constructor / Destructor ──────────────────────────────────────────────────

WebServer::WebServer(ClockManager& clock_mgr, Networking& net, LedManager& leds)
    : clock_mgr_(clock_mgr), net_(net), leds_(leds)
{
    s_instance_ = this;
    // Depth-1 queue; each item is a fixed 32-byte command name (copied by value)
    cmd_queue_ = xQueueCreate(1, 32);
}

WebServer::~WebServer()
{
    stop();
    if (cmd_queue_) { vQueueDelete(cmd_queue_); cmd_queue_ = nullptr; }
}

// ── start() / stop() ─────────────────────────────────────────────────────────

void WebServer::start()
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // Register URI handlers — store 'this' as user_ctx
    httpd_uri_t root = { "/",           HTTP_GET,  on_root,       this };
    httpd_uri_t stat = { "/api/status", HTTP_GET,  on_api_status, this };
    httpd_uri_t cmd  = { "/api/cmd",    HTTP_POST, on_api_cmd,    this };
    httpd_uri_t ws   = {
        .uri      = "/ws",
        .method   = HTTP_GET,
        .handler  = on_ws,
        .user_ctx = this,
        .is_websocket = true
    };

    httpd_register_uri_handler(server_, &root);
    httpd_register_uri_handler(server_, &stat);
    httpd_register_uri_handler(server_, &cmd);
    httpd_register_uri_handler(server_, &ws);

    xTaskCreate(ws_push_task,  "ws_push",  4096, this, 2, &ws_task_handle_);
    xTaskCreate(cmd_exec_task, "web_cmd",  4096, this, 3, &cmd_task_handle_);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

void WebServer::stop()
{
    if (ws_task_handle_)  { vTaskDelete(ws_task_handle_);  ws_task_handle_  = nullptr; }
    if (cmd_task_handle_) { vTaskDelete(cmd_task_handle_); cmd_task_handle_ = nullptr; }
    if (server_)          { httpd_stop(server_);           server_          = nullptr; }
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────

esp_err_t WebServer::on_root(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, WEB_UI_HTML, WEB_UI_HTML_LEN);
}

esp_err_t WebServer::on_api_status(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);
    char* json = self->build_status_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

esp_err_t WebServer::on_api_cmd(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);

    char buf[128] = {};
    int  len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON* body = cJSON_Parse(buf);
    const char* cmd_name = nullptr;
    if (body) {
        cJSON* c = cJSON_GetObjectItem(body, "cmd");
        if (cJSON_IsString(c)) cmd_name = c->valuestring;
    }

    if (!cmd_name) {
        if (body) cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'cmd'");
        return ESP_FAIL;
    }

    // Colour command is handled inline (not queued) since it's fast
    if (strcmp(cmd_name, "led-color") == 0) {
        cJSON* rj = cJSON_GetObjectItem(body, "r");
        cJSON* gj = cJSON_GetObjectItem(body, "g");
        cJSON* bj = cJSON_GetObjectItem(body, "b");
        if (cJSON_IsNumber(rj) && cJSON_IsNumber(gj) && cJSON_IsNumber(bj)) {
            self->leds_.set_color(LedManager::Target::BOTH,
                (uint8_t)rj->valueint,
                (uint8_t)gj->valueint,
                (uint8_t)bj->valueint);
        }
        cJSON_Delete(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Colour set\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    // Copy name to a fixed-size buffer (queue items are 32 bytes, value-copied)
    char name[32] = {};
    strncpy(name, cmd_name, sizeof(name) - 1);
    cJSON_Delete(body);

    // Non-blocking send — if queue is full (command already pending) report busy
    bool queued = (xQueueSend(self->cmd_queue_, name, 0) == pdTRUE);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char* resp = queued
        ? "{\"ok\":true,\"msg\":\"Command accepted\"}"
        : "{\"ok\":false,\"msg\":\"Busy \xe2\x80\x94 try again\"}";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::on_ws(httpd_req_t* req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected, fd=%d",
                 httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    // Drain any incoming client frames (push-only server)
    httpd_ws_frame_t pkt = {};
    uint8_t buf[64]      = {};
    pkt.payload          = buf;
    httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    return ESP_OK;
}

// ── WebSocket broadcast ───────────────────────────────────────────────────────

struct WsBcastArg {
    httpd_handle_t hd;
    char*          json;
    size_t         len;
};

static void do_ws_broadcast(void* arg)
{
    auto* a = static_cast<WsBcastArg*>(arg);
    size_t clients = 8;
    int    fds[8]  = {};

    if (httpd_get_client_list(a->hd, &clients, fds) == ESP_OK) {
        httpd_ws_frame_t pkt = {};
        pkt.type             = HTTPD_WS_TYPE_TEXT;
        pkt.payload          = reinterpret_cast<uint8_t*>(a->json);
        pkt.len              = a->len;
        for (size_t i = 0; i < clients; i++) {
            if (httpd_ws_get_fd_info(a->hd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(a->hd, fds[i], &pkt);
            }
        }
    }

    free(a->json);
    free(a);
}

void WebServer::ws_push_task(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!self->server_) continue;

        char* json = self->build_status_json();
        if (!json) continue;

        auto* a = static_cast<WsBcastArg*>(malloc(sizeof(WsBcastArg)));
        if (!a) { free(json); continue; }

        a->hd   = self->server_;
        a->json = json;
        a->len  = strlen(json);

        if (httpd_queue_work(self->server_, do_ws_broadcast, a) != ESP_OK) {
            free(json);
            free(a);
        }
    }
}

// ── Command executor ──────────────────────────────────────────────────────────

void WebServer::cmd_exec_task(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);
    char  name[32];
    while (true) {
        if (xQueueReceive(self->cmd_queue_, name, portMAX_DELAY) == pdTRUE) {
            self->dispatch_cmd(name);
        }
    }
}

void WebServer::dispatch_cmd(const char* cmd)
{
    ESP_LOGI(TAG, "Executing command: %s", cmd);

    if      (strcmp(cmd, "set-time")       == 0) clock_mgr_.cmd_set_time(-1);
    else if (strcmp(cmd, "advance")        == 0) clock_mgr_.cmd_test_advance();
    else if (strcmp(cmd, "step-fwd")       == 0) clock_mgr_.cmd_microstep(8, true);
    else if (strcmp(cmd, "step-bwd")       == 0) clock_mgr_.cmd_microstep(8, false);
    else if (strcmp(cmd, "calibrate")      == 0) clock_mgr_.cmd_calibrate_sensor();
    else if (strcmp(cmd, "measure")        == 0) clock_mgr_.cmd_measure_sensor_average();
    // LED commands
    else if (strcmp(cmd, "led-off")        == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::OFF);
    else if (strcmp(cmd, "led-static")     == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::STATIC);
    else if (strcmp(cmd, "led-breathe")    == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::BREATHE);
    else if (strcmp(cmd, "led-rainbow")    == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::RAINBOW);
    else if (strcmp(cmd, "led-chase")      == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::CHASE);
    else if (strcmp(cmd, "led-sparkle")    == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::SPARKLE);
    else if (strcmp(cmd, "led-wipe")       == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::WIPE);
    else if (strcmp(cmd, "led-comet")      == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::COMET);
    else if (strcmp(cmd, "led-next")       == 0) leds_.next_effect(LedManager::Target::BOTH);
    else if (strcmp(cmd, "led-bright-up")  == 0) {
        uint8_t b = leds_.get_brightness(0);
        leds_.set_brightness(LedManager::Target::BOTH, b > 230 ? 255 : b + 25);
    }
    else if (strcmp(cmd, "led-bright-down") == 0) {
        uint8_t b = leds_.get_brightness(0);
        leds_.set_brightness(LedManager::Target::BOTH, b < 25 ? 0 : b - 25);
    }
    else ESP_LOGW(TAG, "Unknown command: %s", cmd);
}

// ── JSON status builder ───────────────────────────────────────────────────────

char* WebServer::build_status_json()
{
    const NetStatus& net = net_.get_status();
    uint32_t uptime_s  = static_cast<uint32_t>(esp_timer_get_time() / 1'000'000ULL);
    uint32_t free_heap = esp_get_free_heap_size();

    cJSON* root = cJSON_CreateObject();
    if (!root) return nullptr;

    // Time
    cJSON_AddStringToObject(root, "time",         clock_mgr_.time_hms().c_str());
    cJSON_AddStringToObject(root, "date",         clock_mgr_.date_long().c_str());
    cJSON_AddNumberToObject(root, "displayed_min",clock_mgr_.displayed_minute());
    cJSON_AddBoolToObject  (root, "sntp",         clock_mgr_.is_time_valid());
    cJSON_AddStringToObject(root, "iana_tz",      net.iana_tz[0] ? net.iana_tz : "");

    // Clock details
    cJSON_AddNumberToObject(root, "sensor_offset_sec", clock_mgr_.sensor_offset_sec());
    cJSON_AddBoolToObject  (root, "motor_powered",     false);   // getter to be added
    cJSON_AddNumberToObject(root, "step_delay_us",     2000);    // from firmware constant

    // Network / WiFi
    cJSON_AddBoolToObject  (root, "wifi",        net.wifi_connected);
    cJSON_AddStringToObject(root, "ssid",        net.ssid);
    cJSON_AddNumberToObject(root, "rssi",        static_cast<double>(net.rssi));
    cJSON_AddStringToObject(root, "local_ip",    net.local_ip);
    cJSON_AddStringToObject(root, "gateway",     net.gateway);

    // Geolocation
    cJSON_AddStringToObject(root, "external_ip", net.external_ip);
    cJSON_AddStringToObject(root, "city",        net.city);
    cJSON_AddStringToObject(root, "region",      net.region);
    cJSON_AddStringToObject(root, "isp",         net.isp);

    // System
    cJSON_AddNumberToObject(root, "uptime_s",  static_cast<double>(uptime_s));
    cJSON_AddNumberToObject(root, "free_heap", static_cast<double>(free_heap));

    // LED strips
    cJSON* leds_obj = cJSON_AddObjectToObject(root, "leds");
    if (leds_obj) {
        const char* strip_keys[LedManager::STRIP_COUNT] = { "strip1", "strip2" };
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            cJSON* strip = cJSON_AddObjectToObject(leds_obj, strip_keys[i]);
            if (strip) {
                uint8_t r, g, b;
                leds_.get_color(i, r, g, b);
                cJSON_AddStringToObject(strip, "effect",
                    LedManager::effect_name(leds_.get_effect(i)));
                cJSON_AddNumberToObject(strip, "brightness",
                    leds_.get_brightness(i));
                cJSON_AddNumberToObject(strip, "r", r);
                cJSON_AddNumberToObject(strip, "g", g);
                cJSON_AddNumberToObject(strip, "b", b);
            }
        }
    }

    char* result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result;  // caller must free()
}
