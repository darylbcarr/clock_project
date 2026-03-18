#pragma once

/**
 * @file console_commands.h
 * @brief Simple UART command shell for the clock project
 *
 * Commands (115200 baud, UART0):
 *
 *   calibrate               Measure dark baseline, set sensor threshold
 *   measure                 Print average sensor ADC reading
 *   set-offset <s>          Sensor-to-hour offset in seconds
 *   set-clock <HH> <MM>     Manually set system time (bypasses SNTP)
 *   set-time [<minute>]     Move hand to match system time
 *   microstep <n> [fwd|bwd] Fine-adjust hand by N half-steps
 *   advance                 Force one test minute advance
 *   status                  Print clock manager status
 *   sync-status             Show time sync / SNTP state
 *   net-status              Show full network status (IP, geo, RSSI, etc.)
 *   enc-test [n]            Poll encoder n times (default 50) and print raw values
 *   time [<fmt>]            Print current time (strftime format)
 *   matter-pair             Reopen BLE commissioning window (fast advertising)
 *   help                    List all commands
 */

#include "clock_manager.h"
#include "networking.h"
#include "encoder.h"
#include "matter_bridge.h"
#include "ota_manager.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"

/**
 * @brief Start the UART command shell task.
 * @param clock_mgr   Pointer to the shared ClockManager.
 * @param net         Pointer to the shared Networking instance.
 * @param encoder     Pointer to the shared RotaryEncoder (for enc-test).
 * @param matter      Pointer to the shared MatterBridge.
 * @param ota         Pointer to the shared OtaManager.
 * @param bus_mutex   The I2C bus mutex owned by Display.
 * @param bus_handle  The I2C master bus handle (for i2c-scan).
 */
void console_start(ClockManager*           clock_mgr,
                   Networking*             net,
                   RotaryEncoder*          encoder,
                   MatterBridge*           matter,
                   OtaManager*             ota,
                   SemaphoreHandle_t       bus_mutex,
                   i2c_master_bus_handle_t bus_handle);
