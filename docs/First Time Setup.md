# First Time Setup

This guide walks through configuring the clock after it has been flashed and powered on for the first time.

---

## Prerequisites

WiFi credentials (SSID and password) are compiled into the firmware. Before powering on, confirm the correct network credentials were set in `main/main.cpp` and the firmware was flashed to the device.

---

## 1. Find the Device Address

Once powered on, the clock connects to WiFi automatically. The IP address and mDNS hostname are shown on the OLED display via the menu:

**On the clock display:**

> Main Menu → **Status** → **Network**

The screen will show:
- The local IP address (e.g. `192.168.1.42`)
- The mDNS hostname (e.g. `DNS:clock_a1b2`)

Either can be used to access the web interface.

> **Note:** mDNS (`.local`) addresses work natively on macOS, iOS, and most Linux systems. On Windows, mDNS requires a Bonjour-compatible service (included with iTunes, or available via Windows 11 built-in support).

---

## 2. Open the Web Interface

In a browser, navigate to either:

- `http://192.168.1.42` — using the IP address shown on the display, **or**
- `http://clock_a1b2.local` — using the mDNS hostname

The web interface will open showing the **Clock** page.

---

## 3. Check for Firmware Updates

Navigate to **Config** → **Firmware Update**.

The card shows:
- **Running** — the version currently installed
- **Latest** — populated after checking

**Steps:**
1. Note the **Auto Update** toggle at the bottom of the card. Leave it on **Disabled** for now if you prefer manual control, or set it to **Enabled** to allow automatic daily checks.
2. Press **`Check for Updates`** — the device queries GitHub for the latest release.
3. If a newer version is available, an **`Update Now`** button will appear. Press it and watch the OLED display for progress. The device will reboot automatically when the update is complete and reconnect to WiFi.
4. Reload the browser after reboot and return to the Config page to confirm the **Running** version has changed.

> **Note:** If no update is available, or the device is already on the latest version, no further action is needed here.

---

## 4. Set the Device Hostname

Navigate to **Config** → **Device Hostname**.

The default hostname is derived from the device MAC address (e.g. `clock_a1b2`). To set a more memorable name:

1. Type the desired hostname in the input field (e.g. `living_room_clock`).
2. Press **`Save`**.
3. The change takes effect immediately — no restart required.
4. Reload the browser using the new address: `http://living_room_clock.local`

> **Tip:** Keep the name lowercase with underscores or hyphens. Avoid spaces and special characters.

---

## 5. Verify Motor Direction

The motor direction must be set so that the minute hand advances clockwise.

Navigate to the **Clock** page → **Fine Tune** card.

1. Press **`1 Min Fwd →`** and observe the minute hand.
   - If the hand moves **clockwise** (forward) — the direction is correct. Continue to the next step.
   - If the hand moves **counter-clockwise** (backward) — the direction must be reversed.

2. If the hand moved the wrong way, navigate to **Config** → **Motor Direction** and select **`Reverse`**.

3. Return to the **Clock** page and press **`1 Min Fwd →`** again to confirm the hand now moves clockwise.

---

## 6. Calibrate the Sensor

The optical sensor detects a reflective slot on the clock ring to establish the 12:00 reference position. Calibration is a two-step process.

Navigate to **Config** → **Sensor Calibration**.

### Step 1 — Background Sample

This step measures ambient light across multiple ring positions to set the detection threshold.

1. Press **`Sample Background`**.
2. The ring will advance approximately 10 minutes while the sensor samples. This takes about 12 seconds.
3. When complete, the result is displayed showing the measured mean and threshold values (e.g. `Mean: 412  Threshold: 620`).

> **Important:** Perform this step under normal ambient lighting — the same conditions the clock will operate in. Avoid shining a direct light source at the sensor during sampling.

### Step 2 — Slot Offset

This step finds the reflective slot on the ring and sets its offset relative to 12:00.

1. Press **`Find Slot ▶`**.
2. The ring advances until the sensor detects the reflective slot. The display will show **"Slot found. Step the hand to exactly 12:00, then save."**
3. Use the **`◀ Back`** and **`Fwd ▶`** step buttons to position the minute hand precisely at 12:00 on the clock face.
   - The **Steps from slot** counter updates with each press to track how far you have moved from the trigger point.
4. When the hand is exactly at 12:00, press **`Save Offset`**.

> **If the slot is not found:** Check sensor wiring and ensure the background threshold was sampled correctly (Step 1). Press **`Try Again`** to retry.

---

## 7. Set the Clock Time

With the motor direction confirmed and sensor calibrated, set the clock to the correct time.

Navigate to the **Clock** page → **Observed Clock Position** card.

1. Look at the physical clock face and note the time shown by the hands.
2. Enter that time in the time input field.
3. Press **`Set Clock to Actual Time`**.
4. The hands will advance to the correct current time. A progress indicator is shown during movement.
5. Press **`Cancel`** at any time if you need to abort the operation.

> **Note:** Once the clock has an internet time sync (shown by the green **● Internet synced** badge at the top of the Clock page), the displayed time is accurate. The "Observed Clock Position" only needs to be set once — the clock will maintain correct time automatically thereafter via SNTP.

---

## Setup Complete

The clock is now configured. Going forward:

- Time is kept accurate automatically via internet time sync (SNTP).
- The optical sensor performs an automatic drift correction near the top of each hour.
- The web interface is accessible at `http://<hostname>.local` from any device on the same network.
