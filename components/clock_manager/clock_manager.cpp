/**
 * @file clock_manager.cpp
 * @brief Clock management implementation
 */

#include "clock_manager.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <sys/time.h>   // settimeofday()
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "clock_manager";

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

ClockManager::ClockManager(uint32_t motor_delay_us)
    : motor_(motor_delay_us),
      sensor_(),
      displayed_minute_(-1),   // unknown until set
      sensor_offset_sec_(0),
      time_valid_(false),
      running_(false),
      task_handle_(nullptr),
      mutex_(nullptr)
{
    mutex_ = xSemaphoreCreateMutex();
    configASSERT(mutex_);

    // Timezone only — safe to set any time, no lwIP dependency.
    setenv("TZ", DEFAULT_TZ, 1);
    tzset();

    // NOTE: SNTP configuration is deferred to start(), which is called
    // after the TCP/IP stack and event loop are initialised by Networking::begin().
    ESP_LOGI(TAG, "ClockManager constructed");
}

ClockManager::~ClockManager()
{
    stop();
    if (mutex_) vSemaphoreDelete(mutex_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::start()
{
    if (running_) return;
    running_ = true;

    // SNTP is initialised by the Networking component after the TCP/IP stack
    // and event loop are up.  ClockManager::start() must NOT call esp_sntp_*
    // here — the lwIP mbox does not exist yet at construction time.

    xTaskCreate(
        clock_task,
        "clock_mgr",
        4096,
        this,
        5,              // priority
        &task_handle_
    );
    ESP_LOGI(TAG, "Clock manager started");
}

void ClockManager::stop()
{
    running_ = false;
    if (task_handle_) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    // SNTP lifecycle is owned by the Networking component; do not call
    // esp_sntp_stop() here.
}

// ─────────────────────────────────────────────────────────────────────────────
// Background task
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::clock_task(void* arg)
{
    ClockManager* self = static_cast<ClockManager*>(arg);

    // Calculate the delay to the next whole minute boundary
    struct tm now_tm = self->get_local_tm();
    int seconds_into_minute = now_tm.tm_sec;
    uint32_t delay_to_next_minute_ms =
        (uint32_t)((60 - seconds_into_minute) * 1000);

    // Wait until the next whole minute
    vTaskDelay(pdMS_TO_TICKS(delay_to_next_minute_ms));

    while (self->running_) {
        self->tick();
        // Sleep until the next whole minute (1-second correction loop)
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-minute tick
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::tick()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);

    struct tm t = get_local_tm();
    ESP_LOGI(TAG, "Tick — real time: %02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);

    // ── Advance the hand one minute ──────────────────────────────────────────
    advance_one_minute();

    // ── Check sensor near top-of-hour ────────────────────────────────────────
    // Sensor window: last 30 s before or first 30 s after the hour
    int min = t.tm_min;
    int sec = t.tm_sec;
    bool near_hour = (min == 59 && sec >= (60 - SENSOR_WINDOW_SECONDS))
                  || (min == 0  && sec <= SENSOR_WINDOW_SECONDS);

    if (near_hour) {
        check_sensor_and_correct();
    }

    xSemaphoreGive(mutex_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sensor check & correction
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::check_sensor_and_correct()
{
    struct tm t = get_local_tm();
    int seconds_into_hour = t.tm_min * 60 + t.tm_sec;

    bool triggered = sensor_.is_triggered();
    if (!triggered) {
        ESP_LOGD(TAG, "Sensor not triggered at %d s into hour", seconds_into_hour);
        return;
    }

    // Apply user-defined offset so that trigger aligns with actual top-of-hour
    int adjusted_seconds = seconds_into_hour - sensor_offset_sec_;

    // Convert to minute error: positive = hand is late, negative = early
    // Top-of-hour = second 0 (or 3600).
    // If the sensor fires at adjusted_seconds = 10, hand is 10 s late.
    // We convert seconds to fractional minutes for display.
    int error_seconds = adjusted_seconds;   // relative to hour boundary (0)

    ESP_LOGI(TAG, "Sensor triggered: adjusted_s=%d  error_s=%d",
             adjusted_seconds, error_seconds);

    // ── Determine correction in whole minutes ────────────────────────────────
    int error_minutes = error_seconds / 60;
    if (std::abs(error_minutes) > MAX_AUTO_CORRECT_MINUTES) {
        ESP_LOGW(TAG,
                 "Drift (%d min) exceeds auto-correct limit (%d min). "
                 "Use cmd_set_time().",
                 error_minutes, MAX_AUTO_CORRECT_MINUTES);
        return;
    }

    if (error_minutes == 0) {
        ESP_LOGI(TAG, "Hand is within 1-minute tolerance. No correction needed.");
        return;
    }

    ESP_LOGI(TAG, "Correcting hand by %d minute(s)", -error_minutes);
    // A positive error means the sensor fired after the hour — hand is late,
    // so we need to move backward (rewind) to correct.
    StepDirection dir = (error_minutes > 0) ? StepDirection::BACKWARD
                                             : StepDirection::FORWARD;
    motor_.move_steps(std::abs(error_minutes) * STEPS_PER_CLOCK_MINUTE, dir);
    displayed_minute_ = (displayed_minute_ - error_minutes + 60) % 60;
}

// ─────────────────────────────────────────────────────────────────────────────
// Motor helpers
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::advance_one_minute()
{
    motor_.move_clock_minutes(1);
    if (displayed_minute_ < 0) {
        displayed_minute_ = 0;
    } else {
        displayed_minute_ = (displayed_minute_ + 1) % 60;
    }
    ESP_LOGD(TAG, "Displayed minute now: %d", displayed_minute_);
}

int ClockManager::minutes_to_target(int target_min) const
{
    if (displayed_minute_ < 0) return 0;
    int current = displayed_minute_;
    int delta = (target_min - current + 60) % 60;
    // Prefer shorter path (max 30 min forward, otherwise go backward)
    if (delta > 30) delta -= 60;
    return delta;
}

// ─────────────────────────────────────────────────────────────────────────────
// Time sync
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::set_timezone(const char* tz)
{
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz);
}

void ClockManager::on_time_synced()
{
    time_valid_ = true;
    ESP_LOGI(TAG, "Time synced. Current time: %s", time_full().c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// Command interface
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::cmd_set_time(int current_displayed_minute)
{
    if (!time_valid_) {
        ESP_LOGW(TAG, "cmd_set_time: time not yet valid (SNTP not synced)");
        ESP_LOGW(TAG, "  → Use 'set-clock <HH> <MM>' to set time manually for testing");
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    struct tm t = get_local_tm();
    int target_minute = t.tm_min;

    if (current_displayed_minute >= 0 && current_displayed_minute < 60) {
        displayed_minute_ = current_displayed_minute;
    }

    int delta = minutes_to_target(target_minute);

    ESP_LOGI(TAG, "cmd_set_time: displayed=%d  target=%d  delta=%d",
             displayed_minute_, target_minute, delta);

    if (delta == 0) {
        ESP_LOGI(TAG, "Hand already at correct minute");
    } else if (delta > 0) {
        motor_.move_clock_minutes(delta);
        displayed_minute_ = target_minute;
    } else {
        motor_.move_steps(std::abs(delta) * STEPS_PER_CLOCK_MINUTE,
                          StepDirection::BACKWARD);
        displayed_minute_ = target_minute;
    }

    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "cmd_set_time: hand set to minute %d", displayed_minute_);
}

void ClockManager::cmd_set_manual_time(int hour, int minute, int second)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        ESP_LOGE(TAG, "cmd_set_manual_time: invalid time %02d:%02d:%02d",
                 hour, minute, second);
        return;
    }

    // Build a tm from the supplied local time + today's date.
    // We keep today's date by reading it from the current system time
    // (which may be epoch 0 if never set — that's fine, only H:M:S matters
    // for the clock hand logic).
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);

    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;

    // mktime() interprets t as local time and returns UTC epoch.
    time_t new_epoch = mktime(&t);
    if (new_epoch == (time_t)-1) {
        ESP_LOGE(TAG, "cmd_set_manual_time: mktime failed");
        return;
    }

    struct timeval tv = { .tv_sec = new_epoch, .tv_usec = 0 };
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGE(TAG, "cmd_set_manual_time: settimeofday failed");
        return;
    }

    time_valid_ = true;
    ESP_LOGI(TAG, "Manual time set to %02d:%02d:%02d  (epoch %lld)",
             hour, minute, second, (long long)new_epoch);
    ESP_LOGI(TAG, "You can now run 'set-time <current_hand_minute>'");
}

void ClockManager::cmd_sync_status()
{
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);
    char tz_buf[64] = {};
    strftime(tz_buf, sizeof(tz_buf), "%Z %z", &t);

    ESP_LOGI(TAG, "──── Sync / Network Status ──────────────────────────");
    ESP_LOGI(TAG, "  time_valid   : %s", time_valid_ ? "YES" : "NO");
    ESP_LOGI(TAG, "  epoch        : %lld", (long long)now);
    if (now > 1000000000LL) {   // sanity: after year 2001
        ESP_LOGI(TAG, "  local time   : %s", time_full().c_str());
        ESP_LOGI(TAG, "  timezone     : %s", tz_buf);
    } else {
        ESP_LOGI(TAG, "  local time   : (not set — epoch near zero)");
    }
    if (!time_valid_) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "  Time is not valid.  Options:");
        ESP_LOGW(TAG, "    1) Implement WiFi in networking.cpp for SNTP sync");
        ESP_LOGW(TAG, "    2) Run: set-clock <hour> <minute>  (manual entry)");
        ESP_LOGW(TAG, "       e.g.  set-clock 14 35");
        ESP_LOGW(TAG, "       Then: set-time <hand_minute>");
    }
    ESP_LOGI(TAG, "─────────────────────────────────────────────────────");
}

