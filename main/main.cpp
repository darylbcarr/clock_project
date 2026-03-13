/**
 * @file main.cpp
 * @brief Application entry point — Analog clock driver with display + encoder menu
 *
 * Boot sequence
 * ─────────────
 *  1. Construct all objects (ClockManager, Networking, Display, Menu).
 *  2. Display: init I2C bus, show splash.
 *  3. Encoder: attach to Display's bus handle (shared I2C), init seesaw.
 *  4. Menu: wire dismiss function + build full tree.
 *  5. Networking: WiFi + SNTP chain (async).
 *  6. UART console: start shell task.
 *  7. ClockManager: sensor calibrate + start tick task.
 *  8. Encoder poll task: 50 Hz rotation/button → menu navigation + blank wake.
 *  9. Blank timer task: tick once per second, enforce 5-minute display timeout.
 *
 * I2C bus sharing
 * ───────────────
 *  Display and Encoder share one physical I2C bus (GPIO8/9).  The Display
 *  component owns the bus handle and exposes a FreeRTOS mutex via
 *  getBusMutex().  Every caller that touches the bus — encoder reads,
 *  display writes — must take this mutex for the duration of its transaction.
 *
 * Dismiss / info-screen blocking
 * ───────────────────────────────
 *  Menu callbacks that show info screens call wait_for_dismiss() which spins
 *  on dismiss_fn_.  Because those callbacks run ON the encoder_task stack
 *  (via select() → execute() → lambda), encoder_task is blocked while an
 *  info screen is showing.  dismiss_fn_ therefore polls the encoder hardware
 *  directly (with mutex) rather than depending on encoder_task to set a flag.
 *
 * Hardware
 * ────────
 *  Motor:    IN1=GPIO16, IN2=GPIO15, IN3=GPIO7,  IN4=GPIO6  (28BYJ-48 + ULN2003)
 *  LED:      GPIO13 (330Ω)   LDR: GPIO14 (10kΩ pull-down)
 *  I2C bus:  SDA=GPIO8  SCL=GPIO9  (shared by display + encoder)
 *  Display:  SSD1306 @ 0x3C  (new i2c_master driver)
 *  Encoder:  Seesaw SAMD09 @ 0x36  (new i2c_master driver, same bus handle)
 *
 * Encoder gestures
 * ────────────────
 *  Rotate CW       → menu next
 *  Rotate CCW      → menu previous
 *  Short press     → select / enter submenu
 *  Long press      → back to parent menu
 *  Any gesture     → wake display if blanked
 */

#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "clock_manager.h"
#include "networking.h"
#include "console_commands.h"
#include "display.h"
#include "encoder.h"
#include "menu.h"

static const char* TAG = "main";

// ── Configuration ─────────────────────────────────────────────────────────────

static constexpr const char* WIFI_SSID           = "Starstuff";
static constexpr const char* WIFI_PASSWORD       = "ItsyBitsy";
// static constexpr const char* TZ_OVERRIDE         = "CST6CDT,M3.2.0,M11.1.0";
static constexpr const char* TZ_OVERRIDE         = "";
static constexpr uint32_t    MOTOR_STEP_DELAY_US = 2000;

static constexpr gpio_num_t  I2C_SDA             = GPIO_NUM_8;
static constexpr gpio_num_t  I2C_SCL             = GPIO_NUM_9;
static constexpr uint8_t     SEESAW_ADDR         = 0x36;
static constexpr uint32_t    SEESAW_HZ           = 100'000;
static constexpr uint32_t    LONG_PRESS_MS       = 800;

// ── Shared system objects (static lifetime) ───────────────────────────────────

static ClockManager  s_clock(MOTOR_STEP_DELAY_US);
static Networking    s_net(s_clock);
static Display       s_display;
static SeesawDevice  s_seesaw;
static RotaryEncoder s_encoder(s_seesaw);
static Menu          s_menu(s_display);

