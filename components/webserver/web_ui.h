#pragma once
static const char WEB_UI_HTML[] = R"html(<!DOCTYPE html>
<html lang="en" data-theme="dark" data-accent="cyan">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Clock Driver</title>
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

/* ── Theme variables ── */
:root {
  --bg:        #0a0a14;
  --surf:      #141422;
  --surf2:     #1c1c2e;
  --border:    rgba(255,255,255,0.08);
  --bh:        rgba(255,255,255,0.15);
  --text:      #e2e8f0;
  --muted:     #64748b;
  --dim:       #94a3b8;
  --ok:        #10b981;
  --warn:      #f59e0b;
  --err:       #ef4444;
  --radius:    12px;
  --radius-sm: 8px;
  --nav-h:     52px;
  --tr:        0.18s ease;
}
html[data-accent="cyan"]   { --a: #00d4ff; --a2: #0099bb; --ag: rgba(0,212,255,0.12); }
html[data-accent="blue"]   { --a: #4f8ef7; --a2: #2563eb; --ag: rgba(79,142,247,0.12); }
html[data-accent="green"]  { --a: #22d3a0; --a2: #059669; --ag: rgba(34,211,160,0.12); }
html[data-accent="orange"] { --a: #fb923c; --a2: #ea580c; --ag: rgba(251,146,60,0.12); }
html[data-accent="rose"]   { --a: #fb7185; --a2: #e11d48; --ag: rgba(251,113,133,0.12); }

html[data-theme="light"] {
  --bg:    #f0f2f8;
  --surf:  #ffffff;
  --surf2: #e8eaf2;
  --border:rgba(0,0,0,0.09);
  --bh:    rgba(0,0,0,0.16);
  --text:  #1e293b;
  --muted: #94a3b8;
  --dim:   #64748b;
}

html, body {
  height: 100%;
  background: var(--bg);
  color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  font-size: 14px;
  line-height: 1.5;
  overflow-x: hidden;
}

::-webkit-scrollbar { width: 5px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: rgba(128,128,128,0.3); border-radius: 3px; }

/* ── Top nav ── */
#topbar {
  position: fixed; top: 0; left: 0; right: 0;
  height: var(--nav-h);
  background: var(--surf);
  border-bottom: 1px solid var(--border);
  display: flex; align-items: center; gap: 4px;
  padding: 0 12px;
  z-index: 100;
}
#topbar .logo { font-weight: 700; color: var(--a); margin-right: auto; font-size: 15px; letter-spacing: .5px; }
.nav-btn {
  padding: 6px 14px; border-radius: var(--radius-sm);
  border: none; background: transparent; color: var(--dim);
  cursor: pointer; font-size: 13px; font-weight: 500;
  transition: color var(--tr), background var(--tr);
}
.nav-btn:hover { color: var(--text); background: var(--ag); }
.nav-btn.active { color: var(--a); background: var(--ag); }

/* theme/accent controls */
#theme-toggle {
  width: 32px; height: 32px; border-radius: 50%; border: 1px solid var(--border);
  background: var(--surf2); color: var(--dim); cursor: pointer;
  font-size: 16px; display: flex; align-items: center; justify-content: center;
  margin-left: 8px; transition: border-color var(--tr), color var(--tr);
}
#theme-toggle:hover { border-color: var(--a); color: var(--a); }
.accent-dots { display: flex; gap: 6px; margin-left: 8px; }
.accent-dot {
  width: 16px; height: 16px; border-radius: 50%; cursor: pointer;
  border: 2px solid transparent; transition: border-color var(--tr), transform var(--tr);
}
.accent-dot:hover { transform: scale(1.2); }
.accent-dot.active { border-color: var(--text); }

/* ── Main content ── */
#main {
  margin-top: var(--nav-h);
  padding: 16px;
  max-width: 640px;
  margin-left: auto; margin-right: auto;
}

/* ── Sections ── */
.section { display: none; flex-direction: column; gap: 12px; }
.section.active { display: flex; }

/* ── Cards ── */
.card {
  background: var(--surf);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 16px;
}
.card-title {
  font-size: 11px; font-weight: 600; letter-spacing: .8px;
  text-transform: uppercase; color: var(--muted);
  margin-bottom: 12px;
}

/* ── Time display ── */
#big-time {
  font-size: 52px; font-weight: 200; letter-spacing: -2px;
  color: var(--a); line-height: 1;
  font-variant-numeric: tabular-nums;
}
#big-date { font-size: 15px; color: var(--dim); margin-top: 4px; }
.sntp-badge {
  display: inline-flex; align-items: center; gap: 5px;
  font-size: 11px; padding: 3px 8px; border-radius: 20px;
  margin-top: 8px;
}
.sntp-badge.ok { background: rgba(16,185,129,.15); color: var(--ok); }
.sntp-badge.no { background: rgba(239,68,68,.12); color: var(--err); }

/* ── Observed time input ── */
.obs-row { display: flex; gap: 8px; align-items: center; }
#obs-time {
  flex: 1; padding: 8px 12px; border-radius: var(--radius-sm);
  border: 1px solid var(--border); background: var(--surf2);
  color: var(--text); font-size: 20px; font-weight: 300;
  letter-spacing: 1px;
}
#obs-time:focus { outline: none; border-color: var(--a); }

