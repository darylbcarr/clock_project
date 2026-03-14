#pragma once

#include "clock_manager.h"
#include "networking.h"

class LedManager;
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

private:
    // HTTP handlers (static, stored as context via req->user_ctx)
    static esp_err_t on_root(httpd_req_t* req);
    static esp_err_t on_api_status(httpd_req_t* req);
    static esp_err_t on_api_cmd(httpd_req_t* req);
    static esp_err_t on_api_cfg(httpd_req_t* req);
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
    httpd_handle_t server_          = nullptr;
    TaskHandle_t   ws_task_handle_  = nullptr;
    TaskHandle_t   cmd_task_handle_ = nullptr;
    QueueHandle_t  cmd_queue_          = nullptr;  // depth-1 queue of 32-char cmd names
    int            pending_observed_min_ = -1;     // for set-time with known position

    static WebServer* s_instance_;
};
