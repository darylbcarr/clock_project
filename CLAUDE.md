# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 firmware (ESP-IDF v5.4.1, C++17) for a stepper-motor-driven analog clock with SNTP time sync, optical position sensing, SSD1306 OLED display, and a rotary encoder menu.

## Build Commands

`build.sh` wraps all ESP-IDF environment setup — use it instead of calling `idf.py` directly.

```bash
# Build
./build.sh build

# Flash and open monitor
./build.sh -p /dev/ttyUSB0 flash monitor

# Menuconfig
./build.sh menuconfig

# Open serial monitor without flashing
idf.py -p /dev/ttyUSB0 monitor
```

There are no automated tests — validation is done via the UART console at 115200 baud (prompt: `clock> `).

## Architecture

### Component Dependency Graph

```
main
 ├── clock_manager   (ClockManager, PositionSensor)
 ├── networking      (Networking → calls ClockManager::set_timezone / on_time_synced)
 ├── display         (Display — owns the I2C bus handle + mutex)
 ├── encoder         (SeesawDevice + RotaryEncoder — shares Display's bus handle)
 └── menu            (Menu — depends on display, clock_manager, networking)
```

### Boot Sequence (`main/main.cpp`)

1. `Display::init()` — creates the I2C bus (GPIO8/9), shows splash
2. `display.probe_bus()` — resets I2C bus internal state before adding a second device
3. `SeesawDevice::begin()` + `RotaryEncoder::init()` — registers encoder on same bus handle
4. `Menu::build()` — wires all menu callbacks
5. `Networking::begin()` — async WiFi → geolocation → SNTP chain
6. `console_start()` — starts UART shell task
7. `ClockManager::cmd_calibrate_sensor()` + `ClockManager::start()` — starts minute-tick task
8. `encoder_task` (50 Hz, priority 4) — polls encoder, drives menu navigation
9. `blank_timer_task` (1 Hz, priority 2) — enforces 5-minute display timeout

### I2C Bus Sharing

Display and Encoder share one physical I2C bus (GPIO8/9). `Display` owns the `i2c_master_bus_handle_t` and a `SemaphoreHandle_t` mutex. All callers (encoder reads, display writes) must take `display.getBusMutex()` for the duration of their transaction. The `probe_bus()` call between Display init and Seesaw registration is required to reset `bus->status` inside the ESP-IDF i2c_master driver.

### Key Design Patterns

- **`ClockManager`** runs a FreeRTOS task that wakes once per minute (wall-clock aligned). Near the top of each hour (±`SENSOR_WINDOW_SECONDS`), it checks the optical sensor and auto-corrects up to `MAX_AUTO_CORRECT_MINUTES` minutes of drift.
- **`Networking`** is fully async: WiFi connect → IP geolocation via `ip-api.com` (HTTP, no key) → IANA→POSIX TZ lookup (`tz_lookup.cpp`) → SNTP → calls back into `ClockManager`.
- **`Menu`** info screens block the `encoder_task` stack (intentionally). The `dismiss_fn_` polls the encoder hardware directly under the I2C mutex to detect button release while the task is blocked.
- **`StepperMotor`** is synchronous (no FreeRTOS task). Coils are de-energised automatically after each move. Half-step mode (8 phases), 4096 steps/revolution.

### Configuration Constants

| File | Constant | Purpose |
|------|----------|---------|
| `stepper_motor.h` | `DEFAULT_STEP_DELAY_US` (2000) | µs between half-steps |
| `stepper_motor.h` | `MOTOR_REVS_PER_CLOCK_MINUTE` (1.0) | Gear ratio |
| `clock_manager.h` | `SENSOR_WINDOW_SECONDS` (30) | ±s around hour to check sensor |
| `clock_manager.h` | `MAX_AUTO_CORRECT_MINUTES` (5) | Max auto drift correction |
| `main/main.cpp` | `WIFI_SSID` / `WIFI_PASSWORD` | Credentials (hardcoded) |
| `main/main.cpp` | `TZ_OVERRIDE` | POSIX TZ string; empty = use geolocation |

### Managed Dependencies

- `k0i05/esp_ssd1306` — SSD1306 driver (via IDF Component Manager, `main/idf_component.yml`)
- Encoder uses the Adafruit Seesaw SAMD09 protocol (I2C address 0x36)

## Hardware Pin Map

| Signal | GPIO |
|--------|------|
| Stepper IN1–IN4 | 16, 15, 7, 6 |
| LED (sensor) | 13 |
| LDR (ADC) | 14 (ADC2_CH3 — ADC2 works fine on ESP32-S3 with WiFi active) |
| I2C SDA | 8 |
| I2C SCL | 9 |
| Display (SSD1306) | 0x3C |
| Encoder (Seesaw) | 0x36 |