/* ── Timezone card ── */
.tz-row { display: flex; justify-content: space-between; align-items: baseline; padding: 5px 0; border-bottom: 1px solid var(--border); }
.tz-row:last-child { border-bottom: none; }
.tz-label { font-size: 11px; color: var(--muted); }
.tz-val { font-size: 13px; font-family: monospace; color: var(--text); max-width: 65%; text-align: right; word-break: break-all; }

/* ── Buttons ── */
.btn {
  padding: 8px 16px; border-radius: var(--radius-sm);
  border: none; cursor: pointer; font-size: 13px; font-weight: 500;
  transition: opacity var(--tr), background var(--tr);
}
.btn:active { opacity: .75; }
.btn-primary { background: var(--a); color: #000; }
.btn-primary:hover { background: var(--a2); }
.btn-secondary {
  background: var(--surf2); color: var(--text);
  border: 1px solid var(--border);
}
.btn-secondary:hover { border-color: var(--bh); }
.btn-sm { padding: 5px 12px; font-size: 12px; }
.btn-full { width: 100%; }

/* ── Fine-tune row ── */
.tune-row { display: flex; gap: 8px; }
.tune-row .btn { flex: 1; }

/* ── Lights ── */
.strip-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
.strip-name { font-size: 14px; font-weight: 600; }
.swatch-box {
  width: 28px; height: 28px; border-radius: 6px;
  border: 1px solid var(--border); display: inline-block;
}
.effect-info { font-size: 12px; color: var(--dim); }
.pct-info { font-size: 12px; color: var(--a); font-weight: 600; }

.slider-row { display: flex; align-items: center; gap: 10px; margin: 10px 0; }
.slider-row label { font-size: 11px; color: var(--muted); width: 70px; flex-shrink: 0; }
input[type=range] {
  flex: 1; -webkit-appearance: none; height: 4px;
  border-radius: 2px; background: var(--surf2);
  outline: none; cursor: pointer;
}
input[type=range]::-webkit-slider-thumb {
  -webkit-appearance: none; width: 16px; height: 16px;
  border-radius: 50%; background: var(--a); cursor: pointer;
}

.fx-grid { display: flex; flex-wrap: wrap; gap: 6px; margin: 10px 0; }
.fx-btn {
  padding: 5px 10px; border-radius: 6px; border: 1px solid var(--border);
  background: var(--surf2); color: var(--dim); cursor: pointer;
  font-size: 12px; transition: all var(--tr);
}
.fx-btn:hover { border-color: var(--a); color: var(--a); }
.fx-btn.active { background: var(--ag); border-color: var(--a); color: var(--a); }

.color-grid { display: grid; grid-template-columns: repeat(7, 1fr); gap: 6px; margin-top: 10px; }
.color-swatch {
  aspect-ratio: 1; border-radius: 6px; cursor: pointer;
  border: 2px solid transparent; transition: border-color var(--tr), transform var(--tr);
}
.color-swatch:hover { transform: scale(1.1); }
.color-swatch.active { border-color: white; }
.color-sect-label { font-size: 10px; color: var(--muted); margin-top: 10px; margin-bottom: 2px; letter-spacing: .5px; }

/* ── Info grid ── */
.info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.info-item { background: var(--surf2); border-radius: var(--radius-sm); padding: 10px; }
.info-label { font-size: 10px; color: var(--muted); text-transform: uppercase; letter-spacing: .6px; }
.info-val { font-size: 14px; font-weight: 500; margin-top: 2px; word-break: break-all; }
.info-item.wide { grid-column: 1/-1; }

.pin-table { width: 100%; border-collapse: collapse; font-size: 12px; }
.pin-table th { text-align: left; padding: 6px 8px; color: var(--muted); font-weight: 500; border-bottom: 1px solid var(--border); }
.pin-table td { padding: 6px 8px; border-bottom: 1px solid var(--border); }
.pin-table tr:last-child td { border-bottom: none; }
.pin-table .pin-num { font-family: monospace; color: var(--a); }

/* ── Config ── */
.cfg-row { display: flex; align-items: center; gap: 8px; margin: 8px 0; }
.cfg-row label { font-size: 13px; color: var(--dim); flex: 1; }
.cfg-row input[type=number] {
  width: 80px; padding: 6px 8px; border-radius: var(--radius-sm);
  border: 1px solid var(--border); background: var(--surf2);
  color: var(--text); font-size: 14px; text-align: center;
}
.cfg-row input[type=number]:focus { outline: none; border-color: var(--a); }

.dir-toggle { display: flex; gap: 0; border-radius: var(--radius-sm); overflow: hidden; border: 1px solid var(--border); }
.dir-opt {
  flex: 1; padding: 7px; text-align: center; cursor: pointer;
  font-size: 12px; font-weight: 500; color: var(--muted);
  background: var(--surf2); border: none;
  transition: background var(--tr), color var(--tr);
}
.dir-opt.active { background: var(--ag); color: var(--a); }

.offset-ctrl { display: flex; align-items: center; gap: 8px; }
.offset-val { font-size: 20px; font-weight: 300; min-width: 48px; text-align: center; font-variant-numeric: tabular-nums; }

/* ── Toast ── */
#toast {
  position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%) translateY(60px);
  background: var(--surf); border: 1px solid var(--border);
  padding: 10px 20px; border-radius: 20px; font-size: 13px;
  transition: transform .25s ease, opacity .25s ease; opacity: 0;
  pointer-events: none; z-index: 999; white-space: nowrap;
}
#toast.show { transform: translateX(-50%) translateY(0); opacity: 1; }
#toast.ok { border-color: var(--ok); color: var(--ok); }
#toast.err { border-color: var(--err); color: var(--err); }
</style>
</head>
<body>

