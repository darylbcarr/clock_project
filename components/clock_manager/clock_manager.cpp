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
      sensor_offset_steps_(0),
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
    // Do not tick during offset calibration — user is manually aligning
    // the hand and the motor must not move autonomously.
    if (cal_mode_) return;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    // Re-check after acquiring mutex (in case cal started while we waited).
    if (cal_mode_) {
        xSemaphoreGive(mutex_);
        return;
    }

    struct tm t = get_local_tm();
    ESP_LOGI(TAG, "Tick — real time: %02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);

    // Sensor correction window: ±SENSOR_WINDOW_SECONDS around the top of the hour.
    bool near_hour = (t.tm_min == 59 && t.tm_sec >= (60 - SENSOR_WINDOW_SECONDS))
                  || (t.tm_min == 0  && t.tm_sec <= SENSOR_WINDOW_SECONDS);

    if (near_hour && sensor_offset_steps_ > 0) {
        // Replace the normal fixed advance with a step-by-step sensor search.
        // This way the hand stops when the slot is found instead of overshooting it.
        check_sensor_and_correct();
    } else {
        advance_one_minute();
    }

    xSemaphoreGive(mutex_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sensor check & correction
// ─────────────────────────────────────────────────────────────────────────────

void ClockManager::check_sensor_and_correct()
{
    // Advance step-by-step up to one clock-minute, watching for the slot.
    // This replaces the normal fixed advance near the top of the hour so the
    // hand stops when the slot is detected rather than overshooting it.
    const int POLL_EVERY = 10;
    const int MAX_SEARCH = STEPS_PER_CLOCK_MINUTE;

    int  steps_moved = 0;
    bool found       = false;

    sensor_.led_on();
    for (int s = 0; s < MAX_SEARCH && !found; s += POLL_EVERY) {
        motor_.microstep_n(POLL_EVERY, StepDirection::FORWARD);
        steps_moved += POLL_EVERY;
        int raw = sensor_.read_raw();
        last_sensor_adc_ = raw;
        if (raw > sensor_.get_threshold()) {
            found = true;
        }
    }
    sensor_.led_off();

    if (found) {
        // Slot found — advance the calibrated offset to reach exactly 12:00.
        motor_.move_steps(sensor_offset_steps_, StepDirection::FORWARD);
        struct tm t = get_local_tm();
        displayed_minute_ = 0;
        displayed_hour_   = t.tm_hour % 12;
        ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
        ESP_LOGI(TAG, "Sensor correction to %02d:00 (found after %d steps)",
                 displayed_hour_, steps_moved);
    } else {
        // Slot not found within one minute of travel (clock is fast or
        // sensor uncalibrated) — complete a normal 1-minute advance.
        int remaining = STEPS_PER_CLOCK_MINUTE - steps_moved;
        if (remaining > 0)
            motor_.move_steps(remaining, StepDirection::FORWARD);
        displayed_minute_ = (displayed_minute_ + 1) % 60;
        if (displayed_minute_ == 0 && displayed_hour_ >= 0)
            displayed_hour_ = (displayed_hour_ + 1) % 12;
        ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
        ESP_LOGD(TAG, "Sensor not found near hour — normal advance");
    }
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

    // If the user cancelled mid-move, the hand position is unknown — leave
    // displayed_* unchanged and let the user re-enter the observed position.
    if (motor_.was_cancelled()) {
        ESP_LOGI(TAG, "cmd_set_time: cancelled — hand position unknown");
        xSemaphoreGive(mutex_);
        return;
    }

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
    if (cal_phase_ == CalPhase::SLOT_FOUND) {
        cal_steps_from_trigger_ += forward ? steps : -steps;
        ESP_LOGD(TAG, "cal_steps_from_trigger: %d", cal_steps_from_trigger_);
    }
}

void ClockManager::calibrate_sensor_static()
{
    // Static calibration — no motor movement.  Safe to call on boot.
    int mean = sensor_.calibrate_safe();
    ESP_LOGI(TAG, "calibrate_sensor_static: dark_mean=%d  threshold=%d",
             mean, sensor_.get_threshold());
}

void ClockManager::cmd_calibrate_sensor_safe()
{
    // Prevent tick() from running while we drive the motor.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cal_mode_ = true;
    xSemaphoreGive(mutex_);

    ESP_LOGI(TAG, "cmd_calibrate_sensor_safe: advancing ring 10 min, sampling ambient...");

    // Advance the ring 10 clock-minutes, collecting 128 ADC samples spread
    // evenly across the full travel.  This averages out position-dependent
    // external-light variation and dilutes the slot reflection (a high outlier)
    // to a small fraction of the dataset that the trimmed mean discards.
    const int MAX_SAMPLES = 128;
    const int TOTAL_STEPS = STEPS_PER_CLOCK_MINUTE * 10;
    const int CHUNK       = TOTAL_STEPS / MAX_SAMPLES;   // ~47 half-steps each

    int buf[MAX_SAMPLES];

    sensor_.led_on();
    for (int i = 0; i < MAX_SAMPLES; ++i) {
        motor_.microstep_n(CHUNK, StepDirection::FORWARD);
        buf[i] = sensor_.read_raw();
    }
    sensor_.led_off();

    sensor_.calibrate_from_samples(buf, MAX_SAMPLES);

    // Update the tracked displayed position for the 10-minute advance.
    for (int i = 0; i < 10; ++i) {
        displayed_minute_ = (displayed_minute_ + 1) % 60;
        if (displayed_minute_ == 0 && displayed_hour_ >= 0)
            displayed_hour_ = (displayed_hour_ + 1) % 12;
    }
    ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);

    ESP_LOGI(TAG, "cmd_calibrate_sensor_safe: dark_mean=%d  threshold=%d  pos=%02d:%02d",
             sensor_.get_dark_mean(), sensor_.get_threshold(),
             displayed_hour_, displayed_minute_);

    cal_mode_ = false;
}

void ClockManager::cmd_start_offset_cal()
{
    // Acquire mutex to wait for any in-progress tick to complete, then set the
    // cal_mode_ flag so tick() won't run while we're calibrating.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cal_mode_  = true;
    cal_phase_ = CalPhase::MOVING_TO_SLOT;
    cal_steps_from_trigger_ = 0;
    xSemaphoreGive(mutex_);

    ESP_LOGI(TAG, "cmd_start_offset_cal: searching for slot...");

    // Advance the motor slowly, checking the sensor every few steps.
    // Search up to 30 full clock-minutes of travel.
    const int MAX_STEPS    = STEPS_PER_CLOCK_MINUTE * 30;
    const int POLL_EVERY   = 10;   // half-steps between sensor checks
    bool found = false;

    for (int s = 0; s < MAX_STEPS; s += POLL_EVERY) {
        motor_.microstep_n(POLL_EVERY, StepDirection::FORWARD);
        if (sensor_.is_triggered()) {
            found = true;
            break;
        }
    }

    if (found) {
        cal_phase_ = CalPhase::SLOT_FOUND;
        ESP_LOGI(TAG, "cmd_start_offset_cal: slot found — step hand to 12:00 then call finish");
    } else {
        cal_phase_ = CalPhase::NOT_FOUND;
        cal_mode_  = false;   // re-enable tick on failure
        ESP_LOGW(TAG, "cmd_start_offset_cal: slot not found within %d steps", MAX_STEPS);
    }
}

void ClockManager::cmd_finish_offset_cal()
{
    if (cal_phase_ != CalPhase::SLOT_FOUND) {
        ESP_LOGW(TAG, "cmd_finish_offset_cal: not in SLOT_FOUND phase — ignored");
        return;
    }

    sensor_offset_steps_ = cal_steps_from_trigger_;

    ClockCfg cc;
    ConfigStore::load(cc);
    cc.sensor_offset_steps = sensor_offset_steps_;
    ConfigStore::save(cc);

    ESP_LOGI(TAG, "cmd_finish_offset_cal: offset=%d steps (saved)", sensor_offset_steps_);

    cal_phase_ = CalPhase::IDLE;
    cal_mode_  = false;
}

void ClockManager::cmd_abort_offset_cal()
{
    ESP_LOGI(TAG, "cmd_abort_offset_cal: calibration cancelled");
    cal_phase_ = CalPhase::IDLE;
    cal_mode_  = false;
}

void ClockManager::cmd_test_advance()
{
    ESP_LOGI(TAG, "cmd_test_advance: forcing one-minute advance");
    xSemaphoreTake(mutex_, portMAX_DELAY);
    advance_one_minute();
    xSemaphoreGive(mutex_);
}

void ClockManager::cmd_test_reverse()
{
    ESP_LOGI(TAG, "cmd_test_reverse: forcing one-minute reverse");
    xSemaphoreTake(mutex_, portMAX_DELAY);
    motor_.move_clock_minutes(-1);
    if (displayed_minute_ >= 0) {
        displayed_minute_ = (displayed_minute_ + 59) % 60;  // subtract 1, wrap
        if (displayed_minute_ == 59 && displayed_hour_ >= 0)
            displayed_hour_ = (displayed_hour_ + 11) % 12;  // subtract 1, wrap
    }
    ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
    xSemaphoreGive(mutex_);
}

void ClockManager::cmd_cancel_move()
{
    motor_.request_cancel();
    ESP_LOGI(TAG, "cmd_cancel_move: cancel requested");
}

void ClockManager::cmd_status()
{
    ESP_LOGI(TAG, "──── ClockManager Status ────────────────────────────");
    ESP_LOGI(TAG, "  Running         : %s", is_running()    ? "yes" : "no");
    ESP_LOGI(TAG, "  Time valid      : %s", is_time_valid() ? "yes" : "no");
    ESP_LOGI(TAG, "  Displayed pos   : %02d:%02d", displayed_hour_, displayed_minute_);
    ESP_LOGI(TAG, "  Sensor offset   : %d steps", sensor_offset_steps_);
    ESP_LOGI(TAG, "  Cal phase       : %d", static_cast<int>(cal_phase_));
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

void ClockManager::cmd_sensor_read(int n)
{
    int thr = sensor_.get_threshold();
    printf("\r\nSensor live readings   mean=%d  threshold=%d\r\n",
           sensor_.get_dark_mean(), thr);
    printf("  Manually rotate ring through the slot to see ADC spike.\r\n");
    printf("  Press any key in terminal to stop early.\r\n\r\n");
    printf("  %-4s  %-6s  %-8s  %s\r\n", "#", "ADC", "trigger?", "level (0–4095)");
    printf("  %s\r\n", "─────────────────────────────────────────────────────");

    sensor_.led_on();
    for (int i = 0; i < n; ++i) {
        int  val  = sensor_.read_raw();
        bool trig = (val > thr);

        // Scale to a 40-char bar
        int bar_len = (val * 40) / 4095;
        if (bar_len < 0)  bar_len = 0;
        if (bar_len > 40) bar_len = 40;

        char bar[42] = {};
        for (int b = 0; b < bar_len; ++b) bar[b] = (trig ? '#' : '=');

        printf("  %-4d  %-6d  %-8s  |%s\r\n",
               i + 1, val, trig ? "YES <<<" : "no", bar);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
    sensor_.led_off();
    printf("\r\n");
}

void ClockManager::cmd_sensor_read_single()
{
    sensor_.led_on();
    last_sensor_adc_ = sensor_.read_raw();
    sensor_.led_off();
    ESP_LOGI(TAG, "sensor-read-single: adc=%d  threshold=%d  triggered=%s",
             last_sensor_adc_, sensor_.get_threshold(),
             last_sensor_adc_ > sensor_.get_threshold() ? "YES" : "no");
}

void ClockManager::cmd_sensor_scan(int minutes)
{
    if (minutes < 1)  minutes = 1;
    if (minutes > 60) minutes = 60;

    // Prevent tick while motor is moving.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cal_mode_ = true;
    xSemaphoreGive(mutex_);

    scanning_   = true;
    scan_count_ = 0;

    int thr         = sensor_.get_threshold();
    int total_steps = STEPS_PER_CLOCK_MINUTE * minutes;
    const int CHUNK = 200;   // half-steps between readings

    printf("\r\nSensor scan  (%d minutes, %d steps)   mean=%d  threshold=%d\r\n",
           minutes, total_steps, sensor_.get_dark_mean(), thr);
    printf("  %-6s  %-6s  %-8s  %s\r\n", "step", "ADC", "trigger?", "level (0-4095)");
    printf("  %s\r\n", "-----------------------------------------------------");

    sensor_.led_on();
    int steps_done = 0;
    while (steps_done < total_steps) {
        int move = (total_steps - steps_done < CHUNK)
                   ? (total_steps - steps_done) : CHUNK;
        motor_.microstep_n(move, StepDirection::FORWARD);
        steps_done += move;

        int  val  = sensor_.read_raw();
        bool trig = (val > thr);

        last_sensor_adc_ = val;

        // Store in scan buffer (capped at SCAN_BUF_SIZE)
        if (scan_count_ < SCAN_BUF_SIZE) {
            scan_buf_[scan_count_].step = steps_done;
            scan_buf_[scan_count_].adc  = val;
            ++scan_count_;
        }

        int bar_len = (val * 40) / 4095;
        if (bar_len < 0)  bar_len = 0;
        if (bar_len > 40) bar_len = 40;

        char bar[42] = {};
        for (int b = 0; b < bar_len; ++b) bar[b] = (trig ? '#' : '=');

        printf("  %-6d  %-6d  %-8s  |%s\r\n",
               steps_done, val, trig ? "YES <<<" : "no", bar);
    }
    sensor_.led_off();
    printf("\r\n");

    // Update tracked position.
    for (int i = 0; i < minutes; ++i) {
        displayed_minute_ = (displayed_minute_ + 1) % 60;
        if (displayed_minute_ == 0 && displayed_hour_ >= 0)
            displayed_hour_ = (displayed_hour_ + 1) % 12;
    }
    ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);

    scanning_ = false;
    cal_mode_ = false;
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
