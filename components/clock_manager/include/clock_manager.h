#pragma once

/**
 * @file clock_manager.h
 * @brief Clock management: time sync, hand positioning, sensor calibration
 */

#include <ctime>
#include <string>
#include <functional>
#include "stepper_motor.h"
#include "position_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ── Clock configuration constants ────────────────────────────────────────────

/// NTP server list (primary, fallback)
static constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
static constexpr const char* NTP_SERVER_2 = "time.google.com";

/// Default timezone string (POSIX TZ).  Updated by networking component.
static constexpr const char* DEFAULT_TZ = "UTC0";

/// Maximum minutes of drift the sensor correction will auto-correct.
/// scan_full() only runs within this many minutes of the top of the hour.
static constexpr int MAX_AUTO_CORRECT_MINUTES = 5;

// ── Calibration phase ────────────────────────────────────────────────────────
enum class CalPhase {
    IDLE,            ///< Not calibrating
    MOVING_TO_SLOT,  ///< Motor advancing to find sensor trigger
    SLOT_FOUND,      ///< Slot found; user should step hand to 12:00 then save
    NOT_FOUND,       ///< Search exhausted without finding slot
};

// ── ClockManager class ────────────────────────────────────────────────────────
class ClockManager {
public:
    explicit ClockManager(uint32_t motor_delay_us = DEFAULT_STEP_DELAY_US);
    ~ClockManager();

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void start();
    void stop();

    // ── Time sync (called by networking component) ────────────────────────────
    void set_timezone(const char* tz);
    void on_time_synced();
    bool is_time_valid() const { return time_valid_; }

    /**
     * @brief Suppress the first-sync hand alignment.
     *        Call before start() on first-time setup, where the stored displayed
     *        position is stale.  SNTP still sets the system clock; the motor will
     *        not move until the user explicitly triggers set-time.
     */
    void suppress_first_sync_align() { suppress_align_ = true; }

    // ── Command interface ─────────────────────────────────────────────────────

    /** Set hands to current real time, moving forward from stored position. */
    void cmd_set_time(int obs_hour = -1, int obs_min = -1);

    /** Inject wall-clock time manually (for testing without network). */
    void cmd_set_manual_time(int hour, int minute, int second = 0);

    /** Print sync / network status report. */
    void cmd_sync_status();

    /**
     * @brief Microstep the motor N half-steps.
     *        During SLOT_FOUND calibration, each call accumulates the step
     *        count in cal_steps_from_trigger_ for later saving.
     */
    void cmd_microstep(int steps, bool forward = true);

    /**
     * @brief Static ambient calibration — does NOT move the motor.
     *        Takes SENSOR_CALIB_SAMPLES ADC readings with LED on, trims
     *        top-25% outliers, and sets the detection threshold.
     *        Called automatically on every boot.
     */
    void calibrate_sensor_static();

    /**
     * @brief Full sweeping calibration — advances ring ~10 minutes while
     *        sampling so ambient light is captured across the full ring arc.
     *        Moves the motor; only call from the web UI or console, never on boot.
     */
    void cmd_calibrate_sensor_safe();

    /**
     * @brief Begin guided offset calibration.
     *        Suspends the clock tick, then steps the motor forward until the
     *        sensor triggers (up to 2 full clock-minutes of travel).
     *        On trigger: sets cal_phase_ = SLOT_FOUND and returns.
     *        On timeout:  sets cal_phase_ = NOT_FOUND  and returns.
     *        While in SLOT_FOUND, use cmd_microstep() to align the hand to
     *        12:00, then call cmd_finish_offset_cal() to save.
     */
    void cmd_start_offset_cal();

    /**
     * @brief Save the accumulated step count as the sensor offset and resume
     *        normal clock operation.  Only valid when cal_phase_ == SLOT_FOUND.
     */
    void cmd_finish_offset_cal();

    /**
     * @brief Cancel offset calibration without saving.  Resumes clock tick.
     */
    void cmd_abort_offset_cal();

    /** Force one-minute motor advance. */
    void cmd_test_advance();

    /** Force one-minute motor reverse. */
    void cmd_test_reverse();

    /**
     * @brief Live sensor readings with LED on.
     *        Prints n ADC values (200 ms apart) so you can manually sweep
     *        the ring through the slot and see whether the reading spikes.
     * @param n  Number of samples (default 30, ~6 seconds).
     */
    void cmd_sensor_read(int n = 30);

    /**
     * @brief Take a single fresh ADC reading (LED on briefly) and store it
     *        in last_sensor_adc_.  Used by the web UI "Read Now" button.
     */
    void cmd_sensor_read_single();

