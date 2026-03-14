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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#include "clock_manager.h"
#include "networking.h"
#include "console_commands.h"
#include "display.h"
#include "encoder.h"
#include "menu.h"
#include "webserver.h"
#include "led_manager.h"
#include "config_store.h"

static const char* TAG = "main";

// ── Configuration defaults (used when NVS has no saved value) ─────────────────

static constexpr const char* WIFI_SSID_DEFAULT     = "Starstuff";
static constexpr const char* WIFI_PASSWORD_DEFAULT = "ItsyBitsy";
// static constexpr const char* TZ_OVERRIDE_DEFAULT = "CST6CDT,M3.2.0,M11.1.0";
static constexpr const char* TZ_OVERRIDE_DEFAULT   = "";

static constexpr gpio_num_t  I2C_SDA             = GPIO_NUM_8;
static constexpr gpio_num_t  I2C_SCL             = GPIO_NUM_9;
static constexpr uint8_t     SEESAW_ADDR         = 0x36;
static constexpr uint32_t    SEESAW_HZ           = 100'000;
static constexpr uint32_t    LONG_PRESS_MS       = 800;
static constexpr gpio_num_t  BUTTON_A            = GPIO_NUM_10;  // menu previous
static constexpr gpio_num_t  BUTTON_B            = GPIO_NUM_11;  // menu next

// ── Shared system objects (static lifetime) ───────────────────────────────────

static ClockManager  s_clock(2000);  // default step delay; overridden from NVS below
static Networking    s_net(s_clock);
static Display       s_display;
static SeesawDevice  s_seesaw;
static RotaryEncoder s_encoder(s_seesaw);
static Menu          s_menu(s_display);
static LedManager    s_leds(GPIO_NUM_1, GPIO_NUM_2, 30);
static WebServer     s_webserver(s_clock, s_net, s_leds);

// Set true only after seesaw.begin() AND encoder.init() both succeed.
// All encoder I2C callers check this before touching hardware.
static bool s_encoder_ok = false;

// ── dismiss_fn implementation ─────────────────────────────────────────────────
// Called by Menu::wait_for_dismiss() at 50ms intervals.
// Runs on the encoder_task stack — polls encoder hardware directly under
// the I2C mutex rather than relying on a flag encoder_task cannot set
// (it is blocked in the call chain that leads here).

