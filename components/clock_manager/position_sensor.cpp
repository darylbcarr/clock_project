/**
 * @file position_sensor.cpp
 * @brief LED + LDR position sensor implementation
 */

#include "position_sensor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char* TAG = "position_sensor";

// ── Constructor / Destructor ─────────────────────────────────────────────────

PositionSensor::PositionSensor()
    : adc_handle_(nullptr),
      threshold_(2048),   // safe default until calibrated
      dark_mean_(0),
      led_state_(false)
{
    init_gpio();
    init_adc();
    ESP_LOGI(TAG, "Position sensor initialised");
}

PositionSensor::~PositionSensor()
{
    led_off();
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
    }
}

// ── GPIO / ADC setup ─────────────────────────────────────────────────────────

void PositionSensor::init_gpio()
{
    gpio_config_t cfg = {};
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask = (1ULL << SENSOR_LED_GPIO);
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(SENSOR_LED_GPIO, 0);
}

void PositionSensor::init_adc()
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id  = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle_));

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten    = ADC_ATTEN_DB_12;   // 0–3.3 V range
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, SENSOR_ADC_CHANNEL, &chan_cfg));
}

// ── LED control ──────────────────────────────────────────────────────────────

void PositionSensor::led_on()
{
    if (!led_state_) {
        gpio_set_level(SENSOR_LED_GPIO, 1);
        led_state_ = true;
        vTaskDelay(pdMS_TO_TICKS(SENSOR_LED_SETTLE_MS));
    }
}

void PositionSensor::led_off()
{
    gpio_set_level(SENSOR_LED_GPIO, 0);
    led_state_ = false;
}

// ── ADC reading ──────────────────────────────────────────────────────────────

int PositionSensor::read_raw()
{
    int value = 0;
    adc_oneshot_read(adc_handle_, SENSOR_ADC_CHANNEL, &value);
    return value;
}

int PositionSensor::read_average(int samples)
{
    if (samples <= 0) samples = 1;
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        sum += read_raw();
        // Small yield every 16 samples
        if ((i & 0x0F) == 0x0F) taskYIELD();
    }
    return static_cast<int>(sum / samples);
}

// ── Calibration ──────────────────────────────────────────────────────────────

int PositionSensor::calibrate_dark()
{
    ESP_LOGI(TAG, "Calibrating dark baseline (%d samples)...", SENSOR_CALIB_SAMPLES);
    led_on();
    int mean = read_average(SENSOR_CALIB_SAMPLES);
    led_off();

    dark_mean_ = mean;
    threshold_ = mean + SENSOR_THRESHOLD_MARGIN;

    ESP_LOGI(TAG, "Dark mean = %d  |  Threshold set to %d", dark_mean_, threshold_);
    return dark_mean_;
}

// ── Detection ────────────────────────────────────────────────────────────────

bool PositionSensor::is_triggered()
{
    led_on();
    int val = read_raw();
    led_off();
    bool triggered = (val > threshold_);
    ESP_LOGD(TAG, "Sensor read=%d  threshold=%d  triggered=%s",
             val, threshold_, triggered ? "YES" : "no");
    return triggered;
}

bool PositionSensor::wait_for_trigger(uint32_t timeout_ms)
{
    int64_t deadline = (timeout_ms > 0)
                       ? esp_timer_get_time() + (int64_t)timeout_ms * 1000LL
                       : INT64_MAX;

    while (esp_timer_get_time() < deadline) {
        if (is_triggered()) {
            ESP_LOGI(TAG, "Trigger detected!");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));  // poll at 50 Hz
    }
    ESP_LOGW(TAG, "wait_for_trigger timed out after %lu ms", (unsigned long)timeout_ms);
    return false;
}
