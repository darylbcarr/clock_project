#pragma once

/**
 * @file matter_bridge.h
 * @brief Exposes the two WS2812B strips as Matter Extended Color Light endpoints.
 *
 * Endpoint 1 → Ring (LedManager strip 1)
 * Endpoint 2 → Base (LedManager strip 2)
 *
 * Each endpoint supports:
 *   On/Off cluster        — toggle strip on or off
 *   Level Control cluster — brightness 0-254
 *   Color Control cluster — hue (0-254) + saturation (0-254), HS color mode
 *
 * Required sdkconfig (run `./build.sh menuconfig` → Component Config → CHIP):
 *   CONFIG_BT_ENABLED=y
 *   CONFIG_BT_NIMBLE_ENABLED=y
 *   CONFIG_ESP_MATTER_ENABLE_DATA_MODEL=y
 *   CONFIG_CHIP_ENABLE_WIFI_STATION=n   ← let Networking manage WiFi
 *
 * Commissioning: pair with Apple Home / Google Home / Alexa using the QR code
 * or manual pairing code printed to UART at boot.
 *
 * NOTE: Requires full reflash after adding the fctry partition to partitions.csv.
 */

#include "led_manager.h"
#include "esp_err.h"
#include <esp_matter.h>
#include <app/server/AppDelegate.h>
#include <string>

class MatterBridge : public AppDelegate {
public:
    explicit MatterBridge(LedManager& leds);

    /**
     * @brief Create the Matter node and endpoints.
     *        Call after NVS is initialised and LED strips are started.
     */
    esp_err_t init();

    /**
     * @brief Start the Matter stack (CHIP device layer + BLE commissioning).
     *        Call once, after init().  Non-blocking — stack runs in its own task.
     *
     * @param fresh_commissioning  Pass true only when doing a fresh Matter
     *   commissioning (after clear_matter_commissioning_data()).  Enables the
     *   BLE coex fix that clears the WiFi driver NVS so CHIP stops retrying
     *   WiFi while BLE is advertising.  Pass false (default) on normal reboots
     *   so that stored WiFi credentials are not disturbed.
     */
    esp_err_t start(bool fresh_commissioning = false);

    /**
     * @brief Re-open the BLE commissioning window (fast advertising, 3-minute timeout).
     *        Safe to call on an uncommissioned device that has timed out of fast
     *        advertising.  Prints the pairing codes at INFO level.
     *        Has no effect if the device already has a commissioned fabric.
     */
    esp_err_t open_commissioning_window();

    /** Temporary pairing info returned by open_enhanced_commissioning_window(). */
    struct EcwInfo {
        uint32_t pin;           ///< Randomly-generated 8-digit setup PIN
        uint16_t discriminator; ///< Device discriminator (from NVS / fixed)
        uint32_t timeout_s;     ///< Window duration in seconds
    };

    /**
     * @brief Open an Enhanced Commissioning Window for multi-admin pairing.
     *
     * Generates a random one-time PIN and SPAKE2+ verifier, then opens a
     * commissioning window on the operational network (not BLE).  Works
     * whether or not the device is already commissioned.  A second smart-home
     * platform can use the returned PIN + the device's mDNS name to pair.
     *
     * @param out_info  Filled with the temporary PIN, discriminator, and
     *                  window duration on success.
     * @return ESP_OK on success, ESP_FAIL on crypto/stack error.
     */
    esp_err_t open_enhanced_commissioning_window(EcwInfo& out_info);

    /**
     * @brief Close any open commissioning window immediately.
     *        Safe to call when no window is open (no-op in that case).
     */
    void close_commissioning_window();

    /** Commissioning info for on-device display (PIN + discriminator). */
    struct CommissioningInfo {
        uint32_t pin_code;       ///< Setup PIN (e.g. 20202021 in dev mode)
        uint16_t discriminator;  ///< 12-bit discriminator
    };

