/**
 * @file main.cpp
 * @brief Application entry point — Analog clock driver project
 *
 * Boot sequence
 * ─────────────
 *  1. Construct ClockManager and Networking.
 *  2. Set WiFi credentials and timezone override.
 *  3. Start networking (async WiFi + SNTP chain).
 *  4. Start UART command shell.
 *  5. Run initial sensor calibration.
 *  6. Start the ClockManager background tick task.
 *
 * The ClockManager advances the motor once per minute, checks the position
 * sensor near the top of each hour, and corrects drift automatically.
 */

#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "clock_manager.h"
#include "networking.h"
#include "console_commands.h"

static const char* TAG = "main";

// ─────────────────────────────────────────────────────────────────────────────
// Configuration — edit as needed
// ─────────────────────────────────────────────────────────────────────────────

// WiFi credentials (move to NVS / menuconfig in production)
static constexpr const char* WIFI_SSID     = "your_ssid_here";
static constexpr const char* WIFI_PASSWORD = "your_password_here";

// POSIX TZ string — set for your timezone.
// Leave as "" to use the UTC default until networking provides geolocation.
// Reference: https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
static constexpr const char* TZ_OVERRIDE = "CST6CDT,M3.2.0,M11.1.0";  // US Central

// Motor step delay (µs): 2000 = quiet/slow, 1200 = faster/louder
static constexpr uint32_t MOTOR_STEP_DELAY_US = 2000;

// ─────────────────────────────────────────────────────────────────────────────
// app_main
// ─────────────────────────────────────────────────────────────────────────────

extern "C" void app_main()
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "  Analog Clock Driver  — ESP32-S3");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // ── Construct core objects ────────────────────────────────────────────────
    static ClockManager clock_mgr(MOTOR_STEP_DELAY_US);
    static Networking   net(clock_mgr);

    // ── Configure networking ──────────────────────────────────────────────────
    net.set_wifi_credentials(WIFI_SSID, WIFI_PASSWORD);
    if (TZ_OVERRIDE[0] != '\0') {
        net.set_timezone_override(TZ_OVERRIDE);
    }

    // ── Start networking (async) ──────────────────────────────────────────────
    net.begin();

    // ── Start UART command shell ──────────────────────────────────────────────
    console_start(&clock_mgr);

    // ── Perform initial sensor calibration ───────────────────────────────────
    ESP_LOGI(TAG, "Running initial sensor calibration...");
    clock_mgr.cmd_calibrate_sensor();

    // ── Start the clock tick task ─────────────────────────────────────────────
    // Use the console to run 'set-time <minute>' before the tick task matters.
    clock_mgr.start();

    ESP_LOGI(TAG, "System running. Connect at 115200 baud and type 'help'.");
    ESP_LOGI(TAG, "Quick-start:");
    ESP_LOGI(TAG, "  1. calibrate        — baseline the sensor");
    ESP_LOGI(TAG, "  2. set-offset <s>   — seconds from trigger to top-of-hour");
    ESP_LOGI(TAG, "  3. set-time <min>   — align hand to current minute");
    ESP_LOGI(TAG, "  4. status           — verify state");

    // app_main returns; clock_mgr task and console task continue running.
}
