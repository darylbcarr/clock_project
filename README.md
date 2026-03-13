# Analog Clock Driver — ESP32-S3 (ESP-IDF v5.4.1)

A stepper-motor-driven analog clock with SNTP time sync, optical position sensing, and automatic drift correction.

---

## Hardware

| Signal        | ESP32-S3 GPIO | Notes                                   |
|---------------|---------------|-----------------------------------------|
| Stepper IN1   | GPIO 16       | ULN2003 → 28BYJ-48                      |
| Stepper IN2   | GPIO 15       |                                         |
| Stepper IN3   | GPIO 7        |                                         |
| Stepper IN4   | GPIO 6        |                                         |
| LED (anode)   | GPIO 13       | 330 Ω series resistor                   |
| LDR (ADC in)  | GPIO 14       | 10 kΩ pull-down to GND (voltage divider)|

### Motor notes
- **28BYJ-48** (5 V) driven through **ULN2003** at 3.3 V logic (ULN2003 accepts 3.3 V inputs).
- **Half-step mode** (8 phases) is used: smoother motion, quieter, better fine positioning.
- **Steps per output revolution**: 4096 half-steps (64 gear ratio × 64 half-steps/electrical revolution).
- Coils are **de-energised** between moves to eliminate holding hum and reduce heat.
- Default step delay: **2000 µs/step** → ~8 s/revolution.  Reduce to ~1200 µs for faster moves at the cost of more noise.

---

## Project Structure

```
clock_project/
├── CMakeLists.txt
├── sdkconfig.defaults
├── components/
│   ├── stepper_motor/           # 28BYJ-48 half-step driver
│   │   ├── CMakeLists.txt
│   │   ├── include/stepper_motor.h
│   │   └── stepper_motor.cpp
│   ├── clock_manager/           # Clock logic, sensor, time formatting
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   ├── clock_manager.h
│   │   │   └── position_sensor.h
│   │   ├── clock_manager.cpp
│   │   └── position_sensor.cpp
│   └── networking/              # WiFi + SNTP stub (implement later)
│       ├── CMakeLists.txt
│       ├── include/networking.h
│       └── networking.cpp
└── main/
    ├── CMakeLists.txt
    ├── main.cpp
    ├── console_commands.h
    └── console_commands.cpp
```

---

## Component Reference

### `stepper_motor`

Self-contained half-step driver.  No FreeRTOS task — all calls are synchronous.

```cpp
StepperMotor motor(2000);           // 2000 µs/step

motor.move_steps(512, StepDirection::FORWARD);
motor.move_revolutions(0.5f);       // half a revolution forward
motor.move_clock_minutes(3);        // 3 gear-minutes forward
motor.microstep_n(16, StepDirection::BACKWARD);
motor.set_step_delay(1500);         // speed up
motor.power_off();                  // de-energise (automatic after move)
```

**Gear ratio calibration**: Set `MOTOR_REVS_PER_CLOCK_MINUTE` in `stepper_motor.h` to match your physical gear train.  Default is `1.0` (one motor revolution = one clock minute).

---

### `position_sensor`

LED + LDR pair on GPIO 13 / 14.

```cpp
PositionSensor sensor;

sensor.calibrate_dark();            // measure baseline, set threshold
int avg = sensor.read_average(64);  // raw ADC average
bool hit = sensor.is_triggered();   // single shot check
sensor.wait_for_trigger(5000);      // block up to 5 s
sensor.set_threshold(1800);         // manual override
```

**Calibration workflow**:
1. Position the clock so the minute hand is **away** from the sensor.
2. Call `calibrate_dark()` — LED fires 64 samples, sets threshold = mean + 200 ADC counts.
3. Manually pass the hand over the sensor and verify `is_triggered()` returns `true`.
4. Fine-tune with `set_threshold()` if needed.

---

### `clock_manager`

Orchestrates motor, sensor, SNTP, and time formatting.

#### Background task
Started by `start()`.  Wakes once per minute (aligned to wall-clock minute boundaries).  Each tick:
1. Advances the motor one minute.
2. Near the top of hour (±30 s window), checks the sensor.
3. If triggered and drift > 0 minutes, auto-corrects up to ±5 minutes.

