#pragma once
static const char WEB_UI_HTML[] = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Clock Driver</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  :root {
    --bg:           #060612;
    --surface:      rgba(255,255,255,0.04);
    --border:       rgba(255,255,255,0.08);
    --border-hover: rgba(255,255,255,0.16);
    --accent:       #00d4ff;
    --accent2:      #a855f7;
    --ok:           #10b981;
    --warn:         #f59e0b;
    --err:          #ef4444;
    --text:         #e2e8f0;
    --text-muted:   #64748b;
    --text-dim:     #94a3b8;
    --header-h:     56px;
    --drawer-w:     240px;
    --radius:       12px;
    --radius-sm:    8px;
    --transition:   0.2s ease;
  }

  html, body {
    height: 100%;
    background: var(--bg);
    color: var(--text);
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    font-size: 14px;
    line-height: 1.5;
    overflow-x: hidden;
  }

  /* ── Scrollbar ── */
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: transparent; }
  ::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.1); border-radius: 3px; }

  /* ── Header ── */
  #header {
    position: fixed;
    top: 0; left: 0; right: 0;
    height: var(--header-h);
    background: rgba(6,6,18,0.85);
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    padding: 0 16px;
    gap: 12px;
    z-index: 100;
  }

  #hamburger {
    width: 36px; height: 36px;
    background: none; border: none; cursor: pointer;
    display: flex; flex-direction: column;
    justify-content: center; align-items: center;
    gap: 5px; padding: 4px;
    border-radius: var(--radius-sm);
    transition: background var(--transition);
    flex-shrink: 0;
  }
  #hamburger:hover { background: var(--surface); }
  #hamburger span {
    display: block;
    width: 20px; height: 2px;
    background: var(--text);
    border-radius: 2px;
    transition: transform 0.3s ease, opacity 0.3s ease, width 0.3s ease;
    transform-origin: center;
  }
  body.drawer-open #hamburger span:nth-child(1) { transform: translateY(7px) rotate(45deg); }
  body.drawer-open #hamburger span:nth-child(2) { opacity: 0; transform: scaleX(0); }
  body.drawer-open #hamburger span:nth-child(3) { transform: translateY(-7px) rotate(-45deg); }

  #header-title {
    font-size: 17px;
    font-weight: 700;
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    background-clip: text;
    letter-spacing: -0.3px;
    white-space: nowrap;
  }

  #conn-dot {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--err);
    flex-shrink: 0;
    transition: background 0.4s ease;
    box-shadow: 0 0 6px currentColor;
  }
  #conn-dot.ok  { background: var(--ok);   color: var(--ok); }
  #conn-dot.err { background: var(--err);  color: var(--err); }

  #header-spacer { flex: 1; }

  #header-time {
    font-size: 18px;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
    color: var(--accent);
    letter-spacing: 0.5px;
    white-space: nowrap;
  }

  /* ── Backdrop ── */
  #backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0,0,0,0.6);
    z-index: 150;
    opacity: 0;
    pointer-events: none;
    transition: opacity 0.3s ease;
  }
  body.drawer-open #backdrop {
    opacity: 1;
    pointer-events: auto;
  }

  /* ── Drawer ── */
  #drawer {
    position: fixed;
    top: 0; left: 0; bottom: 0;
    width: var(--drawer-w);
    background: rgba(8,8,24,0.98);
    border-right: 1px solid var(--border);
    z-index: 200;
    transform: translateX(-100%);
    transition: transform 0.3s cubic-bezier(0.4,0,0.2,1);
    display: flex;
    flex-direction: column;
    padding-top: var(--header-h);
    overflow: hidden;
  }
  body.drawer-open #drawer { transform: translateX(0); }

  #drawer-inner { padding: 16px 8px; flex: 1; overflow-y: auto; }

  #drawer-label {
    font-size: 10px;
    font-weight: 600;
    letter-spacing: 1.2px;
    text-transform: uppercase;
    color: var(--text-muted);
    padding: 0 12px 8px;
  }

  .nav-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border-radius: var(--radius-sm);
    cursor: pointer;
    color: var(--text-dim);
    font-weight: 500;
    transition: background var(--transition), color var(--transition);
    user-select: none;
    border: none;
    background: none;
    width: 100%;
    text-align: left;
    font-size: 14px;
  }
  .nav-item:hover { background: var(--surface); color: var(--text); }
  .nav-item.active {
    background: rgba(0,212,255,0.1);
    color: var(--accent);
  }
  .nav-item .nav-icon { font-size: 16px; width: 20px; text-align: center; flex-shrink: 0; }

  /* ── Main content ── */
  #main {
    margin-top: var(--header-h);
    padding: 24px 16px 40px;
    max-width: 680px;
    margin-left: auto;
    margin-right: auto;
  }

  /* ── Sections ── */
  .section { display: none; }
  .section.active { display: block; }

  /* ── Section header ── */
  .section-header {
    margin-bottom: 20px;
  }
  .section-title {
    font-size: 22px;
    font-weight: 700;
    color: var(--text);
    letter-spacing: -0.5px;
  }
  .section-sub {
    font-size: 13px;
    color: var(--text-muted);
    margin-top: 2px;
  }

  /* ── Cards ── */
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 20px;
    margin-bottom: 16px;
    transition: border-color var(--transition);
  }
  .card:hover { border-color: var(--border-hover); }

  .card-title {
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 1px;
    text-transform: uppercase;
    color: var(--text-muted);
    margin-bottom: 14px;
  }

  /* ── Big clock display ── */
  #big-time {
    text-align: center;
    padding: 28px 0 20px;
  }
  #big-time-val {
    font-size: clamp(52px, 12vw, 80px);
    font-weight: 800;
    font-variant-numeric: tabular-nums;
    background: linear-gradient(135deg, var(--accent) 0%, var(--accent2) 100%);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    background-clip: text;
    letter-spacing: -2px;
    line-height: 1;
    display: block;
  }
  #big-date-val {
    font-size: 15px;
    color: var(--text-dim);
    margin-top: 8px;
    letter-spacing: 0.3px;
  }

  /* ── Stats grid ── */
  .stats-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
    gap: 12px;
  }

  .stat-item {
    background: rgba(255,255,255,0.025);
    border: 1px solid var(--border);
    border-radius: var(--radius-sm);
    padding: 12px 14px;
  }
  .stat-label {
    font-size: 11px;
    color: var(--text-muted);
    font-weight: 500;
    letter-spacing: 0.3px;
    margin-bottom: 4px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .stat-value {
    font-size: 15px;
    font-weight: 600;
    color: var(--text);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .stat-value.accent { color: var(--accent); }
  .stat-value.ok     { color: var(--ok); }
  .stat-value.warn   { color: var(--warn); }
  .stat-value.err    { color: var(--err); }

  /* ── Button grid ── */
  .btn-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(148px, 1fr));
    gap: 10px;
  }

  .btn {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 11px 16px;
    border-radius: var(--radius-sm);
    border: 1px solid var(--border);
    background: var(--surface);
    color: var(--text);
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: background var(--transition), border-color var(--transition),
                color var(--transition), transform 0.1s ease, opacity var(--transition);
    white-space: nowrap;
    user-select: none;
  }
  .btn:hover:not(:disabled) {
    background: rgba(255,255,255,0.07);
    border-color: var(--border-hover);
    color: var(--text);
    transform: translateY(-1px);
  }
  .btn:active:not(:disabled) { transform: translateY(0); }
  .btn:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }

  .btn.primary {
    background: linear-gradient(135deg, rgba(0,212,255,0.18), rgba(168,85,247,0.18));
    border-color: rgba(0,212,255,0.35);
    color: var(--accent);
    font-weight: 600;
  }
  .btn.primary:hover:not(:disabled) {
    background: linear-gradient(135deg, rgba(0,212,255,0.28), rgba(168,85,247,0.28));
    border-color: rgba(0,212,255,0.55);
  }

  .btn.active-effect {
    background: linear-gradient(135deg, rgba(168,85,247,0.25), rgba(0,212,255,0.15));
    border-color: rgba(168,85,247,0.5);
    color: var(--accent2);
    font-weight: 600;
  }

  /* ── Brightness bar ── */
  .bright-wrap {
    background: rgba(255,255,255,0.06);
    border: 1px solid var(--border);
    border-radius: 99px;
    height: 8px;
    overflow: hidden;
    margin-top: 8px;
  }
  .bright-fill {
    height: 100%;
    border-radius: 99px;
    background: linear-gradient(90deg, var(--accent2), var(--accent));
    transition: width 0.4s ease;
  }

  /* ── Colour swatch ── */
  .swatch {
    width: 20px; height: 20px;
    border-radius: 50%;
    border: 2px solid rgba(255,255,255,0.15);
    display: inline-block;
    vertical-align: middle;
    flex-shrink: 0;
  }

  /* ── Strip status row ── */
  .strip-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 0;
  }
  .strip-row + .strip-row {
    border-top: 1px solid var(--border);
  }
  .strip-label {
    font-size: 12px;
    font-weight: 600;
    color: var(--text-muted);
    width: 54px;
    flex-shrink: 0;
  }
  .strip-effect {
    flex: 1;
    font-size: 14px;
    font-weight: 600;
    color: var(--text);
  }
  .strip-bright {
    font-size: 12px;
    color: var(--text-muted);
    width: 36px;
    text-align: right;
    flex-shrink: 0;
  }

  /* ── Info card for About ── */
  .about-card {
    background: linear-gradient(135deg,
      rgba(0,212,255,0.06) 0%,
      rgba(168,85,247,0.06) 100%);
    border: 1px solid rgba(0,212,255,0.15);
  }
  .about-card p {
    color: var(--text-dim);
    font-size: 13.5px;
    line-height: 1.7;
  }
  .about-card p + p { margin-top: 10px; }

  /* ── Divider ── */
  .divider {
    height: 1px;
    background: var(--border);
    margin: 16px 0;
  }

  /* ── Badge ── */
  .badge {
    display: inline-flex;
    align-items: center;
    padding: 2px 8px;
    border-radius: 99px;
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.3px;
  }
  .badge.ok   { background: rgba(16,185,129,0.15); color: var(--ok); }
  .badge.err  { background: rgba(239,68,68,0.15);  color: var(--err); }
  .badge.warn { background: rgba(245,158,11,0.15); color: var(--warn); }

  /* ── Toast ── */
  #toast-container {
    position: fixed;
    bottom: 24px; right: 20px;
    z-index: 999;
    display: flex;
    flex-direction: column;
    gap: 8px;
    pointer-events: none;
  }

  .toast {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 12px 16px;
    border-radius: var(--radius-sm);
    background: rgba(15,15,30,0.96);
    border: 1px solid var(--border);
    backdrop-filter: blur(8px);
    font-size: 13px;
    font-weight: 500;
    color: var(--text);
    min-width: 220px;
    max-width: 320px;
    pointer-events: auto;
    box-shadow: 0 8px 32px rgba(0,0,0,0.5);
    animation: toastIn 0.3s cubic-bezier(0.34,1.56,0.64,1) forwards;
  }
  .toast.hide {
    animation: toastOut 0.25s ease forwards;
  }
  .toast-icon { font-size: 15px; flex-shrink: 0; }
  .toast-msg  { flex: 1; }
  .toast.ok-toast  { border-color: rgba(16,185,129,0.3); }
  .toast.err-toast { border-color: rgba(239,68,68,0.3); }

  @keyframes toastIn {
    from { opacity: 0; transform: translateX(40px) scale(0.92); }
    to   { opacity: 1; transform: translateX(0)    scale(1); }
  }
  @keyframes toastOut {
    from { opacity: 1; transform: translateX(0)    scale(1); }
    to   { opacity: 0; transform: translateX(40px) scale(0.92); }
  }

  /* ── Spinner (for pending cmds) ── */
  @keyframes spin { to { transform: rotate(360deg); } }
  .spinner {
    display: inline-block;
    width: 12px; height: 12px;
    border: 2px solid rgba(255,255,255,0.2);
    border-top-color: var(--accent);
    border-radius: 50%;
    animation: spin 0.7s linear infinite;
    flex-shrink: 0;
  }

  /* ── Pulse dot animation for connected state ── */
  @keyframes pulse-dot {
    0%, 100% { box-shadow: 0 0 4px var(--ok); }
    50%       { box-shadow: 0 0 10px var(--ok), 0 0 18px var(--ok); }
  }
  #conn-dot.ok { animation: pulse-dot 2.5s ease-in-out infinite; }

  /* ── Responsive ── */
  @media (max-width: 480px) {
    #main { padding: 16px 12px 40px; }
    .stats-grid { grid-template-columns: repeat(2, 1fr); }
    .btn-grid   { grid-template-columns: repeat(2, 1fr); }
    #big-time-val { font-size: 52px; }
  }