<!-- ── Top bar ─────────────────────────────────────────────────────────── -->
<div id="topbar">
  <span class="logo">&#9201; Clock</span>
  <button class="nav-btn active" onclick="nav('clock')">Clock</button>
  <button class="nav-btn" onclick="nav('lights')">Lights</button>
  <button class="nav-btn" onclick="nav('info')">Info</button>
  <button class="nav-btn" onclick="nav('config')">Config</button>
  <button id="theme-toggle" onclick="toggleTheme()" title="Toggle light/dark">&#9788;</button>
  <div class="accent-dots">
    <div class="accent-dot active" style="background:#00d4ff" data-accent="cyan"    onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#4f8ef7" data-accent="blue"    onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#22d3a0" data-accent="green"   onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#fb923c" data-accent="orange"  onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#fb7185" data-accent="rose"    onclick="setAccent(this)"></div>
  </div>
</div>

<div id="main">

<!-- ════════════════════════════════════ CLOCK ═══════════════════════════ -->
<div id="sec-clock" class="section active">

  <!-- Time display -->
  <div class="card">
    <div id="big-time">--:--:--</div>
    <div id="big-date">---</div>
    <span id="sntp-badge" class="sntp-badge no">&#9679; SNTP not synced</span>
  </div>

  <!-- Observed Clock Position -->
  <div class="card">
    <div class="card-title">Observed Clock Position</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Look at the clock hands and enter the time the hands currently show, then press the button.
    </p>
    <div class="obs-row">
      <input type="time" id="obs-time" value="12:00">
      <button class="btn btn-primary" onclick="doSetTime()">Set Clock to Actual Time</button>
    </div>
  </div>

  <!-- Fine Tune -->
  <div class="card">
    <div class="card-title">Fine Tune</div>
    <div class="tune-row">
      <button class="btn btn-secondary" onclick="post('step-bwd')">&#8592; Step Back</button>
      <button class="btn btn-secondary" onclick="post('step-fwd')">Step Forward &#8594;</button>
    </div>
  </div>

  <!-- Timezone -->
  <div class="card">
    <div class="card-title">Timezone</div>
    <div class="tz-row"><span class="tz-label">IANA</span><span class="tz-val" id="tz-iana">—</span></div>
    <div class="tz-row"><span class="tz-label">POSIX</span><span class="tz-val" id="tz-posix">—</span></div>
    <div class="tz-row"><span class="tz-label">Location</span><span class="tz-val" id="tz-loc">—</span></div>
  </div>

</div><!-- /sec-clock -->

<!-- ════════════════════════════════════ LIGHTS ══════════════════════════ -->
<div id="sec-lights" class="section">
  <div class="card" id="strip1-card">
    <div class="strip-header">
      <span class="strip-name">Ring</span>
      <div style="display:flex;align-items:center;gap:8px;">
        <span class="swatch-box" id="sw1"></span>
        <span class="effect-info" id="fx1-label">—</span>
        <span class="pct-info" id="bright1-pct">—</span>
      </div>
    </div>
    <div class="slider-row">
      <label>Brightness</label>
      <input type="range" id="bright1" min="0" max="255" value="128"
             onpointerdown="sliderActive[0]=true" oninput="doBright(1,this.value)">
    </div>
    <div class="card-title" style="margin-top:10px;">Effects</div>
    <div class="fx-grid" id="fx1-grid">
      <button class="fx-btn" data-fx1="Static"   onclick="doFx(1,'static')">Static</button>
      <button class="fx-btn" data-fx1="Breathe"  onclick="doFx(1,'breathe')">Breathe</button>
      <button class="fx-btn" data-fx1="Rainbow"  onclick="doFx(1,'rainbow')">Rainbow</button>
      <button class="fx-btn" data-fx1="Chase"    onclick="doFx(1,'chase')">Chase</button>
      <button class="fx-btn" data-fx1="Sparkle"  onclick="doFx(1,'sparkle')">Sparkle</button>
      <button class="fx-btn" data-fx1="Wipe"     onclick="doFx(1,'wipe')">Wipe</button>
      <button class="fx-btn" data-fx1="Comet"    onclick="doFx(1,'comet')">Comet</button>
    </div>
    <div class="card-title" style="margin-top:10px;">Colour</div>
    <div class="color-grid" id="color1-grid"></div>
  </div>

  <!-- Base -->
  <div class="card" id="strip2-card">
    <div class="strip-header">
      <span class="strip-name">Base</span>
      <div style="display:flex;align-items:center;gap:8px;">
        <span class="swatch-box" id="sw2"></span>
        <span class="effect-info" id="fx2-label">—</span>
        <span class="pct-info" id="bright2-pct">—</span>
      </div>
    </div>
    <div class="slider-row">
      <label>Brightness</label>
      <input type="range" id="bright2" min="0" max="255" value="128"
             onpointerdown="sliderActive[1]=true" oninput="doBright(2,this.value)">
    </div>
    <div class="card-title" style="margin-top:10px;">Effects</div>
    <div class="fx-grid" id="fx2-grid">
      <button class="fx-btn" data-fx2="Static"   onclick="doFx(2,'static')">Static</button>
      <button class="fx-btn" data-fx2="Breathe"  onclick="doFx(2,'breathe')">Breathe</button>
      <button class="fx-btn" data-fx2="Rainbow"  onclick="doFx(2,'rainbow')">Rainbow</button>
      <button class="fx-btn" data-fx2="Chase"    onclick="doFx(2,'chase')">Chase</button>
      <button class="fx-btn" data-fx2="Sparkle"  onclick="doFx(2,'sparkle')">Sparkle</button>
      <button class="fx-btn" data-fx2="Wipe"     onclick="doFx(2,'wipe')">Wipe</button>
      <button class="fx-btn" data-fx2="Comet"    onclick="doFx(2,'comet')">Comet</button>
    </div>
    <div class="card-title" style="margin-top:10px;">Colour</div>
    <div class="color-grid" id="color2-grid"></div>
  </div>