#### Time formatting
```cpp
clock_mgr.time_hms()       // "14:35:22"
clock_mgr.time_12h()       // "02:35 PM"
clock_mgr.time_iso8601()   // "2026-03-12T14:35:22"
clock_mgr.date_long()      // "Thursday, 12 March 2026"
clock_mgr.format_time("%A %I:%M %p")  // any strftime format
```

---

### `networking` (stub)

Placeholder component.  Interface already wired to `ClockManager`:
- `set_wifi_credentials(ssid, password)`
- `set_timezone_override(posix_tz_string)` — bypasses geolocation
- `begin()` — starts async connection chain

When fully implemented it should:
1. Connect WiFi.
2. Determine timezone (NVS config or IP-geolocation API).
3. Call `clock_mgr.set_timezone(tz)`.
4. Start SNTP; call `clock_mgr.on_time_synced()` in the sync callback.

---

## UART Console Commands

Connect at **115200 baud** (default UART0).  Prompt: `clock> `

| Command | Args | Description |
|---------|------|-------------|
| `calibrate` | — | Measure dark baseline, set sensor threshold |
| `measure` | — | Print average ADC reading (LED on, no hand) |
| `set-offset <s>` | seconds | Distance from sensor trigger to top-of-hour |
| `set-time [<min>]` | current minute 0-59 | Move hand to match SNTP time |
| `microstep <n> [fwd\|bwd]` | steps, direction | Fine-adjust hand position |
| `advance` | — | Force one test minute advance |
| `status` | — | Dump full system state |
| `time [<fmt>]` | strftime format | Print current time |
| `help` | — | List all commands |

---

## Quick-Start Commissioning

```
clock> calibrate
# Dark mean = 420  |  Threshold set to 620

clock> measure
# avg=415  (confirm hand not over sensor)

# Manually rotate the hand to the sensor position and note the trigger:
clock> measure
# avg=1850  (hand is over sensor, threshold=620 → triggered)

# Determine how many seconds before the actual hour the sensor fires.
# Example: sensor fires at 00:59:47 → 13 seconds before the hour.
clock> set-offset 13

# Set the hand to the current minute (say the hand is showing minute 22):
clock> set-time 22

clock> status
# Shows full system state

# Test motor advance without waiting a full minute:
clock> advance
clock> advance
```

---

## Configuration Constants

| Location | Constant | Default | Purpose |
|----------|----------|---------|---------|
| `stepper_motor.h` | `DEFAULT_STEP_DELAY_US` | 2000 | µs between half-steps |
| `stepper_motor.h` | `MOTOR_REVS_PER_CLOCK_MINUTE` | 1.0 | Gear ratio calibration |
| `position_sensor.h` | `SENSOR_CALIB_SAMPLES` | 64 | Calibration sample count |
| `position_sensor.h` | `SENSOR_THRESHOLD_MARGIN` | 200 | ADC counts above dark mean |
| `clock_manager.h` | `SENSOR_WINDOW_SECONDS` | 30 | ±seconds around hour to check sensor |
| `clock_manager.h` | `MAX_AUTO_CORRECT_MINUTES` | 5 | Max automatic drift correction |
| `main.cpp` | `MOTOR_STEP_DELAY_US` | 2000 | Passed to ClockManager |
| `main.cpp` | `TZ_OVERRIDE` | CST6CDT… | POSIX timezone string |

---

## Building

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

cd clock_project
idf.py set-target esp32s3
idf.py menuconfig   # optional: adjust log level, UART, etc.
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Noise Reduction Tips

1. **Increase `DEFAULT_STEP_DELAY_US`** to 2500–3000 for very quiet operation (slower movement).
2. **Power off between moves** — already done automatically; coils draw ~160 mA each when energised.
3. **Mount the motor on foam** or rubber grommets to isolate vibration.
4. **Half-step mode** (already used) is inherently quieter than full-step due to smoother torque transitions.
5. For near-silent operation consider slowing the move and accepting that the hand takes 10–15 s to travel one minute mark.