</style>
</head>
<body>

<!-- Header -->
<header id="header">
  <button id="hamburger" aria-label="Menu" aria-expanded="false">
    <span></span><span></span><span></span>
  </button>
  <span id="header-title">&#9203; Clock Driver</span>
  <div id="conn-dot" title="WebSocket disconnected"></div>
  <div id="header-spacer"></div>
  <span id="header-time">--:--:--</span>
</header>

<!-- Backdrop -->
<div id="backdrop"></div>

<!-- Slide-in drawer -->
<nav id="drawer" aria-label="Navigation">
  <div id="drawer-inner">
    <div id="drawer-label">Navigation</div>
    <button class="nav-item active" data-section="clock">
      <span class="nav-icon">&#9200;</span> Clock
    </button>
    <button class="nav-item" data-section="status">
      <span class="nav-icon">&#128202;</span> Status
    </button>
    <button class="nav-item" data-section="network">
      <span class="nav-icon">&#127760;</span> Network
    </button>
    <button class="nav-item" data-section="lights">
      <span class="nav-icon">&#128161;</span> Lights
    </button>
    <button class="nav-item" data-section="system">
      <span class="nav-icon">&#9881;</span> System
    </button>
  </div>
</nav>

<!-- Main -->
<main id="main">

  <!-- ── Clock Section ── -->
  <section class="section active" id="sec-clock">
    <div class="section-header">
      <div class="section-title">Clock</div>
      <div class="section-sub">Motor position and real-time control</div>
    </div>

    <div class="card" id="big-time">
      <span id="big-time-val">--:--:--</span>
      <div id="big-date-val">Loading&hellip;</div>
    </div>

    <div class="card">
      <div class="card-title">Motor Status</div>
      <div class="stats-grid">
        <div class="stat-item">
          <div class="stat-label">Displayed Min</div>
          <div class="stat-value accent" id="sv-displayed-min">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Motor State</div>
          <div class="stat-value" id="sv-motor-powered">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">SNTP Sync</div>
          <div class="stat-value" id="sv-sntp">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Timezone</div>
          <div class="stat-value accent" id="sv-iana-tz">--</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Controls</div>
      <div class="btn-grid">
        <button class="btn primary" data-cmd="set-time">&#9654; Set Time</button>
        <button class="btn" data-cmd="advance">&#9193; Advance 1 Min</button>
        <button class="btn" data-cmd="step-fwd">&rarr; Step Fwd</button>
        <button class="btn" data-cmd="step-bwd">&larr; Step Bwd</button>
        <button class="btn" data-cmd="calibrate">&#127919; Calibrate</button>
        <button class="btn" data-cmd="measure">&#128225; Measure</button>
      </div>
    </div>
  </section>

  <!-- ── Status Section ── -->
  <section class="section" id="sec-status">
    <div class="section-header">
      <div class="section-title">Status</div>
      <div class="section-sub">Clock details and time synchronisation</div>
    </div>

    <div class="card">
      <div class="card-title">Clock Details</div>
      <div class="stats-grid">
        <div class="stat-item">
          <div class="stat-label">Displayed Min</div>
          <div class="stat-value accent" id="sv-displayed-min2">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Sensor Offset</div>
          <div class="stat-value" id="sv-sensor-offset">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Threshold</div>
          <div class="stat-value" id="sv-threshold">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Dark Mean</div>
          <div class="stat-value" id="sv-dark-mean">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Step Delay</div>
          <div class="stat-value" id="sv-step-delay">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Total Steps</div>
          <div class="stat-value" id="sv-total-steps">--</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Time Sync</div>
      <div class="stats-grid">
        <div class="stat-item">
          <div class="stat-label">Status</div>
          <div class="stat-value" id="sv-sntp2">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Time</div>
          <div class="stat-value accent" id="sv-time2">--:--:--</div>
        </div>
        <div class="stat-item" style="grid-column: span 2;">
          <div class="stat-label">IANA TZ</div>
          <div class="stat-value accent" id="sv-iana-tz2">--</div>
        </div>
      </div>
    </div>
  </section>

  <!-- ── Network Section ── -->
  <section class="section" id="sec-network">
    <div class="section-header">
      <div class="section-title">Network</div>
      <div class="section-sub">WiFi connection and geolocation info</div>
    </div>

    <div class="card">
      <div class="card-title">WiFi</div>
      <div class="stats-grid">
        <div class="stat-item">
          <div class="stat-label">Status</div>
          <div class="stat-value" id="sv-wifi-status">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">SSID</div>
          <div class="stat-value accent" id="sv-ssid">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">RSSI</div>
          <div class="stat-value" id="sv-rssi">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Local IP</div>
          <div class="stat-value accent" id="sv-local-ip">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Gateway</div>
          <div class="stat-value" id="sv-gateway">--</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Geolocation</div>
      <div class="stats-grid">
        <div class="stat-item">
          <div class="stat-label">External IP</div>
          <div class="stat-value accent" id="sv-ext-ip">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">City</div>
          <div class="stat-value" id="sv-city">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Region</div>
          <div class="stat-value" id="sv-region">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">ISP</div>
          <div class="stat-value" id="sv-isp">--</div>
        </div>
        <div class="stat-item" style="grid-column: span 2;">
          <div class="stat-label">Timezone</div>
          <div class="stat-value accent" id="sv-geo-tz">--</div>
        </div>
      </div>
    </div>
  </section>

  <!-- ── Lights Section ── -->
  <section class="section" id="sec-lights">
    <div class="section-header">
      <div class="section-title">Lights</div>
      <div class="section-sub">WS2812B strip control &mdash; both strips, independently extensible</div>
    </div>

    <div class="card">
      <div class="card-title">Strip Status</div>
      <div class="strip-row">
        <span class="strip-label">Strip 1</span>
        <span class="swatch" id="sv-led1-swatch"></span>
        <span class="strip-effect" id="sv-led1-effect">--</span>
        <span class="strip-bright" id="sv-led1-bright">--</span>
      </div>
      <div class="bright-wrap">
        <div class="bright-fill" id="sv-led1-bar" style="width:50%"></div>
      </div>
      <div class="strip-row">
        <span class="strip-label">Strip 2</span>
        <span class="swatch" id="sv-led2-swatch"></span>
        <span class="strip-effect" id="sv-led2-effect">--</span>
        <span class="strip-bright" id="sv-led2-bright">--</span>
      </div>
      <div class="bright-wrap">
        <div class="bright-fill" id="sv-led2-bar" style="width:50%"></div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Effects</div>
      <div class="btn-grid" id="effect-btn-grid">
        <button class="btn" data-cmd="led-off"    data-effect="Off">&#9898; Off</button>
        <button class="btn" data-cmd="led-static" data-effect="Static">&#11044; Static</button>
        <button class="btn" data-cmd="led-breathe" data-effect="Breathe">&#127744; Breathe</button>
        <button class="btn" data-cmd="led-rainbow" data-effect="Rainbow">&#127752; Rainbow</button>
        <button class="btn" data-cmd="led-chase"  data-effect="Chase">&#9889; Chase</button>
        <button class="btn" data-cmd="led-sparkle" data-effect="Sparkle">&#10024; Sparkle</button>
        <button class="btn" data-cmd="led-wipe"   data-effect="Wipe">&#9654;&#9654; Wipe</button>
        <button class="btn" data-cmd="led-comet"  data-effect="Comet">&#9732; Comet</button>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Brightness</div>
      <div class="btn-grid" style="grid-template-columns: 1fr 1fr 1fr;">
        <button class="btn" data-cmd="led-bright-down">&#9660; Dim</button>
        <button class="btn" data-cmd="led-next">&#8635; Next Effect</button>
        <button class="btn" data-cmd="led-bright-up">&#9650; Bright</button>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Colour Presets &mdash; Both Strips</div>
      <div class="btn-grid">
        <button class="btn" data-cmd="led-color-white">&#9898; White</button>
        <button class="btn" data-cmd="led-color-warm">&#127774; Warm</button>
        <button class="btn" data-cmd="led-color-red"   style="color:#f87171">&#9679; Red</button>
        <button class="btn" data-cmd="led-color-blue"  style="color:#60a5fa">&#9679; Blue</button>
        <button class="btn" data-cmd="led-color-green" style="color:#4ade80">&#9679; Green</button>
        <button class="btn" data-cmd="led-color-purple" style="color:#c084fc">&#9679; Purple</button>
      </div>
    </div>
  </section>

  <!-- ── System Section ── -->
  <section class="section" id="sec-system">
    <div class="section-header">
      <div class="section-title">System</div>
      <div class="section-sub">Hardware and runtime information</div>
    </div>

    <div class="card">
      <div class="card-title">System Stats</div>
      <div class="stats-grid">
        <div class="stat-item">
          <div class="stat-label">Uptime</div>
          <div class="stat-value accent" id="sv-uptime">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Free Heap</div>
          <div class="stat-value" id="sv-free-heap">--</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">Chip</div>
          <div class="stat-value ok">ESP32-S3</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">IDF Version</div>
          <div class="stat-value ok">v5.4.1</div>
        </div>
      </div>
    </div>

    <div class="card about-card">
      <div class="card-title">About</div>
      <p>
        <strong style="color:var(--text)">Clock Driver</strong> is an ESP32-S3
        firmware that drives an analog clock movement via a stepper motor.
        It synchronises time using SNTP over WiFi and automatically corrects
        the hand position using an optical position sensor near the 12&nbsp;o&rsquo;clock
        reference point.
      </p>
      <p>
        The web interface communicates with the device over a persistent WebSocket
        connection, receiving live status updates every second and sending control
        commands on demand.
      </p>
    </div>
  </section>