</div><!-- /sec-lights -->

<!-- ════════════════════════════════════ INFORMATION ═════════════════════ -->
<div id="sec-info" class="section">

  <!-- System -->
  <div class="card">
    <div class="card-title">System</div>
    <div class="info-grid">
      <div class="info-item"><div class="info-label">Firmware</div><div class="info-val" id="inf-fw">—</div></div>
      <div class="info-item"><div class="info-label">Uptime</div><div class="info-val" id="inf-uptime">—</div></div>
      <div class="info-item"><div class="info-label">Free Heap</div><div class="info-val" id="inf-heap">—</div></div>
      <div class="info-item"><div class="info-label">Display Min</div><div class="info-val" id="inf-dmin">—</div></div>
    </div>
  </div>

  <!-- Network -->
  <div class="card">
    <div class="card-title">Network</div>
    <div class="info-grid">
      <div class="info-item"><div class="info-label">WiFi SSID</div><div class="info-val" id="inf-ssid">—</div></div>
      <div class="info-item"><div class="info-label">Signal (RSSI)</div><div class="info-val" id="inf-rssi">—</div></div>
      <div class="info-item"><div class="info-label">Local IP</div><div class="info-val" id="inf-lip">—</div></div>
      <div class="info-item"><div class="info-label">Gateway</div><div class="info-val" id="inf-gw">—</div></div>
      <div class="info-item wide"><div class="info-label">Internet IP</div><div class="info-val" id="inf-eip">—</div></div>
      <div class="info-item wide"><div class="info-label">Location</div><div class="info-val" id="inf-geo">—</div></div>
    </div>
  </div>

  <!-- LED Strips -->
  <div class="card">
    <div class="card-title">LED Strips</div>
    <div class="info-grid">
      <div class="info-item">
        <div class="info-label">Ring</div>
        <div class="info-val" id="inf-s1" style="display:flex;align-items:center;gap:6px;"></div>
      </div>
      <div class="info-item">
        <div class="info-label">Base</div>
        <div class="info-val" id="inf-s2" style="display:flex;align-items:center;gap:6px;"></div>
      </div>
      <div class="info-item"><div class="info-label">Ring LEDs</div><div class="info-val" id="inf-s1len">—</div></div>
      <div class="info-item"><div class="info-label">Base LEDs</div><div class="info-val" id="inf-s2len">—</div></div>
    </div>
  </div>

  <!-- GPIO Map -->
  <div class="card">
    <div class="card-title">GPIO Pin Map</div>
    <table class="pin-table">
      <thead><tr><th>GPIO</th><th>Function</th></tr></thead>
      <tbody>
        <tr><td class="pin-num">6</td><td>Stepper IN4</td></tr>
        <tr><td class="pin-num">7</td><td>Stepper IN3</td></tr>
        <tr><td class="pin-num">8</td><td>I2C SDA (Display + Encoder)</td></tr>
        <tr><td class="pin-num">9</td><td>I2C SCL (Display + Encoder)</td></tr>
        <tr><td class="pin-num">10</td><td>Button A (Menu Prev)</td></tr>
        <tr><td class="pin-num">11</td><td>Button B (Menu Next)</td></tr>
        <tr><td class="pin-num">13</td><td>Sensor LED (output)</td></tr>
        <tr><td class="pin-num">14</td><td>Sensor LDR (ADC input)</td></tr>
        <tr><td class="pin-num">15</td><td>Stepper IN2</td></tr>
        <tr><td class="pin-num">16</td><td>Stepper IN1</td></tr>
        <tr><td class="pin-num">LED1</td><td>WS2812B Ring Data</td></tr>
        <tr><td class="pin-num">LED2</td><td>WS2812B Base Data</td></tr>
      </tbody>
    </table>
  </div>

  <!-- Sensor -->
  <div class="card">
    <div class="card-title">Sensor</div>
    <div class="info-grid">
      <div class="info-item"><div class="info-label">Sensor Offset</div><div class="info-val" id="inf-offset">—</div></div>
      <div class="info-item"><div class="info-label">Last ADC Reading</div><div class="info-val" id="inf-adc">—</div></div>
      <div class="info-item"><div class="info-label">Motor Direction</div><div class="info-val" id="inf-mdir">—</div></div>
      <div class="info-item"><div class="info-label">IANA TZ</div><div class="info-val" id="inf-tz-iana">—</div></div>
      <div class="info-item wide"><div class="info-label">POSIX TZ</div><div class="info-val" id="inf-tz-posix" style="font-size:12px;font-family:monospace;">—</div></div>
    </div>
  </div>