    /**
     * @brief Return the current commissioning PIN and discriminator.
     *        Valid after start() is called.
     */
    CommissioningInfo get_commissioning_info() const;

    /**
     * @brief Return the 11-digit manual pairing code string.
     *        Empty until start() is called successfully.
     */
    const std::string& manual_code() const { return manual_code_; }

    /**
     * @brief Return true if the device has at least one commissioned Matter fabric.
     *        Call after start().  Used by main to skip first_time_setup() on reboot
     *        when Matter manages WiFi.
     */
    bool is_commissioned() const;
    uint8_t fabric_count() const;

    /**
     * @brief Disable BLE advertising.
     *        Call after WiFi is connected on a device with no Matter fabric to stop
     *        BLE from competing with WiFi for the radio.  The user can still trigger
     *        commissioning later via open_commissioning_window() from the menu.
     */
    void disable_ble_advertising();

    // ── AppDelegate ──────────────────────────────────────────────────────────
    void OnCommissioningWindowOpened() override;
    void OnCommissioningWindowClosed() override;
    void OnCommissioningSessionEstablishmentStarted() override;
    void OnCommissioningSessionStarted() override;
    void OnCommissioningSessionStopped() override;
    void OnCommissioningSessionEstablishmentError(CHIP_ERROR err) override;

private:
    std::string manual_code_;  ///< Generated by start(), e.g. "01693312333"
    LedManager& leds_;

    // Per-endpoint state mirrored from Matter attributes
    struct EpState {
        uint16_t id         = 0;
        uint8_t  hue        = 0;
        uint8_t  saturation = 0;
        uint16_t color_x    = 0;   ///< CIE 1931 X × 65536  (0x0003)
        uint16_t color_y    = 0;   ///< CIE 1931 Y × 65536  (0x0004)
        uint16_t color_temp = 250; ///< Mireds              (0x0007)
        uint8_t  color_mode = 0;   ///< 0=HS, 1=XY, 2=CT    (0x0008)
        uint8_t  level      = 128;
        bool     on         = true;

        // HomeKit sets saturation=0 whenever brightness hits 0, wiping the
        // colour.  Cache the last non-zero HS pair so we can restore it when
        // brightness rises from 0 back to a non-zero value.
        uint8_t  cached_hue = 0;
        uint8_t  cached_sat = 0;
        bool     has_color_cache = false;
        uint8_t  prev_level = 128;
    };
    EpState ep_[2];   // 0 = Ring, 1 = Base

    // Passed as priv_data when creating each endpoint so the callback knows
    // which strip index to update.
    struct EpCtx {
        MatterBridge* self;
        int           idx;   // 0 = Ring, 1 = Base
    };
    EpCtx ctx_[2];

    // Apply current ep_[idx] state to LedManager
    void apply(int idx);

    static esp_err_t attr_cb(
        esp_matter::attribute::callback_type_t type,
        uint16_t endpoint_id, uint32_t cluster_id,
        uint32_t attribute_id, esp_matter_attr_val_t* val,
        void* priv_data);

    static void event_cb(
        const chip::DeviceLayer::ChipDeviceEvent* event, intptr_t arg);

    static esp_err_t identify_cb(
        esp_matter::identification::callback_type_t type,
        uint16_t endpoint_id, uint8_t effect_id,
        uint8_t effect_variant, void* priv_data);

    // Convert Matter HSV (0-254 each) + brightness level (0-254) to RGB
    static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v,
                           uint8_t& r, uint8_t& g, uint8_t& b);

    // Convert CIE 1931 XY (Matter uint16 = value × 65536) to RGB
    static void xy_to_rgb(uint16_t x, uint16_t y,
                          uint8_t& r, uint8_t& g, uint8_t& b);

    // Convert color temperature (mireds) to RGB white point
    static void ct_to_rgb(uint16_t mireds,
                          uint8_t& r, uint8_t& g, uint8_t& b);
};