    /**
     * @brief Motor-driven sensor scan.
     *        Advances the ring the requested number of minutes while printing
     *        the ADC reading every ~200 steps.  Results are also stored in
     *        scan_buf_ for retrieval via the web UI.
     * @param minutes  Minutes of travel to scan (default 20, ~24 seconds).
     */
    void cmd_sensor_scan(int minutes = 20);

    // ── Scan results (filled by cmd_sensor_scan) ──────────────────────────────
    struct ScanEntry { int step; int adc; };
    static constexpr int SCAN_BUF_SIZE = 128;

    /** Number of valid entries in the scan buffer (0 if no scan run yet). */
    int  scan_count()       const { return scan_count_; }
    bool is_scanning()      const { return scanning_; }
    /** Pointer to the scan results array (scan_count() valid entries). */
    const ScanEntry* scan_results() const { return scan_buf_; }

    /** Request cancellation of an in-progress cmd_set_time() move. */
    void cmd_cancel_move();

    /** Dump current status to log. */
    void cmd_status();

    /** True while cmd_set_time() is driving the motor. */
    bool is_motor_busy() const { return motor_.is_busy(); }

    // ── Time formatting ───────────────────────────────────────────────────────
    std::string format_time(const char* fmt) const;
    std::string time_hms()     const { return format_time("%H:%M:%S"); }
    std::string time_hm()      const { return format_time("%H:%M"); }
    std::string time_12h()     const { return format_time("%I:%M %p"); }
    std::string time_full()    const { return format_time("%c"); }
    std::string time_iso8601() const { return format_time("%Y-%m-%dT%H:%M:%S"); }
    std::string date_long()    const { return format_time("%A, %d %B %Y"); }
    std::string date_short()   const { return format_time("%Y-%m-%d"); }
    struct tm   get_local_tm() const;
    time_t      get_epoch()    const { return time(nullptr); }

    // ── Getters ───────────────────────────────────────────────────────────────
    int       displayed_minute()     const { return displayed_minute_; }
    int       displayed_hour()       const { return displayed_hour_; }
    int       sensor_offset_steps()  const { return sensor_offset_steps_; }
    int       sensor_dark_mean()     const { return sensor_.get_dark_mean(); }
    int       sensor_threshold()     const { return sensor_.get_threshold(); }
    int       last_sensor_adc()      const { return last_sensor_adc_; }
    CalPhase  cal_phase()            const { return cal_phase_; }
    int       cal_steps()            const { return cal_steps_from_trigger_; }
    bool      is_running()           const { return task_handle_ != nullptr; }

    void set_motor_reverse(bool rev)            { motor_.set_reverse(rev); }
    bool is_motor_reverse()             const { return motor_.is_reverse(); }
    void set_displayed_minute(int m)           { displayed_minute_ = m; }
    void set_displayed_hour(int h)             { displayed_hour_ = h; }
    void set_sensor_offset_steps(int steps)    { sensor_offset_steps_ = steps; }
    void set_step_delay_us(uint32_t us)        { motor_.set_step_delay(us); }
    uint32_t get_step_delay_us()        const { return motor_.get_step_delay(); }

private:
    void tick();

    /**
     * @brief Advance ring exactly one clock-minute with LED on, watching for
     *        the reflective slot.
     * @param[out] trigger_step  Step count within the scan when slot first fired.
     *                           Only valid when return value is true.
     * @return true if slot was detected.
     */
    bool scan_full(int &trigger_step);

    void advance_one_minute();
    static void clock_task(void* arg);

    StepperMotor    motor_;
    PositionSensor  sensor_;

    int      displayed_minute_;
    int      displayed_hour_;
    int      sensor_offset_steps_;     ///< Steps from slot trigger to 12:00
    int      last_sensor_adc_ = 0;
    bool     time_valid_;
    bool     running_;
    bool     needs_sntp_sync_ = false;
    bool     suppress_align_  = false; ///< Set by suppress_first_sync_align(); skip first-sync motor move

    // ── Offset calibration state ─────────────────────────────────────────────
    volatile bool cal_mode_ = false;   ///< Prevents tick() from running
    CalPhase      cal_phase_           = CalPhase::IDLE;
    int           cal_steps_from_trigger_ = 0;

    // ── Per-minute sensor correction state ───────────────────────────────────
    /// Fast case: slot found before :00, backward correction deferred.
    bool correction_pending_ = false;
    /// Accumulated steps past 12:00 to reverse at the :00 tick.
    int  correction_accum_   = 0;
    /// Slow case: :00 tick passed without finding slot; correct forward when found.
    bool past_hour_          = false;

    // ── Scan results buffer ───────────────────────────────────────────────────
    ScanEntry scan_buf_[SCAN_BUF_SIZE] = {};
    int       scan_count_ = 0;
    bool      scanning_   = false;

    TaskHandle_t      task_handle_;
    SemaphoreHandle_t mutex_;
};
