# Floating Clock — User Manual

This document covers day-to-day use of the analog clock via the web interface and via Matter smart-home integration (Alexa, Apple Home, Google Home).

---

## Finding the Device

After the clock connects to WiFi, it is accessible by either its IP address or its mDNS hostname.

**To find the address:**

On the clock display, navigate to:
> Main Menu → **Status** → **Network**

The screen shows the local IP address (e.g. `192.168.0.42`) and the mDNS hostname (e.g. `clock_a1b2`).

**To open the web interface**, enter one of these in a browser (note these are examples, your will be different):

```
http://192.168.0.42
http://clock_a1b2.local
```

> **Note:** `.local` addresses work on macOS, iOS, Android, and most Linux systems. On Windows they require Bonjour (included with iTunes) or Windows 11's built-in mDNS support.

---

## Web Interface Overview

The web interface is a single-page application with six tabs across the top. It maintains a live connection to the clock and updates automatically every second — there is no need to refresh the page.

---

## Clock Tab

Displays the current time and clock status.

| Element | Description |
|---------|-------------|
| **Time** | Current local time (12-hour format) |
| **Date** | Full date |
| **Internet synced** | Green badge when SNTP is active and time is accurate |
| **Timezone** | POSIX timezone string resolved from geolocation |

### Observed Clock Position

Use this when the hands need to be aligned to the correct time — for example after first setup, after a motor stall, or after the clock was powered off for an extended period.

1. Look at the physical clock face and note where the minute hand is pointing.
2. Enter that time in the input field.
3. Press **Set Clock to Actual Time**.
4. The motor advances the hands to the correct current time. This may take several minutes if the hands are significantly behind.
5. Press **Cancel** at any time to stop the movement immediately.

### Fine Tune

| Button | Action |
|--------|--------|
| **1 Min Fwd →** | Advance the hand by exactly one clock-minute |
| **1 Min Back ←** | Reverse the hand by exactly one clock-minute |
| **Step Fwd / Step Back** | Move 8 half-steps at a time for precise positioning |

> **Tip:** Fine Tune is useful after sensor calibration to verify the hand lands precisely on 12:00.

---

## Lights Tab

Controls the two WS2812B LED strips independently: the **Ring** (around the clock face) and the **Base** (underneath the clock).

### Color

A palette of 21 colour presets is shown as swatches. Clicking a swatch applies that colour to the selected strip immediately. The colours are calibrated to match what Alexa's named colours produce when controlled via Matter.

### Brightness

A slider from 0 to 255. Changes take effect as you drag.

### Effects

Each strip can run one of the following effects. The effect is applied to the currently selected colour.

| Effect | Description |
|--------|-------------|
| **Static** | Solid colour at constant brightness |
| **Breathe** | Slow fade in and out |
| **Rainbow** | Cycles through the full colour spectrum |
| **Chase** | A single bright pixel moves around the strip |
| **Sparkle** | Random pixels flash briefly |
| **Comet** | A bright head with a fading tail sweeps around |
| **Wipe** | Fills the strip pixel by pixel, then clears |
| **Off** | Strip is dark |

Click an effect button to apply it. The effect starts immediately and is saved — it will resume after a power cycle.

> **Note:** Select the **Ring** or **Base** button at the top of the card before changing colour, brightness, or effect to target the correct strip.

---

## Config Tab

Persistent settings that survive power cycles.

### Motor Speed

Controls how fast the stepper motor turns, in microseconds per half-step.

- **Lower value** = faster (louder)
- **Higher value** = slower (quieter)
- Default: **2000 µs** (~8 seconds per revolution)
- Recommended quiet range: 2500–3000 µs

Drag the slider to adjust, then press **Save Speed**.

> **Tip:** The slider will not be overwritten while you are dragging it, even though the page updates live every second.

### Motor Direction

If the minute hand moves counter-clockwise, select **Reverse** and press **Save**.

### LED Strip Lengths

Sets the number of LEDs in each strip. Press **Save** after changing.

- **Ring** default: 24 LEDs
- **Base** default: 6 LEDs

### Device Hostname

The mDNS name used to reach the clock on the local network. Default is derived from the MAC address (e.g. `clock_a1b2`). Change it to something memorable (e.g. `office_clock`) and press **Save**. The new address takes effect immediately.

### Timezone Override

Normally the clock detects its timezone automatically via IP geolocation. To force a specific timezone, enter a POSIX TZ string (e.g. `CST6CDT,M3.2.0,M11.1.0` for US Central) and press **Save**. Leave blank to use geolocation.

### Firmware Update (OTA)

| Element | Description |
|---------|-------------|
| **Running** | Currently installed firmware version |
| **Latest** | Most recent version available on GitHub (populated after a check) |
| **Check for Updates** | Queries GitHub for the latest release |
| **Update Now** | Downloads and installs the update; device reboots automatically |
| **Auto Update** | When enabled, the device checks once daily and installs updates automatically |

