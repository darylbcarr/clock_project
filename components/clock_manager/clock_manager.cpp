/**
 * @file clock_manager.cpp
 * @brief Clock management implementation
 */

#include "clock_manager.h"
#include "config_store.h"
#include "event_log.h"

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

    // Sleep until the next whole-minute (:00 second) boundary.
    // If an SNTP notification arrives, run cmd_set_time() then continue waiting
    // for the *following* :00 — this prevents the subsequent tick() from
    // double-advancing the hands on the same minute that cmd_set_time() just set.
    auto wait_for_next_minute = [&]() {
        while (true) {
            struct tm t = self->get_local_tm();
            uint32_t delay_ms = (uint32_t)((60 - t.tm_sec) * 1000);
            uint32_t notif = 0;
            bool synced = (xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(delay_ms)) == pdTRUE
                           && notif == SYNC_NOTIFY);
            if (synced) {
                ESP_LOGI("clock_manager", "SNTP sync — aligning hands");
                self->cmd_set_time(-1, -1);
                // Re-align: wait for the next :00 after cmd_set_time so that
                // tick() doesn't immediately advance again on the same minute.
                continue;
            }
            break;  // timed out naturally at a :00 boundary — ready to tick
        }
    };

    // ── Initial alignment: wait until the next whole-minute boundary ──────────
    wait_for_next_minute();

    while (self->running_) {
        self->tick();

        // Re-align: sleep until the next :00 boundary (not a flat 60 s),
        // so that tick() always fires just after :00 regardless of scan duration.
        wait_for_next_minute();
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

    // Motor must not run until SNTP has provided a valid time.
    // After power-cycle without WiFi, the system clock is at epoch (1970).
    if (!time_valid_) return;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    // Re-check after acquiring mutex (in case cal started while we waited).
    if (cal_mode_) {
        xSemaphoreGive(mutex_);
        return;
    }

    // Hand position unknown (e.g. a Set Time was cancelled mid-move).
    // Don't advance until the user re-runs Set Time to re-establish position.
    if (displayed_hour_ < 0 || displayed_minute_ < 0) {
        xSemaphoreGive(mutex_);
        return;
    }

    struct tm t = get_local_tm();
    ESP_LOGI(TAG, "Tick — real time: %02d:%02d:%02d  disp=%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec,
             displayed_hour_, displayed_minute_);

    // ── E: DST / large-drift detection ───────────────────────────────────────
    // If the displayed position diverges from real time by more than
    // MAX_AUTO_CORRECT_MINUTES, the sensor-based correction cannot recover.
    // A DST transition shifts local time by exactly ±60 minutes; this check
    // catches it on the first tick after the transition and calls cmd_set_time()
    // to drive the hands to the correct position (same path as first-boot align).
    //
    // Exception: if suppress_align_ is still set (first-time setup / BLE
    // commissioning), skip the large move on this tick and clear the flag.
    // The user will align the hands via the web UI after commissioning.
    if (displayed_hour_ >= 0 && displayed_minute_ >= 0) {
        int real_min12 = (t.tm_hour % 12) * 60 + t.tm_min;
        int disp_min12 = displayed_hour_ * 60 + displayed_minute_;
        int dst_delta  = (real_min12 - disp_min12 + 720) % 720;
        if (dst_delta > 360) dst_delta -= 720;  // shortest path on 12-h face
        if (std::abs(dst_delta) > MAX_AUTO_CORRECT_MINUTES) {
            if (suppress_align_) {
                ESP_LOGW(TAG, "DST/drift: delta=%+d min suppressed during commissioning — "
                         "use web UI to align hands after setup", dst_delta);
                suppress_align_ = false;
            } else {
                ESP_LOGW(TAG, "DST/drift: disp=%02d:%02d  real=%02d:%02d  delta=%+d min — realigning",
                         displayed_hour_, displayed_minute_,
                         t.tm_hour % 12, t.tm_min, dst_delta);
                EventLog::log(LogCat::CLOCK_STARTUP,
                    "DST/drift %+d min: disp %02d:%02d → realign",
                    dst_delta, displayed_hour_, displayed_minute_);
                correction_pending_ = false;
                correction_accum_   = 0;
                past_hour_          = false;
                xSemaphoreGive(mutex_);
                cmd_set_time(-1, -1);
                return;
            }
        }
    }

    // ── A: sensor not calibrated ──────────────────────────────────────────────
    if (sensor_offset_steps_ <= 0) {
        advance_one_minute();
        xSemaphoreGive(mutex_);
        return;
    }

    bool at_top_of_hour = (t.tm_min == 0);

    // ── B: fast correction — slot was found in a prior scan, now at :00 ───────
    if (correction_pending_ && at_top_of_hour) {
        // Motor is correction_accum_ steps past 12:00.  Move back.
        if (correction_accum_ > 0) {
            motor_.microstep_n(correction_accum_, StepDirection::BACKWARD);
        } else if (correction_accum_ < 0) {
            motor_.microstep_n(-correction_accum_, StepDirection::FORWARD);
        }
        displayed_hour_   = t.tm_hour % 12;
        displayed_minute_ = 0;
        ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
        ESP_LOGI(TAG, "Fast correction applied: %d steps back → %02d:00",
                 correction_accum_, displayed_hour_);
        EventLog::log(LogCat::CLOCK_SENSOR,
            "Sensor adj (deferred) %+d steps → %02d:00",
            correction_accum_, displayed_hour_);
        correction_pending_ = false;
        correction_accum_   = 0;
        xSemaphoreGive(mutex_);
        return;
    }

    // ── C: fast correction accumulating — slot found, :00 not yet reached ─────
    if (correction_pending_ && !at_top_of_hour) {
        // Safety: if the accumulation has grown beyond the max correction window
        // the original slot detection was almost certainly a false positive.
        // Abandon it rather than executing a destructive large backward move.
        if (correction_accum_ > MAX_AUTO_CORRECT_MINUTES * STEPS_PER_CLOCK_MINUTE) {
            ESP_LOGW(TAG, "Fast correction abandoned: accum=%d exceeded max window — "
                          "likely false sensor trigger", correction_accum_);
            EventLog::log(LogCat::CLOCK_SENSOR,
                "Sensor adj abandoned (accum=%d, false trigger?)", correction_accum_);
            correction_pending_ = false;
            correction_accum_   = 0;
            // Fall through to path D for a normal advance this minute.
        } else {
            // One more real minute has passed; advance motor and grow the correction.
            motor_.microstep_n(STEPS_PER_CLOCK_MINUTE, StepDirection::FORWARD);
            correction_accum_ += STEPS_PER_CLOCK_MINUTE;
            displayed_minute_ = (displayed_minute_ + 1) % 60;
            if (displayed_minute_ == 0 && displayed_hour_ >= 0)
                displayed_hour_ = (displayed_hour_ + 1) % 12;
            ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
            ESP_LOGD(TAG, "Fast correction accumulating: accum=%d  disp=%02d:%02d",
                     correction_accum_, displayed_hour_, displayed_minute_);
            xSemaphoreGive(mutex_);
            return;
        }
    }

    // ── D: normal scan — advance one minute while watching for the slot ────────
    // Only run scan_full() when within MAX_AUTO_CORRECT_MINUTES of the top of the
    // hour, or when past_hour_ is active (slow-case catch-up after missing :00).
    // Scanning every minute risks false positives far from :00 which cause large
    // backward corrections when path B fires at the next :00.
    bool near_top = (t.tm_min >= (60 - MAX_AUTO_CORRECT_MINUTES))
                    || at_top_of_hour
                    || past_hour_;

    if (!near_top) {
        advance_one_minute();
        EventLog::log(LogCat::CLOCK_TICK,
            "Tick → %02d:%02d", displayed_hour_, displayed_minute_);
        xSemaphoreGive(mutex_);
        return;
    }

    int  trigger_step = 0;
    bool found        = scan_full(trigger_step);

    // Update displayed position for the motor advance we just did.
    int new_min  = (displayed_minute_ + 1) % 60;
    int new_hour = displayed_hour_;
    if (new_min == 0 && displayed_hour_ >= 0)
        new_hour = (displayed_hour_ + 1) % 12;

    bool needs_catchup = false;

    if (found) {
        // overshoot > 0  → motor went past 12:00 (need backward move)
        // overshoot < 0  → motor stopped before 12:00 (need forward move)
        int overshoot = STEPS_PER_CLOCK_MINUTE - trigger_step - sensor_offset_steps_;
        ESP_LOGI(TAG, "Slot found at step %d  overshoot=%d  past_hour=%d",
                 trigger_step, overshoot, (int)past_hour_);

        if (!past_hour_) {
            // ── Fast / on-time case ────────────────────────────────────────
            if (at_top_of_hour) {
                // Correct immediately
                if (overshoot > 0)
                    motor_.microstep_n(overshoot, StepDirection::BACKWARD);
                else if (overshoot < 0)
                    motor_.microstep_n(-overshoot, StepDirection::FORWARD);
                displayed_hour_   = t.tm_hour % 12;
                displayed_minute_ = 0;
                ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
                ESP_LOGI(TAG, "On-time correction: %d steps → %02d:00",
                         overshoot, displayed_hour_);
                if (overshoot != 0) {
                    EventLog::log(LogCat::CLOCK_SENSOR,
                        "Sensor adj (on-time) %+d steps → %02d:00",
                        overshoot, displayed_hour_);
                } else {
                    EventLog::log(LogCat::CLOCK_TICK,
                        "Sensor: at %02d:00 exact, no adj needed", displayed_hour_);
                }
            } else {
                // Defer backward correction to the :00 tick
                correction_pending_ = true;
                correction_accum_   = overshoot;
                displayed_minute_   = new_min;
                displayed_hour_     = new_hour;
                ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
                ESP_LOGI(TAG, "Fast case: slot at step %d, correction %d deferred to :00",
                         trigger_step, overshoot);
                EventLog::log(LogCat::CLOCK_SENSOR,
                    "Sensor: slot@step %d, defer %+d steps to :00",
                    trigger_step, overshoot);
            }
        } else {
            // ── Slow case: :00 already passed, slot just found ────────────
            // Motor is at end of scan; move to exact 12:00
            if (overshoot > 0)
                motor_.microstep_n(overshoot, StepDirection::BACKWARD);
            else if (overshoot < 0)
                motor_.microstep_n(-overshoot, StepDirection::FORWARD);
            displayed_hour_   = t.tm_hour % 12;
            displayed_minute_ = 0;
            ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);
            past_hour_        = false;
            ESP_LOGI(TAG, "Slow correction: moved to %02d:00, will fast-forward to real time",
                     displayed_hour_);
            EventLog::log(LogCat::CLOCK_SENSOR,
                "Sensor adj (slow) %+d steps → %02d:00, realigning",
                overshoot, displayed_hour_);
            // Release mutex before calling cmd_set_time() to avoid deadlock
            needs_catchup = true;
        }
    } else {
        // Slot not found this minute — normal advance
        displayed_minute_ = new_min;
        displayed_hour_   = new_hour;
        ConfigStore::save_disp_position(displayed_hour_, displayed_minute_);

        // If we just crossed :00 without seeing the slot, mark slow case active
        if (at_top_of_hour && !past_hour_) {
            past_hour_ = true;
            ESP_LOGI(TAG, "Crossed :00 without slot — slow correction mode active");
        }
        EventLog::log(LogCat::CLOCK_TICK, "Tick → %02d:%02d", new_hour, new_min);
    }

    xSemaphoreGive(mutex_);

    // Slow case catch-up: advance hands to current real time (runs outside mutex)
    if (needs_catchup) {
        ESP_LOGI(TAG, "Slow case: fast-forwarding hands to current time");
        cmd_set_time(-1, -1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sensor scan helpers
// ─────────────────────────────────────────────────────────────────────────────

bool ClockManager::scan_full(int &trigger_step)
{
    const int POLL_EVERY = 10;
    const int MAX_STEPS  = STEPS_PER_CLOCK_MINUTE;

    trigger_step = 0;
    bool found   = false;

    sensor_.led_on();
    for (int s = 0; s < MAX_STEPS; s += POLL_EVERY) {
        motor_.microstep_n(POLL_EVERY, StepDirection::FORWARD);
        int raw = sensor_.read_raw();
        last_sensor_adc_ = raw;
        if (!found && raw > sensor_.get_threshold()) {
            found        = true;
            trigger_step = s + POLL_EVERY;  // steps advanced when trigger fired
        }
    }
    sensor_.led_off();
    return found;
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
    bool first_sync = !time_valid_;
    time_valid_ = true;
    ESP_LOGI(TAG, "Time synced. Current time: %s", time_full().c_str());

    if (first_sync) {
        if (suppress_align_) {
            // First-time setup: hand position is unknown (stale NVS).
            // Time is valid for display purposes; motor stays put.
            ESP_LOGI(TAG, "First sync — hand alignment suppressed (first-time setup)");
            EventLog::log(LogCat::CLOCK_STARTUP, "SNTP first sync (align suppressed)");
        } else if (displayed_hour_ >= 0 && displayed_minute_ >= 0 && task_handle_) {
            // Normal boot: advance hands from NVS-restored position to real time.
            xTaskNotify(task_handle_, 1U, eSetValueWithOverwrite);
            ESP_LOGI(TAG, "First sync — notified clock_task to align hands (displayed=%02d:%02d)",
                     displayed_hour_, displayed_minute_);
            EventLog::log(LogCat::CLOCK_STARTUP,
                "SNTP first sync — aligning from %02d:%02d",
                displayed_hour_, displayed_minute_);
        } else {
            EventLog::log(LogCat::CLOCK_STARTUP, "SNTP first sync (pos unknown)");
        }
    } else {
        // Periodic SNTP re-sync: the system clock is already corrected automatically.
        // Do NOT move the motor — the displayed position is only an estimate and
        // the sensor scan handles hand correction on every minute tick.
        ESP_LOGI(TAG, "Periodic SNTP re-sync — system clock updated, no hand movement");
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
    EventLog::log(LogCat::CLOCK_SET, "Set: %+d min → %02d:%02d",
                  delta, displayed_hour_, displayed_minute_);
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
    // Search up to 60 full clock-minutes of travel.
    const int MAX_STEPS    = STEPS_PER_CLOCK_MINUTE * 60;
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
        ESP_LOGW(TAG, "cmd_start_offset_cal: slot not found within %d steps (60 min)", MAX_STEPS);
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
    // Clear the stored position so the tick task doesn't immediately re-trigger
    // a large realignment move based on the now-stale pre-cancel position.
    // The user must re-run Set Time to re-establish the hand position.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    displayed_hour_   = -1;
    displayed_minute_ = -1;
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "cmd_cancel_move: cancelled — hand position cleared");
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