</div><!-- /sec-info -->

<!-- ════════════════════════════════════ CONFIGURATION ═══════════════════ -->
<div id="sec-config" class="section">

  <!-- Strip Lengths -->
  <div class="card">
    <div class="card-title">LED Strip Lengths</div>
    <div class="cfg-row">
      <label>Ring LED count</label>
      <input type="number" id="cfg-s1len" min="1" max="300" value="30">
      <button class="btn btn-secondary btn-sm" onclick="applyLen(1)">Apply</button>
    </div>
    <div class="cfg-row">
      <label>Base LED count</label>
      <input type="number" id="cfg-s2len" min="1" max="300" value="30">
      <button class="btn btn-secondary btn-sm" onclick="applyLen(2)">Apply</button>
    </div>
  </div>

  <!-- Motor Direction -->
  <div class="card">
    <div class="card-title">Motor Direction</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">Select Normal if the clock hands advance clockwise, or Reverse if they go counter-clockwise.</p>
    <div class="dir-toggle">
      <button class="dir-opt active" id="dir-norm" onclick="setDir(false)">Normal</button>
      <button class="dir-opt"        id="dir-rev"  onclick="setDir(true)">Reverse</button>
    </div>
  </div>

  <!-- Sensor Calibration -->
  <div class="card">
    <div class="card-title">Sensor Calibration</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:12px;">
      The sensor offset compensates for the physical gap between where the magnet/reflector triggers and the actual top-of-hour position.
    </p>
    <div class="cfg-row">
      <label>Sensor offset (seconds)</label>
      <div class="offset-ctrl">
        <button class="btn btn-secondary btn-sm" onclick="adjOffset(-1)">&#8722;</button>
        <span class="offset-val" id="cfg-offset-val">0</span>
        <button class="btn btn-secondary btn-sm" onclick="adjOffset(+1)">&#43;</button>
      </div>
      <button class="btn btn-secondary btn-sm" onclick="saveOffset()">Save</button>
    </div>
    <div style="display:flex;gap:8px;margin-top:8px;">
      <button class="btn btn-secondary btn-sm" style="flex:1" onclick="doCalibrate()">Calibrate Dark</button>
      <button class="btn btn-secondary btn-sm" style="flex:1" onclick="doMeasure()">Measure ADC</button>
    </div>
    <div style="font-size:11px;color:var(--muted);margin-top:8px;">
      Last ADC: <span id="cfg-adc-val">—</span>
    </div>
  </div>

  <!-- Motor Speed -->
  <div class="card">
    <div class="card-title">Motor Speed</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Lower delay = faster &amp; noisier. Higher = slower &amp; quieter. Default: 2000 µs/step.
    </p>
    <div class="slider-row">
      <label>Step delay</label>
      <input type="range" id="cfg-step-delay" min="900" max="3000" value="2000"
             oninput="updateSpeedLabel(this.value)">
    </div>
    <div style="font-size:11px;color:var(--muted);margin-bottom:8px;">
      <span id="cfg-step-label">2000 µs/step</span>
      &nbsp;·&nbsp; <span id="cfg-step-hz">—</span>
    </div>
    <button class="btn btn-secondary btn-sm" onclick="saveSpeed()">Save Speed</button>
  </div>

  <!-- WiFi Credentials -->
  <div class="card">
    <div class="card-title">WiFi Credentials</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Saved credentials are used on next restart.
    </p>
    <div class="cfg-row">
      <label>SSID</label>
      <input type="text" id="cfg-ssid" placeholder="Network name" autocomplete="off"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:13px;">
    </div>
    <div class="cfg-row" style="margin-top:6px;">
      <label>Password</label>
      <input type="password" id="cfg-pass" placeholder="••••••••" autocomplete="off"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:13px;">
    </div>
    <button class="btn btn-secondary btn-sm" style="margin-top:10px;width:100%"
            onclick="saveWifi()">Save Credentials</button>
  </div>

  <!-- Timezone Override -->
  <div class="card">
    <div class="card-title">Timezone Override</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Leave empty to use automatic geolocation. Format: POSIX TZ string, e.g.
      <code style="font-size:11px;color:var(--a)">CST6CDT,M3.2.0,M11.1.0</code>
    </p>
    <div class="cfg-row">
      <input type="text" id="cfg-tz" placeholder="Empty = auto-detect"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:12px;font-family:monospace;">
      <button class="btn btn-secondary btn-sm" onclick="saveTz()">Save</button>
    </div>
  </div>

</div><!-- /sec-config -->
</div><!-- /main -->

<div id="toast"></div>