void ClockManager::cmd_microstep(int steps, bool forward)
{
    StepDirection dir = forward ? StepDirection::FORWARD : StepDirection::BACKWARD;
    ESP_LOGI(TAG, "cmd_microstep: %d steps %s", steps, forward ? "FWD" : "BWD");
    motor_.microstep_n(steps, dir);
}

void ClockManager::cmd_calibrate_sensor()
{
    ESP_LOGI(TAG, "cmd_calibrate_sensor: starting...");
    int dark = sensor_.calibrate_dark();
    ESP_LOGI(TAG, "cmd_calibrate_sensor: dark_mean=%d  threshold=%d",
             dark, sensor_.get_threshold());
}

int ClockManager::cmd_measure_sensor_average()
{
    ESP_LOGI(TAG, "cmd_measure_sensor_average: measuring...");
    sensor_.led_on();
    int avg = sensor_.read_average(SENSOR_CALIB_SAMPLES);
    sensor_.led_off();
    ESP_LOGI(TAG, "cmd_measure_sensor_average: avg=%d  threshold=%d",
             avg, sensor_.get_threshold());
    return avg;
}

void ClockManager::cmd_set_sensor_offset(int seconds_offset)
{
    sensor_offset_sec_ = seconds_offset;
    ESP_LOGI(TAG, "cmd_set_sensor_offset: offset=%d s", sensor_offset_sec_);
}

