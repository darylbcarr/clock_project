#pragma once

#include "clock_manager.h"
#include "networking.h"
#include "event_log.h"

class LedManager;
class OtaManager;
class MatterBridge;
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <functional>

class WebServer {
public:
    WebServer(ClockManager& clock_mgr, Networking& net, LedManager& leds);
    ~WebServer();

    void start();
    void stop();

    bool is_running() const { return server_ != nullptr; }

    /** @brief Wire in the OtaManager before start() to enable the /api/ota endpoint. */
    void set_ota(OtaManager* ota) { ota_ = ota; }

    /** @brief Wire in the MatterBridge before start() to expose Matter status in /api/status. */
    void set_matter(MatterBridge* matter) { matter_ = matter; }

private:
    // HTTP handlers (static, stored as context via req->user_ctx)
    static esp_err_t on_root(httpd_req_t* req);
    static esp_err_t on_api_status(httpd_req_t* req);
    static esp_err_t on_api_cmd(httpd_req_t* req);
    static esp_err_t on_api_cfg(httpd_req_t* req);
    static esp_err_t on_api_ota(httpd_req_t* req);
    static esp_err_t on_api_matter_ecw(httpd_req_t* req);
    static esp_err_t on_api_matter_ecw_cancel(httpd_req_t* req);
    static esp_err_t on_api_scan_results(httpd_req_t* req);
    static esp_err_t on_api_logs_get(httpd_req_t* req);
    static esp_err_t on_api_logs_post(httpd_req_t* req);
    static esp_err_t on_ws(httpd_req_t* req);

    // Background tasks
    static void ws_push_task(void* arg);
    static void cmd_exec_task(void* arg);

    // JSON helpers
    char* build_status_json();

    // Persistence helper — snapshots current LED state to NVS
    void save_led_config();

    // Command dispatcher (runs on cmd_exec_task)
    void dispatch_cmd(const char* cmd_name);

    ClockManager&  clock_mgr_;
    Networking&    net_;
    LedManager&    leds_;
    OtaManager*    ota_            = nullptr;
    MatterBridge*  matter_         = nullptr;
    httpd_handle_t server_          = nullptr;
    TaskHandle_t   ws_task_handle_  = nullptr;
    TaskHandle_t   cmd_task_handle_ = nullptr;
    QueueHandle_t  cmd_queue_          = nullptr;  // depth-1 queue of 32-char cmd names
    int            pending_obs_hour_ = -1;          // for set-time with known position
    int            pending_obs_min_  = -1;

    static WebServer* s_instance_;
};
