#pragma once

/**
 * @file stepper_motor.h
 * @brief 28BYJ-48 stepper motor driver via ULN2003
 *
 * Hardware connections (ESP32-S3):
 *   IN1 -> GPIO16
 *   IN2 -> GPIO15
 *   IN3 -> GPIO7
 *   IN4 -> GPIO6
 *
 * Motor specs:
 *   - Stride angle (full step): 5.625° / 64 internal gear ratio
 *   - Steps per output revolution (half-step): 4096
 *   - Half-step sequence gives smoother motion and better micro-positioning
 *
 * Driving strategy:
 *   - Half-stepping (8-phase) for smoothness and quietness
 *   - Configurable step delay (µs) for speed / noise trade-off
 *   - Power-off between moves to reduce heat and audible hum
 *   - Optional microstep interface for fine hand positioning
 */

#include <cstdint>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ── Pin assignments ──────────────────────────────────────────────────────────
static constexpr gpio_num_t STEPPER_PIN_IN1 = GPIO_NUM_16;
static constexpr gpio_num_t STEPPER_PIN_IN2 = GPIO_NUM_15;
static constexpr gpio_num_t STEPPER_PIN_IN3 = GPIO_NUM_7;
static constexpr gpio_num_t STEPPER_PIN_IN4 = GPIO_NUM_6;

// ── Motor constants ──────────────────────────────────────────────────────────

/// Half-step sequence length
static constexpr int HALF_STEP_PHASES = 8;

/// Steps per full output shaft revolution (half-step mode: 64 * 64 = 4096)
static constexpr int STEPS_PER_REV = 4096;

/// Default inter-step delay in microseconds.
/// ~2 ms/step → ~8 s/rev.  Slower = quieter and more torque.
static constexpr uint32_t DEFAULT_STEP_DELAY_US = 2000;

/// Minimum safe delay (fastest useful speed)
static constexpr uint32_t MIN_STEP_DELAY_US = 900;

// ── Direction ────────────────────────────────────────────────────────────────
enum class StepDirection : int8_t {
    FORWARD  =  1,   ///< Clockwise when viewed from front of clock
    BACKWARD = -1,   ///< Counter-clockwise
};

// ── StepperMotor class ───────────────────────────────────────────────────────
class StepperMotor {
public:
    /**
     * @brief Construct and initialise GPIO pins.
     * @param step_delay_us  Per-step delay in microseconds (default 2000).
     */
    explicit StepperMotor(uint32_t step_delay_us = DEFAULT_STEP_DELAY_US);

    ~StepperMotor();

    // ── Motion ───────────────────────────────────────────────────────────────

    /**
     * @brief Move a given number of steps in the specified direction.
     *        Blocks until complete, then de-energises coils.
     * @param steps      Number of half-steps to move.
     * @param direction  FORWARD or BACKWARD.
     */
    void move_steps(int steps, StepDirection direction = StepDirection::FORWARD);

    /**
     * @brief Rotate by a fractional revolution.
     * @param revolutions  Positive = forward, negative = backward.
     *                     e.g. 1.0 = one full output revolution.
     */
    void move_revolutions(float revolutions);

    /**
     * @brief Move the equivalent of N clock-minutes forward.
     *        One clock-minute = one full motor revolution on the gear train
     *        being driven.  Adjust STEPS_PER_CLOCK_MINUTE as needed for
     *        your actual gear ratio.
     * @param minutes  Number of minute increments (positive = forward).
     */
    void move_clock_minutes(int minutes);

    /**
     * @brief Single microstep forward or backward (one half-step phase).
     *        Useful for fine hand positioning.
     * @param direction  FORWARD or BACKWARD.
     */
    void microstep(StepDirection direction);

    /**
     * @brief Execute a batch of microsteps.
     * @param count      Number of microsteps.
     * @param direction  FORWARD or BACKWARD.
     */
    void microstep_n(int count, StepDirection direction);

    // ── Configuration ────────────────────────────────────────────────────────

    /**
     * @brief Change the per-step delay.  Smaller = faster but noisier.
     * @param delay_us  Microseconds per half-step (min MIN_STEP_DELAY_US).
     */
    void set_step_delay(uint32_t delay_us);

    uint32_t get_step_delay() const { return step_delay_us_; }

    /** @brief Reverse the motor direction without swapping wires. */
    void set_reverse(bool rev) { reverse_ = rev; }
    bool is_reverse()    const { return reverse_; }

    /**
     * @brief Return the cumulative step count since construction (or reset).
     */
    int64_t get_total_steps() const { return total_steps_; }

    /**
     * @brief Reset the cumulative step counter.
     */
    void reset_step_counter() { total_steps_ = 0; }

    // ── Power management ─────────────────────────────────────────────────────

    /**
     * @brief De-energise all coils.  Call when motor does not need to hold
     *        position.  Reduces heat and eliminates holding hum.
     */
    void power_off();

    /**
     * @brief Re-energise coils at the current phase (useful if holding
     *        position is needed before moving again).
     */
    void power_on();

    bool is_powered() const { return powered_; }

private:
    void init_gpio();
    void apply_phase(int phase_index);
    void step_once(StepDirection direction);

    uint32_t step_delay_us_;
    int      current_phase_;   ///< Index into half_step_sequence_ [0..7]
    int64_t  total_steps_;
    bool     powered_;
    bool     reverse_ = false;

    /// Half-step sequence: [IN1, IN2, IN3, IN4]
    static const uint8_t half_step_sequence_[HALF_STEP_PHASES][4];
};

// ── Gear-ratio helper (adjust for your physical clock) ───────────────────────

/// How many motor output revolutions move the minute hand by exactly 1 minute.
/// For a direct-drive 60-tooth / 1-tooth arrangement this would be 1.
/// Set to 1 for now; caller can scale as needed.
static constexpr float MOTOR_REVS_PER_CLOCK_MINUTE = 0.1499f;

/// Derived: steps per clock-minute
static constexpr int STEPS_PER_CLOCK_MINUTE =
    static_cast<int>(STEPS_PER_REV * MOTOR_REVS_PER_CLOCK_MINUTE);
