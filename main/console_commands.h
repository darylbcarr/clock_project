#pragma once

/**
 * @file console_commands.h
 * @brief Simple UART command shell for the clock project
 *
 * Uses only the built-in UART driver — no esp_console or argtable3 managed
 * components required.
 *
 * Commands (type over UART at 115200 baud):
 *
 *   calibrate
 *       Measure dark-baseline ADC value and set detection threshold.
 *
 *   measure
 *       Print the average sensor ADC reading (LED on, no hand).
 *
 *   set-offset <seconds>
 *       Record distance (seconds) between sensor trigger and top-of-hour.
 *       Positive = sensor fires N seconds before the hour.
 *
 *   set-time [<current_minute>]
 *       Move hands to match current SNTP time.
 *       <current_minute> 0-59: what the hand shows right now.
 *
 *   microstep <steps> [fwd|bwd]
 *       Fine-adjust hand by N half-steps (default direction: fwd).
 *
 *   advance
 *       Force one test minute advance without waiting 60 seconds.
 *
 *   status
 *       Print current state of the clock manager.
 *
 *   time [<fmt>]
 *       Print current time using optional strftime format string.
 *       Default: "%Y-%m-%dT%H:%M:%S"
 *
 *   help
 *       List all commands.
 */

#include "clock_manager.h"

/**
 * @brief Start the UART command shell task.
 *        Spawns a FreeRTOS task that reads lines from UART0 and dispatches
 *        commands to the ClockManager.  Call once from app_main.
 * @param clock_mgr  Pointer to the shared ClockManager instance.
 */
void console_start(ClockManager* clock_mgr);
