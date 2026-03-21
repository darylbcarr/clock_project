/**
 * @file stepper_motor.cpp
 * @brief 28BYJ-48 half-step driver implementation
 */

#include "stepper_motor.h"

#include <algorithm>
#include <cmath>
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"    // ets_delay_us (used only for sub-1ms microstep delays)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "stepper_motor";

// ── Half-step sequence (8 phases, 4 coils each) ──────────────────────────────
// Each row: {IN1, IN2, IN3, IN4}
// This energises adjacent pairs of coils giving smooth half-step motion.
const uint8_t StepperMotor::half_step_sequence_[HALF_STEP_PHASES][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

// ── GPIO pin array (order matches sequence columns) ──────────────────────────
static constexpr gpio_num_t PINS[4] = {
    STEPPER_PIN_IN1,
    STEPPER_PIN_IN2,
    STEPPER_PIN_IN3,
    STEPPER_PIN_IN4,
};

// ── Constructor / Destructor ─────────────────────────────────────────────────

StepperMotor::StepperMotor(uint32_t step_delay_us)
    : step_delay_us_(std::max(step_delay_us, MIN_STEP_DELAY_US)),
      current_phase_(0),
      total_steps_(0),
      powered_(false)
{
    init_gpio();
    ESP_LOGI(TAG, "Stepper motor initialised (delay=%lu µs)", (unsigned long)step_delay_us_);
}

StepperMotor::~StepperMotor()
{
    power_off();
}

// ── GPIO initialisation ──────────────────────────────────────────────────────

void StepperMotor::init_gpio()
{
    gpio_config_t cfg = {};
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;

    for (gpio_num_t pin : PINS) {
        cfg.pin_bit_mask = (1ULL << pin);
        gpio_config(&cfg);
        gpio_set_level(pin, 0);
    }
}

// ── Phase application ────────────────────────────────────────────────────────

void StepperMotor::apply_phase(int phase_index)
{
    const uint8_t* phase = half_step_sequence_[phase_index & (HALF_STEP_PHASES - 1)];
    for (int i = 0; i < 4; ++i) {
        gpio_set_level(PINS[i], phase[i]);
    }
}

// ── Single step ──────────────────────────────────────────────────────────────
// step_once() only advances the phase and drives the GPIO.
// The inter-step delay is the caller's responsibility so that move_steps()
// can use vTaskDelay (scheduler-friendly) while microstep() can use
// ets_delay_us for the rare case where sub-millisecond timing is needed.

void StepperMotor::step_once(StepDirection direction)
{
    if (reverse_) {
        direction = (direction == StepDirection::FORWARD)
                    ? StepDirection::BACKWARD : StepDirection::FORWARD;
    }
    current_phase_ = (current_phase_ + static_cast<int>(direction) + HALF_STEP_PHASES)
                     % HALF_STEP_PHASES;
    apply_phase(current_phase_);
    ++total_steps_;
}

// ── Public motion interface ──────────────────────────────────────────────────

void StepperMotor::move_steps(int steps, StepDirection direction)
{
    if (steps <= 0) return;

    cancel_requested_    = false;   // clear any stale cancel from before this move
    last_move_cancelled_ = false;
    busy_    = true;
    powered_ = true;
    ESP_LOGD(TAG, "Moving %d steps %s",
             steps, direction == StepDirection::FORWARD ? "FWD" : "BWD");

    // Convert the per-step delay to FreeRTOS ticks (1 tick = 1 ms at HZ=1000).
    // vTaskDelay yields to the scheduler each step, keeping the IDLE task alive
    // and the task watchdog fed.  Minimum is 1 tick (1 ms); the motor runs fine
    // at 1–3 ms/step and is quieter at the slower end.
    const TickType_t delay_ticks = pdMS_TO_TICKS(step_delay_us_ / 1000);
    const TickType_t actual_delay = (delay_ticks > 0) ? delay_ticks : 1;

    for (int i = 0; i < steps; ++i) {
        if (cancel_requested_) {
            last_move_cancelled_ = true;
            ESP_LOGI(TAG, "Move cancelled after %d/%d steps", i, steps);
            break;
        }
        step_once(direction);
        vTaskDelay(actual_delay);   // yields every step — watchdog stays fed
    }

    power_off();
    busy_ = false;
}

void StepperMotor::move_revolutions(float revolutions)
{
    int steps = static_cast<int>(std::roundf(std::fabs(revolutions) * STEPS_PER_REV));
    StepDirection dir = (revolutions >= 0.0f) ? StepDirection::FORWARD
                                               : StepDirection::BACKWARD;
    move_steps(steps, dir);
}

void StepperMotor::move_clock_minutes(int minutes)
{
    if (minutes == 0) return;
    int steps = std::abs(minutes) * STEPS_PER_CLOCK_MINUTE;
    StepDirection dir = (minutes > 0) ? StepDirection::FORWARD
                                      : StepDirection::BACKWARD;
    ESP_LOGI(TAG, "Moving clock %d minute(s) (%d steps)", minutes, steps);
    move_steps(steps, dir);
}

void StepperMotor::microstep(StepDirection direction)
{
    powered_ = true;
    step_once(direction);
    ets_delay_us(step_delay_us_);   // single step only (~2 ms) — safe to busy-wait
    // Do NOT power off — caller decides when to power off to allow rapid
    // successive single microsteps without re-energising each time.
}

void StepperMotor::microstep_n(int count, StepDirection direction)
{
    if (count <= 0) return;
    powered_ = true;
    ESP_LOGD(TAG, "Microstep x%d %s",
             count, direction == StepDirection::FORWARD ? "FWD" : "BWD");

    // Use vTaskDelay for the same reason as move_steps(): count is unbounded
    // (user can pass any value from the console) so ets_delay_us busy-wait
    // will starve the IDLE task and trigger the watchdog on large counts.
    const TickType_t delay_ticks = pdMS_TO_TICKS(step_delay_us_ / 1000);
    const TickType_t actual_delay = (delay_ticks > 0) ? delay_ticks : 1;

    for (int i = 0; i < count; ++i) {
        step_once(direction);
        vTaskDelay(actual_delay);
    }
    power_off();
}

// ── Configuration ────────────────────────────────────────────────────────────

void StepperMotor::set_step_delay(uint32_t delay_us)
{
    step_delay_us_ = std::max(delay_us, MIN_STEP_DELAY_US);
    ESP_LOGI(TAG, "Step delay set to %lu µs", (unsigned long)step_delay_us_);
}

// ── Power management ─────────────────────────────────────────────────────────

void StepperMotor::power_off()
{
    for (gpio_num_t pin : PINS) {
        gpio_set_level(pin, 0);
    }
    powered_ = false;
}

void StepperMotor::power_on()
{
    apply_phase(current_phase_);
    powered_ = true;
}