// Set true only after seesaw.begin() AND encoder.init() both succeed.
// All encoder I2C callers check this before touching hardware.
static bool s_encoder_ok = false;

// ── dismiss_fn implementation ─────────────────────────────────────────────────
// Called by Menu::wait_for_dismiss() at 50ms intervals.
// Runs on the encoder_task stack — polls encoder hardware directly under
// the I2C mutex rather than relying on a flag encoder_task cannot set
// (it is blocked in the call chain that leads here).

static bool dismiss_fn()
{
    if (!s_encoder_ok) return false;
    static bool last_btn = false;

    SemaphoreHandle_t mtx = s_display.getBusMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool btn_now = s_encoder.button_pressed();
    xSemaphoreGive(mtx);

    // Rising edge = button released after a press → dismiss
    bool edge = (!btn_now && last_btn);
    last_btn = btn_now;
    return edge;
}

// ── Encoder poll task ─────────────────────────────────────────────────────────
// Runs at 50 Hz. Handles rotation, short press (select), long press (back).
// Calls s_menu.wake() on any activity so the blank timer resets.
//
// All encoder I2C reads are wrapped in the display bus mutex so they don't
// race with ssd1306 transactions happening on the same bus.

static void encoder_task(void* /*arg*/)
{
    bool     btn_last         = false;
    uint32_t btn_press_tick   = 0;
    bool     long_press_fired = false;
    uint32_t diag_tick        = 0;   // for rate-limited diagnostics

    SemaphoreHandle_t mtx = s_display.getBusMutex();

    while (true) {
        int8_t delta   = 0;
        bool   btn_now = false;

        // ── Read encoder under I2C mutex (only if hardware is ready) ──────────
        if (s_encoder_ok) {
            xSemaphoreTake(mtx, portMAX_DELAY);
            delta   = s_encoder.read_delta();
            btn_now = s_encoder.button_pressed();
            xSemaphoreGive(mtx);

            // Diagnostic: log raw values every 2s so we can see if reads work
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - diag_tick >= 2000) {
                diag_tick = now;
                ESP_LOGI("enc_diag", "delta=%d btn=%d", delta, (int)btn_now);
            }
        }

        // ── Rotation ──────────────────────────────────────────────────────────
        if (delta != 0) {
            s_menu.wake();
            if (!s_menu.is_blanked()) {
                if (delta > 0) s_menu.next();
                else           s_menu.previous();
                s_menu.render();
            }
        }

        // ── Button ────────────────────────────────────────────────────────────
        if (btn_now && !btn_last) {
            // Falling edge — button down
            btn_press_tick   = xTaskGetTickCount();
            long_press_fired = false;
            s_menu.wake();

        } else if (btn_now && btn_last && !long_press_fired) {
            // Held — check for long press threshold
            uint32_t held_ms = (xTaskGetTickCount() - btn_press_tick)
                               * portTICK_PERIOD_MS;
            if (held_ms >= LONG_PRESS_MS) {
                if (!s_menu.is_blanked()) {
                    s_menu.back();
                    s_menu.render();
                }
                long_press_fired = true;
            }

        } else if (!btn_now && btn_last && !long_press_fired) {
            // Rising edge — short press release → select
            // NOTE: if the selected item shows an info screen, select() will
            // block here (inside wait_for_dismiss) until button pressed again.
            // That is intentional — the encoder task IS the dismiss poller.
            if (!s_menu.is_blanked()) {
                s_menu.select();
                s_menu.render();
            }
        }

        btn_last = btn_now;
        vTaskDelay(pdMS_TO_TICKS(20));   // 50 Hz
    }
}

// ── Blank timer task ──────────────────────────────────────────────────────────

static void blank_timer_task(void* /*arg*/)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        s_menu.tick_blank_timer();
    }
}

// ── app_main ──────────────────────────────────────────────────────────────────