> **Warning:** Do not power off the clock while an update is in progress. The OLED display shows progress and will indicate when it is safe.

---

## Info Tab

Read-only system status.

| Field | Description |
|-------|-------------|
| **IP Address** | Local network address |
| **Hostname** | mDNS hostname |
| **SSID / RSSI** | WiFi network name and signal strength |
| **Uptime** | Time since last boot |
| **Free Heap** | Available memory (useful for diagnostics) |
| **Firmware** | Running version |
| **Sensor ADC** | Last optical sensor reading |
| **Sensor Threshold** | Calibrated detection threshold |
| **Displayed Position** | Where the clock manager believes the hand is pointing |

---

## Sensor Tab

Tools for calibrating the optical position sensor that keeps the clock hands accurate.

The sensor uses an LED and a light-dependent resistor (LDR) to detect a reflective slot on the clock ring as it passes 12:00 each hour.

### Sample Background

Measures ambient light across multiple ring positions to establish the detection threshold. Run this once during initial setup, and again if lighting conditions change significantly (e.g. moving the clock to a different room).

1. Ensure the minute hand is **not** directly over the sensor slot.
2. Press **Sample Background**.
3. The ring advances approximately 10 minutes while sampling. This takes about 12 seconds.
4. The result shows the measured **Mean** and the calculated **Threshold**.

### Find Slot / Set Offset

Locates the reflective slot and calibrates its position relative to 12:00.

1. Press **Find Slot ▶**.
2. The ring advances until the slot is detected.
3. Use the **◀ Back** and **Fwd ▶** step buttons to position the minute hand exactly at 12:00.
4. Press **Save Offset** when the hand is correctly positioned.

### Read Now

Takes a single sensor reading and displays the raw ADC value. Use this to verify the sensor is detecting the slot when the ring is manually positioned over it.

### Live Scan

Advances the ring a set number of minutes and plots the ADC reading at each step — useful for diagnosing sensor issues or confirming the slot is visible.

---

## Matter / Smart Home Integration

The clock appears on your smart-home network as two **Extended Color Light** devices:

| Device name | Strip |
|-------------|-------|
| Clock Ring  | Ring LED strip |
| Clock Base  | Base LED strip |

These devices are compatible with **Alexa**, **Apple Home**, and **Google Home**.

### What Matter Controls

| Control | Behaviour |
|---------|-----------|
| **On / Off** | Turns the strip on or off |
| **Brightness** | Sets light level (0–100%) |
| **Colour** | Sets the strip to a specific colour by name, hex, or colour picker |
| **Colour Temperature** | Sets warm to cool white |

### Effects and Matter

**Matter commands always use Static mode.** When Alexa or another controller sends a colour or brightness command, the strip switches to a solid, static colour — any effect that was running (Breathe, Rainbow, Chase, etc.) is replaced.

This is intentional: smart-home platforms have no concept of animated effects, and a solid colour is the only state they can reliably monitor and control.

**To use effects:**
- Set the desired effect on the **Lights tab** of the web interface.
- Effects are preserved across power cycles.
- Effects will be replaced the next time a Matter colour or brightness command is received.

**Practical workflow:**

1. Use the **web interface** to choose an effect (e.g. Breathe in warm white).
2. Use **Alexa / Matter** to turn the strip on and off, or to switch to a specific solid colour for a scene.
3. If you want the effect back, return to the web interface and re-select it.

### Colour Accuracy

Alexa sends colours using the XY colour space. The clock converts these to RGB and normalises to full brightness (max channel = 255), then applies the configured brightness level separately. The colour presets on the website **Lights tab** are calibrated to match Alexa's named colours exactly.

To verify what RGB values a specific Alexa colour command produces, check the UART monitor (115200 baud) — the firmware logs each colour change:

```
I MatterBridge: ep0 [XY] → RGB(0,255,0) bri=200
```

### Adding a Second Controller

To add the device to a second smart-home platform (e.g. both Alexa and Apple Home):

1. Navigate to the **Info tab** and find the **Matter** section.
2. Press **Open Commissioning Window**.
3. Scan the QR code or enter the pairing code on the second platform.

Alternatively, from the UART console:
```
clock> matter-open-window
```

---

## Factory Reset

Hold both the **A** and **B** buttons on the rotary encoder for **2 seconds** from any screen.

This clears:
- Saved WiFi credentials
- Matter fabric data
- The `wifi_only` and `matter_commissioned` flags

The device returns to first-time setup on the next boot.

> **Note:** After a factory reset and re-commissioning via Matter, the clock will boot into normal operation. Any previously set LED effects, motor configuration, and sensor calibration are preserved — only network and commissioning data is erased.
