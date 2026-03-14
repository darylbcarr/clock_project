#pragma once

/**
 * @file clock_manager.h
 * @brief Clock management: time sync, hand positioning, sensor calibration
 *
 * Responsibilities
 * ────────────────
 * • Obtain real time via SNTP (delegated to networking component later).
 * • Track current displayed time (minute hand position).
 * • Drive the stepper motor once per minute (or on-demand for testing).
 * • Monitor the position sensor to detect the minute hand at the top-of-hour
 *   reference position and correct drift.
 * • Expose command functions used by the CLI / UART console.
 * • Provide time-string formatting helpers.
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

/// How long after top-of-hour the sensor window is open (seconds).
/// The hand must pass the sensor within this window to count as on-time.
static constexpr int SENSOR_WINDOW_SECONDS = 30;

/// Maximum drift correction in minutes before a full re-set is required.
static constexpr int MAX_AUTO_CORRECT_MINUTES = 5;

/// NTP server list (primary, fallback)
static constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
static constexpr const char* NTP_SERVER_2 = "time.google.com";

/// Default timezone string (POSIX TZ).  Updated by networking component.
static constexpr const char* DEFAULT_TZ = "UTC0";

// ── Sensor trigger result ─────────────────────────────────────────────────────
struct SensorTriggerInfo {
    bool     triggered;          ///< Was the sensor tripped this minute?
    int64_t  trigger_time_us;    ///< esp_timer_get_time() at trigger
    int      seconds_from_hour;  ///< Seconds after the hour when triggered
    int      error_seconds;      ///< Positive = hand late; negative = early
};

// ── ClockManager class ────────────────────────────────────────────────────────
class ClockManager {
public:
    /**
     * @brief Construct and initialise hardware.
     * @param motor_delay_us  Per-step delay forwarded to StepperMotor.
     */
    explicit ClockManager(uint32_t motor_delay_us = DEFAULT_STEP_DELAY_US);
    ~ClockManager();

    // ──────────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * @brief Start the background clock tick task.
     *        Call after SNTP / networking is ready.
     */
    void start();

    /**
     * @brief Stop the background task gracefully.
     */
    void stop();

    // ──────────────────────────────────────────────────────────────────────────
    // Time sync (called by networking component when connected)
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * @brief Set the POSIX timezone string.  Must be called before start().
     * @param tz  e.g. "EST5EDT,M3.2.0,M11.1.0"
     */
    void set_timezone(const char* tz);

    /**
     * @brief Callback invoked by networking component once SNTP sync is done.
     *        Stores the synced epoch and re-aligns the displayed minute.
     */
    void on_time_synced();

    /**
     * @brief Returns true once a valid SNTP sync has been obtained.
     */
    bool is_time_valid() const { return time_valid_; }

    // ──────────────────────────────────────────────────────────────────────────
    // Command interface  (used by UART console / CLI)
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * @brief CMD: Set the clock hands to align with current real time.
     *        Moves the motor forward (or backward) the necessary minutes.
     *        The displayed_minute_ counter is updated accordingly.
     *
     * @param current_displayed_minute  What minute the hand is currently
     *        showing (0-59).  -1 = unknown (perform full 60-min sweep from 0).
     */
    void cmd_set_time(int current_displayed_minute = -1);

    /**
     * @brief CMD: Manually inject a wall-clock time without SNTP.
     *        Useful for testing before networking is implemented.
     *        Sets the system RTC via settimeofday() and marks time_valid_ = true
     *        so cmd_set_time() will work normally afterwards.
     *
     * @param hour    Local hour   (0-23)
     * @param minute  Local minute (0-59)
     * @param second  Local second (0-59, default 0)
     */
    void cmd_set_manual_time(int hour, int minute, int second = 0);

    /**
     * @brief CMD: Print a detailed sync / network status report.
     *        Shows time_valid flag, current epoch, SNTP state, and
     *        instructions for manual time entry when network is absent.
     */
    void cmd_sync_status();

    /**
     * @brief CMD: Microstep the motor for fine hand adjustment.
     * @param steps      Number of microsteps (half-steps).
     * @param forward    true = forward, false = backward.
     */
    void cmd_microstep(int steps, bool forward = true);

    /**
     * @brief CMD: Run sensor calibration to find the dark (no-hand) baseline.
     *        Prints the result via ESP_LOGI.
     */
    void cmd_calibrate_sensor();

    /**
     * @brief CMD: Measure average sensor reading with LED on (no hand present).
     *        Returns the mean ADC value.
     */
    int cmd_measure_sensor_average();

    /**
     * @brief CMD: User reports the number of seconds between the sensor
     *        trigger and the actual top-of-hour.  This value is stored and
     *        used to correct future triggers.
     * @param seconds_offset  Positive = sensor fires N seconds before top-of-hour.
     *                        Negative = sensor fires N seconds after.
     */
    void cmd_set_sensor_offset(int seconds_offset);

    /**
     * @brief CMD: Force an immediate motor tick (advance one minute).
     *        Useful for testing without waiting 60 seconds.
     */
    void cmd_test_advance();

    /**
     * @brief CMD: Dump current status to log output.
     */
    void cmd_status();

    // ──────────────────────────────────────────────────────────────────────────
    // Time formatting helpers
    // ──────────────────────────────────────────────────────────────────────────

    /**
     * @brief Format the current local time using a strftime format string.
     * @param fmt  strftime format, e.g. "%H:%M:%S" or "%I:%M %p".
     * @return Formatted string.
     */
    std::string format_time(const char* fmt) const;

    /** @brief  "HH:MM:SS"  (24-hour) */
    std::string time_hms()         const { return format_time("%H:%M:%S"); }

    /** @brief  "HH:MM"  (24-hour, no seconds) */
    std::string time_hm()          const { return format_time("%H:%M"); }

    /** @brief  "hh:MM AM/PM" */
    std::string time_12h()         const { return format_time("%I:%M %p"); }

    /** @brief  "Www Mmm DD HH:MM:SS YYYY" */
    std::string time_full()        const { return format_time("%c"); }

    /** @brief  ISO-8601 "YYYY-MM-DDTHH:MM:SS" */
    std::string time_iso8601()     const { return format_time("%Y-%m-%dT%H:%M:%S"); }

    /** @brief  "Weekday, DD Month YYYY" (human-friendly date) */
    std::string date_long()        const { return format_time("%A, %d %B %Y"); }

    /** @brief  "YYYY-MM-DD" */
    std::string date_short()       const { return format_time("%Y-%m-%d"); }

    /** @brief  Return current struct tm (local time). */
    struct tm   get_local_tm()     const;

    /** @brief  Current Unix epoch (UTC). */
    time_t      get_epoch()        const { return time(nullptr); }

    // ──────────────────────────────────────────────────────────────────────────
    // Getters
    // ──────────────────────────────────────────────────────────────────────────
    int  displayed_minute()   const { return displayed_minute_; }
    int  sensor_offset_sec()  const { return sensor_offset_sec_; }
    bool is_running()         const { return task_handle_ != nullptr; }

    void set_motor_reverse(bool rev) { motor_.set_reverse(rev); }
    bool is_motor_reverse()    const { return motor_.is_reverse(); }

    /** Last ADC value returned by cmd_measure_sensor_average(); 0 until first call. */
    int  last_sensor_adc()     const { return last_sensor_adc_; }

private:
    // ── Internal helpers ────────────────────────────────────────────────────
    void tick();                              ///< Called every minute by the timer task
    void check_sensor_and_correct();         ///< Evaluate sensor near top-of-hour
    void advance_one_minute();               ///< Move motor + update counter
    int  minutes_to_target(int target_min) const; ///< Shortest-path minute delta

    static void clock_task(void* arg);       ///< FreeRTOS task entry

    // ── Hardware ────────────────────────────────────────────────────────────
    StepperMotor    motor_;
    PositionSensor  sensor_;

    // ── State ────────────────────────────────────────────────────────────────
    int     displayed_minute_;   ///< What minute the hand currently shows (0-59)
    int     sensor_offset_sec_;  ///< User-calibrated sensor-to-hour offset (s)
    int     last_sensor_adc_ = 0;
    bool    time_valid_;
    bool    running_;

    // ── Task ────────────────────────────────────────────────────────────────
    TaskHandle_t      task_handle_;
    SemaphoreHandle_t mutex_;
};