<script>
// ── 21 Matter-compatible colour presets ──────────────────────────────────────
const COLORS = [
  {name:'Red',          r:255, g:20,  b:20 },
  {name:'Orange',       r:255, g:96,  b:0  },
  {name:'Amber',        r:255, g:191, b:0  },
  {name:'Yellow',       r:255, g:224, b:0  },
  {name:'Lime',         r:128, g:255, b:0  },
  {name:'Green',        r:0,   g:224, b:64 },
  {name:'Spring Green', r:0,   g:255, b:128},
  {name:'Cyan',         r:0,   g:232, b:255},
  {name:'Azure',        r:0,   g:112, b:255},
  {name:'Blue',         r:32,  g:32,  b:255},
  {name:'Indigo',       r:64,  g:0,   b:255},
  {name:'Violet',       r:128, g:0,   b:255},
  {name:'Magenta',      r:255, g:0,   b:255},
  {name:'Pink',         r:255, g:0,   b:128},
  {name:'Rose',         r:255, g:0,   b:64 },
  {name:'Warm White',   r:255, g:198, b:128},
  {name:'Soft White',   r:255, g:224, b:160},
  {name:'Cool White',   r:208, g:232, b:255},
  {name:'Daylight',     r:240, g:240, b:255},
  {name:'Coral',        r:255, g:88,  b:64 },
  {name:'Lavender',     r:150, g:123, b:182},
];

// ── State ────────────────────────────────────────────────────────────────────
let brightTimer1 = null, brightTimer2 = null;
let cfgOffset = 0;
let lastData = {};

// Config fields that require explicit Save/Apply should only be populated
// once on first load, then only refreshed when the server value actually
// changes (e.g. via UART console) — never during normal 1-second WS pushes.
let cfgInitialized = false;
let serverCfg = {};   // last values seen FROM the server for config fields

// Brightness sliders: skip WS updates while user is actively dragging.
let sliderActive = [false, false];
document.addEventListener('pointerup',     () => { sliderActive = [false, false]; });
document.addEventListener('pointercancel', () => { sliderActive = [false, false]; });

// ── Navigation ───────────────────────────────────────────────────────────────
function nav(id) {
  document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  document.getElementById('sec-' + id).classList.add('active');
  event.currentTarget.classList.add('active');
}

// ── Theme ────────────────────────────────────────────────────────────────────
function toggleTheme() {
  const cur = document.documentElement.getAttribute('data-theme');
  const next = cur === 'dark' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', next);
  localStorage.setItem('theme', next);
}
function setAccent(el) {
  const a = el.dataset.accent;
  document.documentElement.setAttribute('data-accent', a);
  document.querySelectorAll('.accent-dot').forEach(d => d.classList.toggle('active', d.dataset.accent === a));
  localStorage.setItem('accent', a);
}
(function initTheme() {
  const t = localStorage.getItem('theme') || 'dark';
  const a = localStorage.getItem('accent') || 'cyan';
  document.documentElement.setAttribute('data-theme', t);
  document.documentElement.setAttribute('data-accent', a);
  document.querySelectorAll('.accent-dot').forEach(d => d.classList.toggle('active', d.dataset.accent === a));
})();

// ── Toast ────────────────────────────────────────────────────────────────────
let toastTimer;
function toast(msg, type='ok') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'show ' + type;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = ''; }, 2500);
}

// ── API helpers ──────────────────────────────────────────────────────────────
function post(cmd, extra) {
  return fetch('/api/cmd', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(Object.assign({cmd}, extra))
  })
  .then(r => r.json())
  .then(d => { if (!d.ok) toast(d.msg || 'Error', 'err'); })
  .catch(() => toast('Network error', 'err'));
}

function postCfg(body) {
  return fetch('/api/cfg', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body)
  })
  .then(r => r.json())
  .then(d => { toast(d.ok ? 'Saved' : 'Error', d.ok ? 'ok' : 'err'); })
  .catch(() => toast('Network error', 'err'));
}

// ── Clock commands ───────────────────────────────────────────────────────────
function doSetTime() {
  const val = document.getElementById('obs-time').value; // "HH:MM"
  if (!val) { toast('Enter observed time', 'err'); return; }
  const parts = val.split(':');
  const observed_hour = parseInt(parts[0], 10) % 12;
  const observed_min  = parseInt(parts[1], 10);
  post('set-time', {observed_hour, observed_min}).then(() => toast('Set-time command sent'));
}

// ── Lights ───────────────────────────────────────────────────────────────────
function buildColorGrid(n) {
  const grid = document.getElementById('color' + n + '-grid');
  grid.innerHTML = '';
  COLORS.forEach((c, i) => {
    const el = document.createElement('div');
    el.className = 'color-swatch';
    el.style.background = 'rgb(' + c.r + ',' + c.g + ',' + c.b + ')';
    el.title = c.name;
    el.dataset.idx = i;
    el.onclick = () => doColor(n, c.r, c.g, c.b, el);
    grid.appendChild(el);
  });
}

function doColor(n, r, g, b, el) {
  document.querySelectorAll('#color' + n + '-grid .color-swatch').forEach(s => s.classList.remove('active'));
  if (el) el.classList.add('active');
  post('led' + n + '-color', {r, g, b});
}

function doFx(n, fx) {
  post('led' + n + '-' + fx).then(() => {
    // Highlight is updated by next WS push
  });
}