void ClockManager::cmd_test_advance()
{
    ESP_LOGI(TAG, "cmd_test_advance: forcing one-minute advance");
    xSemaphoreTake(mutex_, portMAX_DELAY);
    advance_one_minute();
    xSemaphoreGive(mutex_);
}

void ClockManager::cmd_status()
{
    ESP_LOGI(TAG, "──── ClockManager Status ────────────────────────────");
    ESP_LOGI(TAG, "  Running         : %s", is_running()    ? "yes" : "no");
    ESP_LOGI(TAG, "  Time valid      : %s", is_time_valid() ? "yes" : "no");
    ESP_LOGI(TAG, "  Displayed min   : %d", displayed_minute_);
    ESP_LOGI(TAG, "  Sensor offset   : %d s", sensor_offset_sec_);
    ESP_LOGI(TAG, "  Sensor threshold: %d", sensor_.get_threshold());
    ESP_LOGI(TAG, "  Sensor dark mean: %d", sensor_.get_dark_mean());
    ESP_LOGI(TAG, "  Motor powered   : %s", motor_.is_powered() ? "yes" : "no");
    ESP_LOGI(TAG, "  Motor step delay: %lu µs", (unsigned long)motor_.get_step_delay());
    ESP_LOGI(TAG, "  Total steps     : %lld", motor_.get_total_steps());
    if (time_valid_) {
        ESP_LOGI(TAG, "  Local time      : %s", time_full().c_str());
    }
    ESP_LOGI(TAG, "─────────────────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// Time formatting
// ─────────────────────────────────────────────────────────────────────────────

struct tm ClockManager::get_local_tm() const
{
    time_t now = time(nullptr);
    struct tm result = {};
    localtime_r(&now, &result);
    return result;
}

std::string ClockManager::format_time(const char* fmt) const
{
    struct tm t = get_local_tm();
    char buf[64];
    strftime(buf, sizeof(buf), fmt, &t);
    return std::string(buf);
}
