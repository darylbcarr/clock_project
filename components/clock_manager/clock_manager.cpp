/**
 * @file clock_manager.cpp
 * @brief Clock management implementation
 */

#include "clock_manager.h"
#include "config_store.h"

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
      displayed_hour_(-1),     // unknown until set
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
    static constexpr uint32_t SYNC_NOTIFY = 1U;

    // ── Initial alignment: wait until the next whole-minute boundary ──────────
    // Interruptible so an SNTP sync can trigger an immediate hand correction.
    {
        struct tm t = self->get_local_tm();
        uint32_t delay_ms = (uint32_t)((60 - t.tm_sec) * 1000);
        uint32_t notif = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(delay_ms)) == pdTRUE
            && notif == SYNC_NOTIFY)
        {
            ESP_LOGI("clock_manager", "Initial SNTP sync — aligning hands");
            self->cmd_set_time(-1, -1);
        }
    }

    while (self->running_) {
        self->tick();

        // Sleep 60 s; wake early if SNTP syncs and we need to re-align.
        uint32_t notif = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(60000)) == pdTRUE
            && notif == SYNC_NOTIFY)
        {
            ESP_LOGI("clock_manager", "Mid-run SNTP sync — aligning hands");
            self->cmd_set_time(-1, -1);
        }
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
    // Sensor fires near the hour; sync displayed_hour_ from real time
    displayed_hour_ = t.tm_hour % 12;
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
        if (displayed_minute_ == 0 && displayed_hour_ >= 0)
            displayed_hour_ = (displayed_hour_ + 1) % 12;
    }
    ESP_LOGD(TAG, "Displayed position now: %02d:%02d", displayed_hour_, displayed_minute_);
    ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
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
    // If we restored a known hand position from NVS, notify clock_task to sync immediately
    if (displayed_hour_ >= 0 && displayed_minute_ >= 0 && task_handle_) {
        xTaskNotify(task_handle_, 1U, eSetValueWithOverwrite);
        ESP_LOGI(TAG, "Notified clock_task to sync hand (displayed=%02d:%02d)",
                 displayed_hour_, displayed_minute_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Command interface
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::cmd_set_time(int obs_hour, int obs_min)
{
    if (!time_valid_) {
        ESP_LOGW(TAG, "cmd_set_time: time not yet valid (SNTP not synced)");
        ESP_LOGW(TAG, "  → Use 'set-clock <HH> <MM>' to set time manually for testing");
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Accept new observed position if provided
    if (obs_hour >= 0 && obs_hour <= 11 && obs_min >= 0 && obs_min <= 59) {
        displayed_hour_   = obs_hour;
        displayed_minute_ = obs_min;
    }

    if (displayed_hour_ < 0 || displayed_minute_ < 0) {
        ESP_LOGW(TAG, "cmd_set_time: hand position unknown — call with obs_hour/obs_min first");
        xSemaphoreGive(mutex_);
        return;
    }

    struct tm t = get_local_tm();
    int target_12h  = (t.tm_hour % 12) * 60 + t.tm_min;
    int current_12h = displayed_hour_  * 60 + displayed_minute_;
    int delta       = (target_12h - current_12h + 720) % 720;

    // If the forward distance is more than 6 hours (360 min on a 12-h face),
    // the backward path is shorter — negate delta to drive the motor in reverse.
    if (delta > 360) delta -= 720;

    ESP_LOGI(TAG, "cmd_set_time: current=%02d:%02d  target=%02d:%02d  delta=%d min (%s)",
             displayed_hour_, displayed_minute_,
             t.tm_hour % 12, t.tm_min, delta,
             delta < 0 ? "backward" : "forward");

    if (delta == 0) {
        ESP_LOGI(TAG, "Hand already at correct position");
        xSemaphoreGive(mutex_);
        ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
        return;
    }

    // Move hands to the target captured above.
    // This takes real time (~1.23 s per clock-minute at 2 ms/step), so by the
    // time the motor stops, real time has advanced beyond the original target.
    motor_.move_clock_minutes(delta);

    // Re-read the clock and apply a small catch-up correction for the
    // minutes that elapsed while the motor was running.
    struct tm t2 = get_local_tm();
    int actual_12h = (t2.tm_hour % 12) * 60 + t2.tm_min;
    int correction = (actual_12h - target_12h + 720) % 720;

    if (correction > 0) {
        ESP_LOGI(TAG, "cmd_set_time: post-move correction %d min (motor elapsed time)",
                 correction);
        motor_.move_clock_minutes(correction);
        // Re-read once more to get the settled time after correction
        t2 = get_local_tm();
    }

    displayed_hour_   = t2.tm_hour % 12;
    displayed_minute_ = t2.tm_min;

    xSemaphoreGive(mutex_);
    ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
    ESP_LOGI(TAG, "cmd_set_time: hand set to %02d:%02d", displayed_hour_, displayed_minute_);
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
    last_sensor_adc_ = avg;
    ESP_LOGI(TAG, "cmd_measure_sensor_average: avg=%d  threshold=%d",
             avg, sensor_.get_threshold());
    return avg;
}

void ClockManager::cmd_set_sensor_offset(int seconds_offset)
{
    sensor_offset_sec_ = seconds_offset;
    ClockCfg cc;
    ConfigStore::load(cc);
    cc.sensor_offset = sensor_offset_sec_;
    ConfigStore::save(cc);
    ESP_LOGI(TAG, "cmd_set_sensor_offset: offset=%d s  (saved)", sensor_offset_sec_);
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
    ESP_LOGI(TAG, "  Displayed pos   : %02d:%02d", displayed_hour_, displayed_minute_);
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