function doBright(n, val) {
  val = parseInt(val);
  document.getElementById('bright' + n + '-pct').textContent = Math.round(val/255*100) + '%';
  clearTimeout(n === 1 ? brightTimer1 : brightTimer2);
  const t = setTimeout(() => post('led' + n + '-bright', {brightness: val}), 80);
  if (n === 1) brightTimer1 = t; else brightTimer2 = t;
}

function applyStrip(n, s) {
  const pct = Math.round((s.brightness || 0) / 255 * 100);
  // Skip brightness slider update while user is actively dragging it
  if (!sliderActive[n - 1]) {
    document.getElementById('bright' + n).value = s.brightness || 0;
    document.getElementById('bright' + n + '-pct').textContent = pct + '%';
  }
  document.getElementById('sw' + n).style.background = 'rgb(' + s.r + ',' + s.g + ',' + s.b + ')';
  document.getElementById('fx' + n + '-label').textContent = s.effect || '—';
  // Highlight active effect button
  const attr = 'data-fx' + n;
  document.querySelectorAll('[' + attr + ']').forEach(b => {
    b.classList.toggle('active', b.getAttribute(attr) === s.effect);
  });
  // Highlight matching color swatch
  const grid = document.getElementById('color' + n + '-grid');
  if (grid) {
    grid.querySelectorAll('.color-swatch').forEach(sw => {
      const c = COLORS[parseInt(sw.dataset.idx)];
      const match = c && c.r === s.r && c.g === s.g && c.b === s.b;
      sw.classList.toggle('active', match);
    });
  }
}

// ── Config ───────────────────────────────────────────────────────────────────
function applyLen(n) {
  const v = parseInt(document.getElementById('cfg-s' + n + 'len').value);
  if (isNaN(v) || v < 1) { toast('Invalid length', 'err'); return; }
  const body = {};
  body['strip' + n + '_len'] = v;
  postCfg(body);
}

function setDir(rev) {
  document.getElementById('dir-norm').classList.toggle('active', !rev);
  document.getElementById('dir-rev').classList.toggle('active', rev);
  postCfg({motor_reverse: rev});
}

function adjOffset(delta) {
  cfgOffset += delta;
  document.getElementById('cfg-offset-val').textContent = cfgOffset;
}
function saveOffset() {
  postCfg({sensor_offset: cfgOffset});
}

function doCalibrate() {
  post('calibrate').then(() => toast('Calibration done'));
}
function doMeasure() {
  post('measure').then(() => toast('ADC measured'));
}

function updateSpeedLabel(v) {
  v = parseInt(v);
  const hz = (1e6 / v).toFixed(1);
  document.getElementById('cfg-step-label').textContent = v + ' µs/step';
  document.getElementById('cfg-step-hz').textContent = hz + ' steps/s';
}
function saveSpeed() {
  const v = parseInt(document.getElementById('cfg-step-delay').value);
  postCfg({step_delay_us: v}).then(() => toast('Motor speed saved'));
}

function saveWifi() {
  const ssid = document.getElementById('cfg-ssid').value.trim();
  const pass = document.getElementById('cfg-pass').value;
  if (!ssid) { toast('Enter SSID', 'err'); return; }
  postCfg({ssid, password: pass}).then(() => {
    toast('WiFi saved — restart to apply');
    document.getElementById('cfg-pass').value = '';
  });
}

function saveTz() {
  const tz = document.getElementById('cfg-tz').value.trim();
  postCfg({tz_override: tz}).then(() => toast('Timezone saved — restart to apply'));
}

// ── Data application ─────────────────────────────────────────────────────────
function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}

function fmtUptime(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h + 'h ' + m + 'm ' + sec + 's';
}

function fmtHeap(b) {
  return (b / 1024).toFixed(1) + ' KB';
}