</main>

<!-- Toast container -->
<div id="toast-container"></div>

<script>
'use strict';

// ── Utility: format uptime ────────────────────────────────────────────────────
function fmtUptime(s) {
  s = Math.floor(s);
  if (s < 60)  return s + 's';
  if (s < 3600) {
    const m = Math.floor(s / 60);
    const r = s % 60;
    return m + 'm ' + String(r).padStart(2, '0') + 's';
  }
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return h + 'h ' + String(m).padStart(2, '0') + 'm';
}

// ── Utility: format heap ─────────────────────────────────────────────────────
function fmtHeap(b) {
  return (b / 1024).toFixed(1) + ' KB';
}

// ── Toast ─────────────────────────────────────────────────────────────────────
function showToast(msg, ok) {
  const container = document.getElementById('toast-container');
  const el = document.createElement('div');
  el.className = 'toast ' + (ok ? 'ok-toast' : 'err-toast');
  el.innerHTML =
    '<span class="toast-icon">' + (ok ? '&#10003;' : '&#10007;') + '</span>' +
    '<span class="toast-msg">' + msg + '</span>';
  container.appendChild(el);
  setTimeout(() => {
    el.classList.add('hide');
    setTimeout(() => el.remove(), 280);
  }, 3000);
}

// ── Navigation ────────────────────────────────────────────────────────────────
const sections  = {
  clock:   'sec-clock',
  status:  'sec-status',
  network: 'sec-network',
  lights:  'sec-lights',
  system:  'sec-system'
};
const navItems  = document.querySelectorAll('.nav-item');
const drawer    = document.getElementById('drawer');
const backdrop  = document.getElementById('backdrop');
const hamburger = document.getElementById('hamburger');