extern "C" void app_main()
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "  Analog Clock Driver  — ESP32-S3");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // ── 1. Display — inits I2C bus, exposes bus handle + mutex ───────────────
    ESP_LOGI(TAG, "Initialising display...");
    if (!s_display.init(I2C_NUM_0, 0x3C, (int)I2C_SDA, (int)I2C_SCL)) {
        ESP_LOGE(TAG, "Display init failed — halting");
        return;
    }
    s_display.clear();
    s_display.print(1, " Clock Driver");
    s_display.print(2, " Initializing");
    s_display.print(4, " Please wait...");

    // ── 2. Encoder — shares Display's bus handle (same physical I2C bus) ─────
    // After the display writes above the I2C bus->status may be non-idle.
    // Probe the display address (0x3C) — guaranteed ACK — to reset bus->status
    // to I2C_STATUS_DONE before adding a second device.  Probing SEESAW_ADDR
    // here is counterproductive: the SAMD09 may still be booting (no ACK),
    // which leaves bus->status in an unknown state rather than resetting it.
    ESP_LOGI(TAG, "Settling I2C bus before encoder registration...");
    esp_err_t probe_ret = s_display.probe_bus(0x3C);  // probe display (ACKs) → reset bus->status
    ESP_LOGI(TAG, "Bus probe (0x3C): %s", esp_err_to_name(probe_ret));
    vTaskDelay(pdMS_TO_TICKS(500));     // give SAMD09 time to complete boot

    ESP_LOGI(TAG, "Initialising encoder...");
    esp_err_t ret = s_seesaw.begin(s_display.getBusHandle(), SEESAW_ADDR, SEESAW_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Seesaw begin failed: %s — encoder disabled", esp_err_to_name(ret));
    } else {
        ret = s_encoder.init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Encoder init failed: %s — encoder disabled", esp_err_to_name(ret));
        } else {
            s_encoder_ok = true;
            ESP_LOGI(TAG, "Encoder ready");
        }
    }

    // ── 3. Menu — wire dismiss, then build full tree ──────────────────────────
    // dismiss_fn polls the encoder directly (with mutex) so it works even
    // while encoder_task is blocked inside wait_for_dismiss.
    s_menu.set_dismiss_fn(dismiss_fn);
    ESP_LOGI(TAG, "Building menu...");
    s_menu.build(s_clock, s_net);

    // ── 4. Networking — async WiFi + SNTP + geolocation ──────────────────────
    s_net.set_wifi_credentials(WIFI_SSID, WIFI_PASSWORD);
    if (TZ_OVERRIDE[0] != '\0') {
        s_net.set_timezone_override(TZ_OVERRIDE);
    }
    s_net.begin();

    // ── 5. UART console ───────────────────────────────────────────────────────
    console_start(&s_clock, &s_net,
                  s_encoder_ok ? &s_encoder : nullptr,
                  s_display.getBusMutex(),
                  s_display.getBusHandle());

    // ── 6. Sensor calibration + clock tick task ───────────────────────────────
    ESP_LOGI(TAG, "Calibrating sensor...");
    s_clock.cmd_calibrate_sensor();
    s_clock.start();

    // ── 7. Encoder poll task (priority 4) ─────────────────────────────────────
    xTaskCreate(encoder_task,     "encoder_poll", 3072, nullptr, 4, nullptr);

    // ── 8. Blank timer task (priority 2) ─────────────────────────────────────
    xTaskCreate(blank_timer_task, "blank_timer",  2048, nullptr, 2, nullptr);

    // ── 9. Splash → menu ─────────────────────────────────────────────────────
    s_display.clear();
    s_display.print(0, " Clock Driver");
    s_display.print(2, " Rotate: navigate");
    s_display.print(3, " Press:  select");
    s_display.print(4, " Hold:   back");
    vTaskDelay(pdMS_TO_TICKS(2500));

    s_menu.render();

    ESP_LOGI(TAG, "System running.");
    ESP_LOGI(TAG, "UART console at 115200 baud — type 'help'");
    ESP_LOGI(TAG, "Display blanks after 5 min inactivity.");
}
