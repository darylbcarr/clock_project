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
 *   help                    List all commands
 */

#include "clock_manager.h"
#include "networking.h"
#include "encoder.h"
#include "freertos/semphr.h"

/**
 * @brief Start the UART command shell task.
 * @param clock_mgr   Pointer to the shared ClockManager.
 * @param net         Pointer to the shared Networking instance.
 * @param encoder     Pointer to the shared RotaryEncoder (for enc-test).
 * @param bus_mutex   The I2C bus mutex owned by Display.
 */
void console_start(ClockManager*     clock_mgr,
                   Networking*       net,
                   RotaryEncoder*    encoder,
                   SemaphoreHandle_t bus_mutex);