function setSection(name) {
  navItems.forEach(b => b.classList.toggle('active', b.dataset.section === name));
  Object.entries(sections).forEach(([key, id]) => {
    document.getElementById(id).classList.toggle('active', key === name);
  });
}

function closeDrawer() {
  document.body.classList.remove('drawer-open');
  hamburger.setAttribute('aria-expanded', 'false');
}

function toggleDrawer() {
  const open = document.body.classList.toggle('drawer-open');
  hamburger.setAttribute('aria-expanded', String(open));
}

hamburger.addEventListener('click', toggleDrawer);
backdrop.addEventListener('click', closeDrawer);

navItems.forEach(item => {
  item.addEventListener('click', () => {
    setSection(item.dataset.section);
    closeDrawer();
  });
});

// ── Stat element update helpers ───────────────────────────────────────────────
function setVal(id, text, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  if (cls !== undefined) {
    el.className = 'stat-value ' + (cls || '');
  }
}

function boolVal(v, trueStr, falseStr, trueCls, falseCls) {
  return v
    ? { text: trueStr  || 'Yes',  cls: trueCls  || 'ok' }
    : { text: falseStr || 'No',   cls: falseCls || 'err' };
}

function rssiClass(r) {
  if (r >= -60) return 'ok';
  if (r >= -75) return 'warn';
  return 'err';
}

