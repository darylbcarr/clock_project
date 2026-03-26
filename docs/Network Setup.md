# Network Setup

This guide covers connecting the clock to a WiFi network for the first time, and how to reconfigure the network connection later.

The clock supports two connection methods:

| Method | Description |
|--------|-------------|
| **Setup WiFi** | Enter SSID and password directly on the device using the encoder/buttons |
| **Matter** | Commission via a smart home app (Amazon Alexa, Apple Home, Google Home) over BLE |

---

## First-Time Setup

The first-time setup screen appears automatically on first boot when no WiFi credentials or Matter commissioning data are stored. It does not appear on subsequent boots once the device is configured.

### The Choice Screen

The display shows:

```
  First-Time Setup

> Matter
  Setup WiFi

A/B/Rot: pick
LongA/Enc: ok
Enc 5s:WiFiRst
```

**To navigate:**

| Input | Action |
|-------|--------|
| Rotate encoder, tap A, or tap B | Toggle between **Matter** and **Setup WiFi** |
| Long-press A (≥ 800 ms) | Confirm selection |
| Encoder short press | Confirm selection |
| Encoder long press (≥ 800 ms) | Confirm selection |

---

## Option 1 — Setup WiFi

Select **Setup WiFi** and confirm. The device prompts for the network SSID and password using an on-screen character grid.

### Entering Text

The character grid shows all available characters at once. A cursor (`>`) marks the current character.

| Input | Action |
|-------|--------|
| Rotate encoder | Move cursor |
| Tap A | Move cursor forward one step |
| Tap B | Move cursor forward one step |
| Long-press A (≥ 800 ms) | Append selected character |
| Encoder short press | Append selected character (or confirm if cursor is on `>`) |
| Encoder long press (≥ 800 ms) | Backspace — delete last character |
| Long-press B (≥ 800 ms) | Backspace — delete last character |
| Navigate to `>` then tap A/B or press encoder | Confirm and submit |
| Hold A + B (≥ 800 ms) | Cancel — returns to the choice screen |

### SSID Entry

The display shows `SSID:` as the prompt. Enter the exact network name (case-sensitive), then confirm with `>`.

### Password Entry

After the SSID is accepted, the display shows `Password:`. Enter the network password and confirm. Leave blank and confirm immediately for open (no-password) networks.

### Connection

After the password is entered, the display shows:

```
  WiFi Saved!

<your SSID>

Connecting...
```

The device then attempts to connect within 15 seconds.

- **Success** — the device proceeds to the main menu and clock operation. The SSID and password are saved in NVS and used on all subsequent boots.
- **Failure** — the display shows `Connect failed! / Check SSID & / password. / Restarting...` for 3 seconds, clears the stored credentials, and restarts back into first-time setup. Check the SSID and password and try again.

---

## Option 2 — Matter

Select **Matter** and confirm. This starts the Matter stack and begins BLE advertising so the device is discoverable by a smart home app.

> **Note:** Matter commissioning requires a compatible smart home app — Amazon Alexa, Apple Home (iOS/macOS), or Google Home. Open the app on your phone and use the "Add Device" flow.

### The Pairing Screen

The display shows the pairing information needed by the smart home app:

```
 Matter Pairing
Go to Smart Home


Disc: XXXX
<manual code>
```

- **Disc** — the discriminator value used to identify this device during scanning
- The full manual pairing code is shown on the line below

Open your smart home app, choose **Add Device**, and either scan the QR code (if available) or enter the manual code shown on the display.

### Completing Commissioning

The pairing screen stays active while BLE advertising is running. The display exits automatically once commissioning completes and WiFi credentials have been delivered by the app.

**To cancel and return to the choice screen:**

| Input | Action |
|-------|--------|
| Encoder short press | Back to choice screen |
| Tap A (single press) | Back to choice screen |
| Tap B (single press) | Back to choice screen |

Returning to the choice screen does not stop the Matter stack. If you re-select Matter, the existing pairing screen is shown again without restarting BLE.

---

## Returning Device — WiFi Not Found

If the device has stored credentials but fails to connect within 15 seconds (e.g. wrong password, network changed), the display shows a recovery prompt:

```
WiFi not found!
<stored SSID>
Reconfigure:
 A+B 2s/enc 5s
Keep trying:
 btn/enc/30s
```

| Input | Action |
|-------|--------|
| Hold A + B for 2 seconds | Clear stored credentials and restart into first-time setup |
| Hold encoder button for 5 seconds | Clear stored credentials and restart into first-time setup |
| Single button press or 30-second timeout | Keep trying — proceed to the main menu |

---

## Emergency WiFi Reset

At any point — including during the setup screens, while the display is blanked, or in normal operation — the device can be force-reset to clear all WiFi and Matter commissioning data:

| Input | Duration | Result |
|-------|----------|--------|
| Hold A + B | 5 seconds | Display shows `WiFi Reset! / Restarting...` then device restarts |
| Hold encoder button | 5 seconds | Same as above |

After a reset the device restarts and shows the first-time setup screen.

> **Note:** This clears WiFi credentials, Matter fabric data, and ACLs. The device will need to be re-commissioned in any smart home app it was previously paired with.

---

## Changing WiFi Credentials After Setup

To change the stored WiFi network on a configured device, use the emergency reset (hold A+B for 5 seconds). The device will restart into first-time setup where new credentials can be entered.

Alternatively, use the on-device menu:

> Main Menu → **Network** → **Set WiFi**

This prompts for a new SSID and password using the same text-entry interface. The new credentials are saved immediately and used on the next connection attempt.
