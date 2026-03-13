#pragma once

/**
 * @file position_sensor.h
 * @brief LED + LDR position sensor for minute-hand detection
 *
 * Hardware:
 *   LED  : GPIO13 → 330 Ω → LED anode → cathode → GND
 *   LDR  : GPIO14 → ADC input; LDR between 3V3 and GPIO14,
 *           10 kΩ pull-down to GND (voltage divider)
 *
 * When the minute hand passes over the LED/LDR pair the LDR sees
 * the reflected/transmitted light and the ADC reading rises.
 *
 * Calibration workflow:
 *   1. Call calibrate() to take N samples with LED on to find ambient + lit
 *      baseline.
 *   2. The resulting threshold is stored and used by is_triggered().
 */

#include <cstdint>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

// ── Pin / channel assignments ─────────────────────────────────────────────────
static constexpr gpio_num_t SENSOR_LED_GPIO  = GPIO_NUM_13;
static constexpr adc_channel_t SENSOR_ADC_CHANNEL = ADC_CHANNEL_3; // GPIO14 on ESP32-S3

// ── Default calibration parameters ───────────────────────────────────────────
static constexpr int    SENSOR_CALIB_SAMPLES   = 64;   ///< Samples per calibration run
static constexpr int    SENSOR_THRESHOLD_MARGIN = 200;  ///< ADC counts above dark mean
static constexpr uint32_t SENSOR_LED_SETTLE_MS  = 5;    ///< LED warm-up settle time

// ── PositionSensor class ──────────────────────────────────────────────────────
class PositionSensor {
public:
    PositionSensor();
    ~PositionSensor();

    // ── LED control ──────────────────────────────────────────────────────────
    void led_on();
    void led_off();

    // ── ADC reading ──────────────────────────────────────────────────────────
    /**
     * @brief Read a single raw ADC value (0–4095, 12-bit).
     */
    int read_raw();

    /**
     * @brief Read the average of N raw samples.
     */
    int read_average(int samples = SENSOR_CALIB_SAMPLES);

    // ── Calibration ──────────────────────────────────────────────────────────
    /**
     * @brief Measure the dark (no hand) baseline with LED on.
     *        Sets internal threshold = dark_mean + SENSOR_THRESHOLD_MARGIN.
     * @return Average dark reading.
     */
    int calibrate_dark();

    /**
     * @brief Manually override the detection threshold.
     */
    void set_threshold(int threshold) { threshold_ = threshold; }
    int  get_threshold() const        { return threshold_; }

    int  get_dark_mean() const        { return dark_mean_; }

    // ── Detection ────────────────────────────────────────────────────────────
    /**
     * @brief Returns true when the current ADC reading exceeds the threshold
     *        (hand is reflecting/blocking light onto the LDR).
     *        LED is briefly enabled then disabled automatically.
     */
    bool is_triggered();

    /**
     * @brief Poll continuously for a trigger event (blocking, up to timeout_ms).
     * @param timeout_ms  Maximum wait in milliseconds (0 = no timeout).
     * @return true if triggered within timeout, false if timed out.
     */
    bool wait_for_trigger(uint32_t timeout_ms = 0);

private:
    void init_gpio();
    void init_adc();

    adc_oneshot_unit_handle_t adc_handle_;
    int threshold_;
    int dark_mean_;
    bool led_state_;
};