// ── Apply LED data ─────────────────────────────────────────────────────────────
function applyLeds(leds) {
  if (!leds) return;

  function applyStrip(strip, effectId, swatchId, brightId, barId) {
    if (!strip) return;
    const el = document.getElementById(effectId);
    if (el) el.textContent = strip.effect || '--';

    const sw = document.getElementById(swatchId);
    if (sw) sw.style.background = 'rgb(' + (strip.r||0) + ',' + (strip.g||0) + ',' + (strip.b||0) + ')';

    const pct = Math.round((strip.brightness || 0) / 255 * 100);
    const brightEl = document.getElementById(brightId);
    if (brightEl) brightEl.textContent = pct + '%';
    const bar = document.getElementById(barId);
    if (bar) bar.style.width = pct + '%';
  }

  applyStrip(leds.strip1, 'sv-led1-effect', 'sv-led1-swatch', 'sv-led1-bright', 'sv-led1-bar');
  applyStrip(leds.strip2, 'sv-led2-effect', 'sv-led2-swatch', 'sv-led2-bright', 'sv-led2-bar');

  // Highlight active effect button
  const activeEffect = leds.strip1 ? leds.strip1.effect : null;
  document.querySelectorAll('#effect-btn-grid [data-effect]').forEach(btn => {
    btn.classList.toggle('active-effect', btn.dataset.effect === activeEffect);
  });
}