// Returns true while a dismiss gesture is held.
// wait_for_dismiss() in Menu accumulates hold duration and fires after 800ms.
// Two sources: encoder button (I2C) OR both hardware buttons pressed together.
static bool dismiss_fn()
{
    // Both GPIO buttons held (no I2C needed)
    if ((gpio_get_level(BUTTON_A) == 0) && (gpio_get_level(BUTTON_B) == 0))
        return true;
    // Encoder button
    if (!s_encoder_ok) return false;
    SemaphoreHandle_t mtx = s_display.getBusMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool btn = s_encoder.button_pressed();
    xSemaphoreGive(mtx);
    return btn;
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
    bool     btnA_last        = false;
    bool     btnB_last        = false;
    uint32_t btnA_press_tick  = 0;
    uint32_t btnB_press_tick  = 0;
    bool     btnA_long_fired  = false;
    bool     btnB_long_fired  = false;
    int      h_scroll_counter = 0;

    SemaphoreHandle_t mtx = s_display.getBusMutex();

    while (true) {
        int8_t delta   = 0;
        bool   btn_now = false;

        // ── Read encoder under I2C mutex ──────────────────────────────────────
        if (s_encoder_ok) {
            xSemaphoreTake(mtx, portMAX_DELAY);
            delta   = s_encoder.read_delta();
            btn_now = s_encoder.button_pressed();
            xSemaphoreGive(mtx);
        }

        // ── Read hardware buttons (active-low, pull-up) ───────────────────────
        bool btnA = (gpio_get_level(BUTTON_A) == 0);
        bool btnB = (gpio_get_level(BUTTON_B) == 0);

        bool btnA_fall = (btnA  && !btnA_last);   // press edge
        bool btnA_rise = (!btnA &&  btnA_last);   // release edge
        bool btnB_fall = (btnB  && !btnB_last);
        bool btnB_rise = (!btnB &&  btnB_last);
        bool both      = btnA && btnB;

        // ── Wake-up suppression ───────────────────────────────────────────────
        // Any input wakes the display; the triggering gesture is consumed.
        if ((delta != 0 || btn_now || btnA || btnB) && s_menu.is_blanked()) {
            s_menu.wake();
            btn_last         = btn_now;
            btnA_last        = btnA;
            btnB_last        = btnB;
            long_press_fired = true;   // suppress encoder press cycle until release
            btnA_long_fired  = true;   // suppress GPIO press cycle until release
            btnB_long_fired  = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // ── Rotation ─────────────────────────────────────────────────────────
        if (delta != 0) {
            s_menu.wake();
            if (delta > 0) { s_menu.next();     s_menu.render_scrolled(true);  }
            else            { s_menu.previous(); s_menu.render_scrolled(false); }
            h_scroll_counter = 0;
        }

        // ── Hardware buttons ──────────────────────────────────────────────────
        // Short press A → previous,  Short press B → next
        // Long press A or B (≥800ms) → select (simulates encoder button)
        // Both pressed simultaneously → reserved for dismiss_fn (info screens)
        if (both) {
            // Suppress individual actions while both are held so that releasing
            // to a single button doesn't trigger that button's short/long press.
            btnA_long_fired = true;
            btnB_long_fired = true;
        } else {
            // ── Falling edges (press start) ───────────────────────────────────
            if (btnA_fall) {
                btnA_press_tick = xTaskGetTickCount();
                btnA_long_fired = false;
                s_menu.wake();
            }
            if (btnB_fall) {
                btnB_press_tick = xTaskGetTickCount();
                btnB_long_fired = false;
                s_menu.wake();
            }
            // ── Long-press detection (while held) ─────────────────────────────
            if (btnA && !btnA_long_fired) {
                uint32_t held = (xTaskGetTickCount() - btnA_press_tick) * portTICK_PERIOD_MS;
                if (held >= LONG_PRESS_MS) {
                    btnA_long_fired = true;
                    if (!s_menu.is_blanked()) {
                        s_menu.select();
                        s_menu.render();
                        h_scroll_counter = 0;
                    }
                }
            }
            if (btnB && !btnB_long_fired) {
                uint32_t held = (xTaskGetTickCount() - btnB_press_tick) * portTICK_PERIOD_MS;
                if (held >= LONG_PRESS_MS) {
                    btnB_long_fired = true;
                    if (!s_menu.is_blanked()) {
                        s_menu.select();
                        s_menu.render();
                        h_scroll_counter = 0;
                    }
                }
            }
            // ── Rising edges (release) → short press if no long press fired ──
            if (btnA_rise && !btnA_long_fired) {
                s_menu.wake();
                s_menu.previous();
                s_menu.render_scrolled(false);
                h_scroll_counter = 0;
            }
            if (btnB_rise && !btnB_long_fired) {
                s_menu.wake();
                s_menu.next();
                s_menu.render_scrolled(true);
                h_scroll_counter = 0;
            }
        }

        btnA_last = btnA;
        btnB_last = btnB;

        // ── Encoder button ────────────────────────────────────────────────────
        if (btn_now && !btn_last) {
            // Falling edge — button down
            btn_press_tick   = xTaskGetTickCount();
            long_press_fired = false;
            s_menu.wake();

        } else if (btn_now && btn_last && !long_press_fired) {
            // Held — check for long press (back)
            uint32_t held_ms = (xTaskGetTickCount() - btn_press_tick)
                               * portTICK_PERIOD_MS;
            if (held_ms >= LONG_PRESS_MS) {
                if (!s_menu.is_blanked()) {
                    s_menu.back();
                    s_menu.render_scrolled(false);
                    h_scroll_counter = 0;
                }
                long_press_fired = true;
            }

        } else if (!btn_now && btn_last && !long_press_fired) {
            // Rising edge — short press → select
            // If selected item shows an info screen, select() blocks here
            // (inside wait_for_dismiss) — encoder_task IS the dismiss poller.
            if (!s_menu.is_blanked()) {
                s_menu.select();
                s_menu.render();
                h_scroll_counter = 0;
            }
        }

        btn_last = btn_now;

        // ── Horizontal scroll tick (every ~300 ms) ────────────────────────────
        if (++h_scroll_counter >= 15) {
            h_scroll_counter = 0;
            s_menu.tick_h_scroll();
        }

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

    // ── 0. NVS flash init (must be first; Networking::begin() reuses it) ─────
    {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS partition issue — erasing and reinitialising");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        ConfigStore::init();
    }

    // ── 0a. Load persisted configuration ────────────────────────────────────
    ClockCfg  clockCfg;
    LedCfg    ledCfg;
    NetCfg    netCfg;
    ConfigStore::load(clockCfg);
    ConfigStore::load(ledCfg);
    ConfigStore::load(netCfg);

    // Apply clock config before start()
    s_clock.set_motor_reverse(clockCfg.motor_reverse);
    s_clock.set_step_delay_us(clockCfg.step_delay_us);
    s_clock.cmd_set_sensor_offset(clockCfg.sensor_offset);
    if (clockCfg.disp_minute >= 0 && clockCfg.disp_hour >= 0) {
        s_clock.set_displayed_minute(clockCfg.disp_minute);
        s_clock.set_displayed_hour(clockCfg.disp_hour);
        ESP_LOGI(TAG, "Restored displayed position: %02d:%02d",
                 (int)clockCfg.disp_hour, (int)clockCfg.disp_minute);
    }

    // Resolve WiFi credentials (NVS → fallback to compile-time defaults)
    const char* wifi_ssid = (netCfg.ssid[0] != '\0') ? netCfg.ssid : WIFI_SSID_DEFAULT;
    const char* wifi_pass = (netCfg.password[0] != '\0') ? netCfg.password : WIFI_PASSWORD_DEFAULT;
    const char* tz_override = (netCfg.tz_override[0] != '\0') ? netCfg.tz_override : TZ_OVERRIDE_DEFAULT;
    ESP_LOGI(TAG, "WiFi SSID: %s (source: %s)", wifi_ssid,
             netCfg.ssid[0] ? "NVS" : "default");

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

    // ── 2b. LED strips ────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Initialising LED strips...");
    if (s_leds.init() != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed — continuing without LEDs");
    }
    s_leds.start();

    // Apply persisted LED config (after start so the effect task is running)
    for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
        LedManager::Target tgt = (i == 0) ? LedManager::Target::STRIP_1
                                           : LedManager::Target::STRIP_2;
        s_leds.set_active_len(tgt, ledCfg.strip[i].len);
        s_leds.set_brightness(tgt, ledCfg.strip[i].brightness);
        s_leds.set_color(tgt, ledCfg.strip[i].r, ledCfg.strip[i].g, ledCfg.strip[i].b);
        s_leds.set_effect(tgt, static_cast<LedManager::Effect>(ledCfg.strip[i].effect));
    }

    // ── 2c. Hardware buttons (active-low, pull-up) ────────────────────────────
    {
        const gpio_config_t btn_cfg = {
            .pin_bit_mask = (1ULL << BUTTON_A) | (1ULL << BUTTON_B),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&btn_cfg);
        ESP_LOGI(TAG, "Buttons: A=GPIO%d (prev/long=select), B=GPIO%d (next/long=select), A+B=dismiss",
                 (int)BUTTON_A, (int)BUTTON_B);
    }

    // ── 3. Menu — wire dismiss, then build full tree ──────────────────────────
    // dismiss_fn polls the encoder directly (with mutex) so it works even
    // while encoder_task is blocked inside wait_for_dismiss.
    s_menu.set_dismiss_fn(dismiss_fn);
    ESP_LOGI(TAG, "Building menu...");
    s_menu.build(s_clock, s_net, s_leds);

    // ── 4. Networking — async WiFi + SNTP + geolocation ──────────────────────
    s_net.set_wifi_credentials(wifi_ssid, wifi_pass);
    if (tz_override[0] != '\0') {
        s_net.set_timezone_override(tz_override);
    }
    s_net.begin();

    // ── 4a. Web server — starts HTTP + WebSocket on port 80 ──────────────────
    s_webserver.start();

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
    xTaskCreate(blank_timer_task, "blank_timer",  3072, nullptr, 2, nullptr);

    // ── 9. Splash → menu ─────────────────────────────────────────────────────
    s_display.clear();
    s_display.print(0, " Clock Driver");
    s_display.print(2, " Rotate: navigate");
    s_display.print(3, " Press:  select");
    s_display.print(4, " Hold:   back/select");
    vTaskDelay(pdMS_TO_TICKS(2500));

    s_menu.render();

    ESP_LOGI(TAG, "System running.");
    ESP_LOGI(TAG, "UART console at 115200 baud — type 'help'");
    ESP_LOGI(TAG, "Display blanks after 5 min inactivity.");
}