function applyData(d) {
  lastData = d;

  // Clock section
  setText('big-time', d.time || '--:--:--');
  setText('big-date', d.date || '---');
  const badge = document.getElementById('sntp-badge');
  if (d.sntp) {
    badge.className = 'sntp-badge ok';
    badge.textContent = '\u25cf SNTP synced';
  } else {
    badge.className = 'sntp-badge no';
    badge.textContent = '\u25cf SNTP not synced';
  }
  setText('tz-iana', d.iana_tz || '—');
  setText('tz-posix', d.posix_tz || '—');
  const loc = [d.city, d.region].filter(Boolean).join(', ');
  setText('tz-loc', loc || '—');

  // Lights
  if (d.leds) {
    if (d.leds.strip1) applyStrip(1, d.leds.strip1);
    if (d.leds.strip2) applyStrip(2, d.leds.strip2);
  }

  // Info section
  setText('inf-fw', d.fw_version || '—');
  setText('inf-uptime', fmtUptime(d.uptime_s || 0));
  setText('inf-heap', fmtHeap(d.free_heap || 0));
  const dispPos = (d.displayed_hour >= 0 && d.displayed_min >= 0)
    ? String(d.displayed_hour).padStart(2,'0') + ':' + String(d.displayed_min).padStart(2,'0')
    : '—';
  setText('inf-dmin', dispPos);
  setText('inf-ssid', d.ssid || '—');
  setText('inf-rssi', d.rssi !== undefined ? d.rssi + ' dBm' : '—');
  setText('inf-lip', d.local_ip || '—');
  setText('inf-gw', d.gateway || '—');
  setText('inf-eip', d.external_ip || '—');
  const geo = [d.city, d.region, d.isp].filter(Boolean).join(' · ');
  setText('inf-geo', geo || '—');

  // Strip info
  function stripInfoHTML(s) {
    if (!s) return '—';
    return '<span style="width:14px;height:14px;border-radius:3px;display:inline-block;background:rgb(' + s.r + ',' + s.g + ',' + s.b + ');flex-shrink:0"></span>' +
           '<span>' + (s.effect || '—') + ' · ' + Math.round((s.brightness||0)/255*100) + '%</span>';
  }
  const s1el = document.getElementById('inf-s1');
  const s2el = document.getElementById('inf-s2');
  if (s1el && d.leds && d.leds.strip1) s1el.innerHTML = stripInfoHTML(d.leds.strip1);
  if (s2el && d.leds && d.leds.strip2) s2el.innerHTML = stripInfoHTML(d.leds.strip2);
  setText('inf-s1len', d.leds && d.leds.strip1 ? d.leds.strip1.active_len + ' LEDs' : '—');
  setText('inf-s2len', d.leds && d.leds.strip2 ? d.leds.strip2.active_len + ' LEDs' : '—');

  setText('inf-offset', (d.sensor_offset_sec !== undefined ? d.sensor_offset_sec : '—') + ' s');
  setText('inf-adc', d.sensor_adc !== undefined ? d.sensor_adc : '—');
  setText('inf-mdir', d.motor_reverse ? 'Reverse' : 'Normal');
  setText('inf-tz-iana', d.iana_tz || '—');
  setText('inf-tz-posix', d.posix_tz || '—');

  // ── Config fields: init once, then only update on external server-side change ──
  // This prevents WS pushes from stomping on values the user is editing.
  const s1len = d.leds && d.leds.strip1 ? d.leds.strip1.active_len : undefined;
  const s2len = d.leds && d.leds.strip2 ? d.leds.strip2.active_len : undefined;

  if (!cfgInitialized) {
    // First data received — populate all config inputs
    if (d.sensor_offset_sec !== undefined) {
      cfgOffset = d.sensor_offset_sec;
      document.getElementById('cfg-offset-val').textContent = cfgOffset;
    }
    if (d.step_delay_us) {
      document.getElementById('cfg-step-delay').value = d.step_delay_us;
      updateSpeedLabel(d.step_delay_us);
    }
    if (d.ssid) document.getElementById('cfg-ssid').value = d.ssid;
    if (s1len !== undefined) document.getElementById('cfg-s1len').value = s1len;
    if (s2len !== undefined) document.getElementById('cfg-s2len').value = s2len;
    document.getElementById('dir-norm').classList.toggle('active', !d.motor_reverse);
    document.getElementById('dir-rev').classList.toggle('active', !!d.motor_reverse);

    // Snapshot initial server state
    serverCfg = {
      sensor_offset: d.sensor_offset_sec,
      step_delay_us: d.step_delay_us,
      motor_reverse: d.motor_reverse,
      s1len, s2len
    };
    cfgInitialized = true;

  } else {
    // Subsequent pushes — only update a config input if the server value
    // actually changed (catches external changes via UART/console).
    if (d.sensor_offset_sec !== undefined && d.sensor_offset_sec !== serverCfg.sensor_offset) {
      cfgOffset = d.sensor_offset_sec;
      document.getElementById('cfg-offset-val').textContent = cfgOffset;
      serverCfg.sensor_offset = d.sensor_offset_sec;
    }
    if (d.step_delay_us && d.step_delay_us !== serverCfg.step_delay_us) {
      document.getElementById('cfg-step-delay').value = d.step_delay_us;
      updateSpeedLabel(d.step_delay_us);
      serverCfg.step_delay_us = d.step_delay_us;
    }
    if (d.motor_reverse !== undefined && d.motor_reverse !== serverCfg.motor_reverse) {
      document.getElementById('dir-norm').classList.toggle('active', !d.motor_reverse);
      document.getElementById('dir-rev').classList.toggle('active', !!d.motor_reverse);
      serverCfg.motor_reverse = d.motor_reverse;
    }
    if (s1len !== undefined && s1len !== serverCfg.s1len) {
      document.getElementById('cfg-s1len').value = s1len;
      serverCfg.s1len = s1len;
    }
    if (s2len !== undefined && s2len !== serverCfg.s2len) {
      document.getElementById('cfg-s2len').value = s2len;
      serverCfg.s2len = s2len;
    }
  }

  setText('cfg-adc-val', d.sensor_adc !== undefined ? d.sensor_adc : '—');
}

// ── WebSocket ─────────────────────────────────────────────────────────────────
function connectWS() {
  const ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onmessage = e => {
    try { applyData(JSON.parse(e.data)); } catch(err) {}
  };
  ws.onclose = () => setTimeout(connectWS, 2000);
}

// ── Init ─────────────────────────────────────────────────────────────────────
buildColorGrid(1);
buildColorGrid(2);

// Initial fetch
fetch('/api/status').then(r => r.json()).then(applyData).catch(() => {});

connectWS();
</script>
</body>
</html>
)html";
static const size_t WEB_UI_HTML_LEN = sizeof(WEB_UI_HTML) - 1;