// ── Apply JSON data to all stat elements ──────────────────────────────────────
function applyData(d) {
  // Header live time (fallback if WS lag)
  if (d.time) {
    document.getElementById('header-time').textContent = d.time;
    document.getElementById('big-time-val').textContent = d.time;
    setVal('sv-time2', d.time, 'accent');
  }
  if (d.date) document.getElementById('big-date-val').textContent = d.date;

  // Clock card
  if (d.displayed_min !== undefined)
    setVal('sv-displayed-min',  d.displayed_min, 'accent');
  if (d.displayed_min !== undefined)
    setVal('sv-displayed-min2', d.displayed_min, 'accent');

  if (d.motor_powered !== undefined) {
    const v = boolVal(d.motor_powered, 'Powered', 'Off', 'ok', 'warn');
    setVal('sv-motor-powered', v.text, v.cls);
  }

  if (d.sntp !== undefined) {
    const v = boolVal(d.sntp, 'Synced', 'Unsynced', 'ok', 'warn');
    setVal('sv-sntp',  v.text, v.cls);
    setVal('sv-sntp2', v.text, v.cls);
  }

  if (d.iana_tz !== undefined) {
    const tz = d.iana_tz || '\u2014';
    setVal('sv-iana-tz',  tz, 'accent');
    setVal('sv-iana-tz2', tz, 'accent');
    setVal('sv-geo-tz',   tz, 'accent');
  }

  // Status card
  if (d.sensor_offset_sec !== undefined)
    setVal('sv-sensor-offset', d.sensor_offset_sec + 's', '');
  if (d.sensor_threshold !== undefined)
    setVal('sv-threshold', d.sensor_threshold, '');
  if (d.sensor_dark_mean !== undefined)
    setVal('sv-dark-mean', d.sensor_dark_mean, '');
  if (d.step_delay_us !== undefined)
    setVal('sv-step-delay', d.step_delay_us + ' \u03bcs', '');
  if (d.total_steps !== undefined)
    setVal('sv-total-steps', d.total_steps, 'accent');

  // Network
  if (d.wifi !== undefined) {
    const v = boolVal(d.wifi, 'Connected', 'Disconnected', 'ok', 'err');
    setVal('sv-wifi-status', v.text, v.cls);
  }
  if (d.ssid !== undefined)     setVal('sv-ssid',     d.ssid     || '\u2014', 'accent');
  if (d.rssi !== undefined)     setVal('sv-rssi',     d.rssi + ' dBm', rssiClass(d.rssi));
  if (d.local_ip !== undefined) setVal('sv-local-ip', d.local_ip || '\u2014', 'accent');
  if (d.gateway !== undefined)  setVal('sv-gateway',  d.gateway  || '\u2014', '');

  if (d.external_ip !== undefined) setVal('sv-ext-ip',  d.external_ip || '\u2014', 'accent');
  if (d.city        !== undefined) setVal('sv-city',    d.city        || '\u2014', '');
  if (d.region      !== undefined) setVal('sv-region',  d.region      || '\u2014', '');
  if (d.isp         !== undefined) setVal('sv-isp',     d.isp         || '\u2014', '');

  // System
  if (d.uptime_s  !== undefined) setVal('sv-uptime',    fmtUptime(d.uptime_s), 'accent');
  if (d.free_heap !== undefined) setVal('sv-free-heap', fmtHeap(d.free_heap), '');

  // LEDs
  if (d.leds) applyLeds(d.leds);
}

