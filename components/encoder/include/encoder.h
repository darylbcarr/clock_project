#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ═══════════════════════════════════════════════════════════════
 * SeesawDevice
 *   Registers on an existing i2c_master_bus_handle_t and implements
 *   the Seesaw two-phase register read/write protocol.
 *
 *   Key: call begin() BEFORE any other device uses the bus after
 *   this one's bus handle was created — mirrors how ssd1306_init()
 *   calls i2c_master_probe() before i2c_master_bus_add_device().
 * ═══════════════════════════════════════════════════════════════ */
class SeesawDevice {
public:
    /**
     * Register this device on the bus.
     * Calls i2c_master_probe() first (matching ssd1306_init pattern)
     * to leave bus->status = I2C_STATUS_DONE before add_device.
     */
    esp_err_t begin(i2c_master_bus_handle_t bus_handle, uint8_t addr,
                    uint32_t speed_hz = 100'000);

    ~SeesawDevice();

    esp_err_t write(uint8_t base, uint8_t func,
                    const uint8_t* data = nullptr, size_t len = 0);
    esp_err_t read(uint8_t base, uint8_t func, uint8_t* buf, size_t len);
    esp_err_t read_byte(uint8_t base, uint8_t func, uint8_t& out);
    esp_err_t read_u32be(uint8_t base, uint8_t func, uint32_t& out);

    // Exposed for RotaryEncoder::init() to call i2c_master_probe()
    // after the post-reset delay without adding a separate reprobe() method.
    i2c_master_bus_handle_t bus_handle_ = nullptr;
    uint8_t                 addr_       = 0;

private:
    i2c_master_dev_handle_t dev_ = nullptr;
};

/* ═══════════════════════════════════════════════════════════════
 * RotaryEncoder
 *   High-level API for Adafruit STEMMA QT Rotary Encoder
 *   (product 4991, SAMD09 @ 0x36).
 *
 * Usage:
 *   SeesawDevice  seesaw;
 *   RotaryEncoder encoder(seesaw);
 *   seesaw.begin(bus_handle, 0x36);
 *   encoder.init();
 * ═══════════════════════════════════════════════════════════════ */
class RotaryEncoder {
public:
    explicit RotaryEncoder(SeesawDevice& dev);

    esp_err_t init();           // soft-reset, verify HW ID, config button
    int8_t    read_delta();     // +1 CW, -1 CCW, 0 no change
    bool      button_pressed(); // true while button held (active-LOW GPIO24)

private:
    int32_t       position_raw();
    SeesawDevice& dev_;
    int32_t       last_pos_ = 0;
};