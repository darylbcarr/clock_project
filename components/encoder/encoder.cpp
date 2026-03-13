#include "encoder.h"
#include "esp_log.h"
#include "esp_check.h"
#include <cstring>

static const char* TAG = "encoder";

/* Seesaw register map (product 4991, SAMD09) */
static constexpr uint8_t SEESAW_STATUS_BASE    = 0x00;
static constexpr uint8_t SEESAW_STATUS_HW_ID   = 0x01;
static constexpr uint8_t SEESAW_STATUS_SWRST   = 0x7F;

static constexpr uint8_t SEESAW_GPIO_BASE      = 0x01;
static constexpr uint8_t SEESAW_GPIO_DIRSET    = 0x02;
static constexpr uint8_t SEESAW_GPIO_DIRCLR    = 0x03;
static constexpr uint8_t SEESAW_GPIO_BULK      = 0x04;
static constexpr uint8_t SEESAW_GPIO_BULK_SET  = 0x02;
static constexpr uint8_t SEESAW_GPIO_PULLENSET = 0x0B;

static constexpr uint8_t SEESAW_ENCODER_BASE     = 0x11;
static constexpr uint8_t SEESAW_ENCODER_POSITION = 0x30;

// Button: GPIO24, active-LOW. Big-endian 32-bit: bit 0 of byte[0].
static constexpr uint32_t ENCODER_BUTTON_MASK = (1UL << 24);

// SAMD09 needs 5ms between register-select write and data read.
static constexpr int SEESAW_READ_DELAY_MS = 5;

/* ═══════════════════════════════════════════════════════════════
 * SeesawDevice
 * ═══════════════════════════════════════════════════════════════ */
esp_err_t SeesawDevice::begin(i2c_master_bus_handle_t bus_handle,
                               uint8_t addr, uint32_t speed_hz)
{
    bus_handle_ = bus_handle;
    addr_       = addr;

    // Probe BEFORE add_device — sets bus->status = I2C_STATUS_DONE.
    // wait_all_done() drains any pending DMA before add_device.
    ESP_RETURN_ON_ERROR(
        i2c_master_probe(bus_handle_, addr_, 500),
        TAG, "Seesaw not found on bus");
    i2c_master_bus_wait_all_done(bus_handle_, 100);
    vTaskDelay(pdMS_TO_TICKS(10));

    i2c_device_config_t dev_cfg;
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr_;
    dev_cfg.scl_speed_hz    = speed_hz;

    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus_handle_, &dev_cfg, &dev_),
        TAG, "i2c_master_bus_add_device failed");

    ESP_LOGI(TAG, "Seesaw registered  addr=0x%02X  %lu Hz",
             addr_, (unsigned long)speed_hz);
    return ESP_OK;
}

SeesawDevice::~SeesawDevice()
{
    if (dev_) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t SeesawDevice::write(uint8_t base, uint8_t func,
                               const uint8_t* data, size_t len)
{
    uint8_t buf[34];
    buf[0] = base;
    buf[1] = func;
    if (data && len) memcpy(buf + 2, data, len);
    return i2c_master_transmit(dev_, buf, 2 + len, 500);
}

esp_err_t SeesawDevice::read(uint8_t base, uint8_t func,
                              uint8_t* buf, size_t len)
{
    // Two-phase: write register address, delay 5ms, read response.
    // STOP + START required between write and read (not repeated-start).
    uint8_t reg[2] = { base, func };
    esp_err_t ret = i2c_master_transmit(dev_, reg, 2, 500);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reg select [%02X:%02X] failed: %s",
                 base, func, esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(SEESAW_READ_DELAY_MS));
    ret = i2c_master_receive(dev_, buf, len, 500);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "data read [%02X:%02X] failed: %s",
                 base, func, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t SeesawDevice::read_byte(uint8_t base, uint8_t func, uint8_t& out)
{
    return read(base, func, &out, 1);
}

esp_err_t SeesawDevice::read_u32be(uint8_t base, uint8_t func, uint32_t& out)
{
    uint8_t b[4] = {};
    esp_err_t ret = read(base, func, b, 4);
    if (ret == ESP_OK)
        out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
              ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
    return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * RotaryEncoder
 * ═══════════════════════════════════════════════════════════════ */
RotaryEncoder::RotaryEncoder(SeesawDevice& dev) : dev_(dev) {}

esp_err_t RotaryEncoder::init()
{
    uint8_t rst = 0xFF;
    ESP_RETURN_ON_ERROR(
        dev_.write(SEESAW_STATUS_BASE, SEESAW_STATUS_SWRST, &rst, 1),
        TAG, "soft reset failed");

    // Wait for SAMD09 to complete its reset.  500ms is what the Adafruit
    // Arduino library uses; the chip is unresponsive during this window.
    vTaskDelay(pdMS_TO_TICKS(500));

    // Drain any pending bus transactions — do NOT call i2c_master_probe()
    // here.  Probing after add_device has been called corrupts bus->status
    // on IDF v5.x and causes all subsequent dev_ transactions to fail.
    // i2c_master_bus_wait_all_done() is the correct way to idle the bus.
    i2c_master_bus_wait_all_done(dev_.bus_handle_, 100);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t hw_id = 0;
    ESP_RETURN_ON_ERROR(
        dev_.read_byte(SEESAW_STATUS_BASE, SEESAW_STATUS_HW_ID, hw_id),
        TAG, "HW ID read failed");
    ESP_LOGI(TAG, "Seesaw HW ID: 0x%02X%s", hw_id,
             hw_id == 0x55 ? " (OK)" : " (unexpected)");

    // Configure GPIO24 as INPUT_PULLUP.
    // Sequence must match Arduino Seesaw library: DIRSET->BULK_SET->DIRCLR->PULLENSET
    const uint8_t pin24[4] = { 0x01, 0x00, 0x00, 0x00 };
    dev_.write(SEESAW_GPIO_BASE, SEESAW_GPIO_DIRSET,    pin24, 4);
    vTaskDelay(pdMS_TO_TICKS(10));
    dev_.write(SEESAW_GPIO_BASE, SEESAW_GPIO_BULK_SET,  pin24, 4);
    vTaskDelay(pdMS_TO_TICKS(10));
    dev_.write(SEESAW_GPIO_BASE, SEESAW_GPIO_DIRCLR,    pin24, 4);
    vTaskDelay(pdMS_TO_TICKS(10));
    dev_.write(SEESAW_GPIO_BASE, SEESAW_GPIO_PULLENSET, pin24, 4);
    vTaskDelay(pdMS_TO_TICKS(10));

    last_pos_ = position_raw();
    ESP_LOGI(TAG, "Encoder ready, initial pos=%ld", (long)last_pos_);
    return ESP_OK;
}

int8_t RotaryEncoder::read_delta()
{
    int32_t cur = position_raw();
    int32_t d   = cur - last_pos_;
    last_pos_   = cur;
    if (d > 0) return  1;
    if (d < 0) return -1;
    return 0;
}

bool RotaryEncoder::button_pressed()
{
    uint32_t gpio = 0xFFFF'FFFF;
    dev_.read_u32be(SEESAW_GPIO_BASE, SEESAW_GPIO_BULK, gpio);
    return !(gpio & ENCODER_BUTTON_MASK);
}

int32_t RotaryEncoder::position_raw()
{
    uint32_t raw = 0;
    dev_.read_u32be(SEESAW_ENCODER_BASE, SEESAW_ENCODER_POSITION, raw);
    return static_cast<int32_t>(raw);
}