// ── WebSocket ─────────────────────────────────────────────────────────────────
let ws         = null;
let wsRetry    = 0;
let wsTimer    = null;
const dot      = document.getElementById('conn-dot');

function wsSetState(state) {
  dot.className = '';
  dot.title     = state === 'ok' ? 'WebSocket connected' : 'WebSocket disconnected';
  if (state === 'ok')  dot.classList.add('ok');
  if (state === 'err') dot.classList.add('err');
}

function fetchStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(d => applyData(d))
    .catch(() => {});
}

function wsConnect() {
  if (ws) { try { ws.close(); } catch(e) {} ws = null; }
  const url = 'ws://' + location.host + '/ws';
  ws = new WebSocket(url);

  ws.onopen = () => {
    wsRetry = 0;
    wsSetState('ok');
    fetchStatus();
  };

  ws.onmessage = (ev) => {
    try {
      const d = JSON.parse(ev.data);
      applyData(d);
    } catch(e) {}
  };

  ws.onerror = () => { wsSetState('err'); };

  ws.onclose = () => {
    wsSetState('err');
    ws = null;
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s, cap at 30s
    const delay = Math.min(1000 * Math.pow(2, wsRetry), 30000);
    wsRetry++;
    clearTimeout(wsTimer);
    wsTimer = setTimeout(wsConnect, delay);
  };
}

wsConnect();

// ── Local clock tick (keeps header time smooth between WS pushes) ─────────────
(function localTick() {
  const now = new Date();
  const hh  = String(now.getHours()).padStart(2, '0');
  const mm  = String(now.getMinutes()).padStart(2, '0');
  const ss  = String(now.getSeconds()).padStart(2, '0');
  const t   = hh + ':' + mm + ':' + ss;
  // Only update header (WS data takes precedence for big clock)
  document.getElementById('header-time').textContent = t;
  const msToNext = 1000 - now.getMilliseconds();
  setTimeout(localTick, msToNext);
})();

// ── Command buttons ───────────────────────────────────────────────────────────

// Colour preset commands resolved client-side to set-color API call
const COLOR_PRESETS = {
  'led-color-white':  [255, 255, 255],
  'led-color-warm':   [255, 180,  80],
  'led-color-red':    [255,   0,   0],
  'led-color-blue':   [  0,  80, 255],
  'led-color-green':  [  0, 220,  50],
  'led-color-purple': [180,   0, 255],
};

function sendCmd(cmd, btn) {
  const allBtns = document.querySelectorAll('[data-cmd]');
  allBtns.forEach(b => b.disabled = true);

  const origHTML = btn.innerHTML;
  btn.innerHTML = '<span class="spinner"></span> ' + origHTML;

  // Colour presets use the same /api/cmd endpoint with an rgb payload
  const body = COLOR_PRESETS[cmd]
    ? { cmd: 'led-color', r: COLOR_PRESETS[cmd][0], g: COLOR_PRESETS[cmd][1], b: COLOR_PRESETS[cmd][2] }
    : { cmd };

  fetch('/api/cmd', {
    method:  'POST',
    headers: { 'Content-Type': 'application/json' },
    body:    JSON.stringify(body),
  })
  .then(r => r.json())
  .then(d => {
    showToast(d.msg || (d.ok ? 'Done' : 'Error'), d.ok === true);
  })
  .catch(() => {
    showToast('Request failed', false);
  })
  .finally(() => {
    allBtns.forEach(b => b.disabled = false);
    btn.innerHTML = origHTML;
  });
}

document.querySelectorAll('[data-cmd]').forEach(btn => {
  btn.addEventListener('click', () => sendCmd(btn.dataset.cmd, btn));
});

// ── Fetch on page focus ───────────────────────────────────────────────────────
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') fetchStatus();
});
</script>
</body>
</html>
)html";
static const size_t WEB_UI_HTML_LEN = sizeof(WEB_UI_HTML) - 1;
