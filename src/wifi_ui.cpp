#include "wifi_ui.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "diag_console.h"
#include "remote_ws.h"

namespace {
WebServer server(80);
DNSServer dnsServer;
Preferences wifiPrefs;

String apSsid;
bool staEnabled = false;
bool setupDone = false;
String staSsid;
String staPassword;
unsigned long lastStaAttemptAt = 0;
bool wifiUiRoutesConfigured = false;
bool wifiUiActive = false;

constexpr uint32_t STA_RETRY_MS = 15000;
constexpr uint16_t DNS_PORT = 53;

const char kPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SG98 WiFi Console</title>
  <style>
    :root {
      --bg:#0a121d; --panel:rgba(10,24,39,.9); --panel2:rgba(8,17,28,.9); --line:rgba(101,177,255,.18);
      --text:#edf5ff; --muted:#96afc8; --blue:#55c0ff; --green:#4de38d; --amber:#ffd463; --red:#ff6b6b;
      --radius:20px; --shadow:0 22px 58px rgba(0,0,0,.33); --font-body:"Avenir Next","Segoe UI",sans-serif;
      --font-display:"Avenir Next Condensed","DIN Condensed","Arial Narrow",sans-serif;
    }
    *{box-sizing:border-box} body{margin:0;color:var(--text);font-family:var(--font-body);background:
      radial-gradient(circle at top left, rgba(24,122,255,.24), transparent 34%),
      radial-gradient(circle at top right, rgba(19,215,178,.15), transparent 28%),
      linear-gradient(150deg,#08111c 0%,#10233a 45%,#09131f 100%);}
    .shell{width:min(1280px,calc(100vw - 12px));margin:0 auto;padding:8px 0 110px}
    .panel,.bottom-dock{border:1px solid var(--line);box-shadow:var(--shadow);backdrop-filter:blur(14px)}
    .eyebrow{margin:0 0 6px;color:var(--blue);text-transform:uppercase;letter-spacing:.24em;font-size:.8rem}
    h1{margin:0;font-family:var(--font-display);font-size:clamp(1.35rem,4.4vw,2.15rem);line-height:1;letter-spacing:.05em}
    button,input{font:inherit} button{border:0;border-radius:12px;padding:10px 12px;color:#04111d;background:var(--blue);cursor:pointer;font-weight:700}
    button.secondary{color:var(--text);background:rgba(88,137,179,.18);border:1px solid rgba(130,177,219,.2)}
    .badge,.chip{display:inline-flex;align-items:center;gap:8px;min-height:30px;padding:0 10px;border-radius:999px;font-weight:700;font-size:.88rem}
    .badge.ok{color:#04180e;background:var(--green)} .badge.bad{color:var(--text);background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.12)}
    .setup-overlay{position:fixed;inset:0;display:grid;place-items:center;padding:16px;background:rgba(3,10,18,.78);backdrop-filter:blur(12px);z-index:50}
    .setup-card{width:min(520px,100%);padding:22px;border-radius:24px;border:1px solid var(--line);background:linear-gradient(135deg,rgba(10,24,39,.98),rgba(8,17,28,.96));box-shadow:var(--shadow)}
    .setup-card h2{margin:0 0 10px;font-family:var(--font-display);font-size:2rem;letter-spacing:.06em}
    .setup-card p{margin:0 0 10px;color:var(--muted);line-height:1.5}
    .setup-url{display:block;margin:12px 0 16px;padding:12px 14px;border-radius:14px;background:rgba(255,255,255,.06);border:1px solid rgba(130,177,219,.16);color:var(--text);font-weight:700;word-break:break-all}
    .wizard-panel{margin-top:18px;padding:22px;border-radius:24px}
    .wizard-panel h2{margin:0 0 8px;font-family:var(--font-display);font-size:2rem;letter-spacing:.06em}
    .wizard-panel p{margin:0 0 16px;color:var(--muted);line-height:1.5}
    .wizard-actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
    .wizard-note{margin:0 0 14px;padding:12px 14px;border-radius:14px;background:rgba(255,255,255,.06);border:1px solid rgba(130,177,219,.16);color:var(--text)}
    .page-nav{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px}
    .page-btn{color:var(--text);background:rgba(255,255,255,.06);border:1px solid rgba(130,177,219,.16)}
    .page-btn.active{color:#04121e;background:linear-gradient(135deg,#70d5ff,#07a7ff)}
    .bottom-dock{position:fixed;left:8px;right:8px;bottom:max(8px,env(safe-area-inset-bottom));display:grid;gap:8px;padding:10px 12px;border-radius:18px;background:
      linear-gradient(135deg,rgba(14,37,61,.96),rgba(8,21,35,.92)),
      repeating-linear-gradient(-35deg,rgba(85,192,255,.04) 0 12px,rgba(12,33,52,.03) 12px 24px);z-index:30}
    .dock-meta{display:flex;align-items:center;justify-content:space-between;gap:10px}
    .dock-title{font-family:var(--font-display);font-size:.95rem;letter-spacing:.08em;text-transform:uppercase;color:var(--text)}
    .dock-badges{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
    .dock-actions{display:flex;gap:6px}
    .dock-actions button{flex:1}
    .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:10px;margin-top:10px}
    .hidden{display:none !important}
    .panel{padding:14px;border-radius:18px;background:var(--panel)}
    .panel-live{grid-column:1 / -1}.panel-status{grid-column:span 4}.panel-controls{grid-column:span 12}
    .panel-wifi{grid-column:span 4}.panel-menus{grid-column:span 8}.panel-log{grid-column:1 / -1}
    .panel-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px}
    .panel-head h2{margin:0;font-family:var(--font-display);font-size:1.08rem;letter-spacing:.08em}
    .status-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px}
    .status-card{padding:7px 8px;border-radius:12px;background:rgba(4,10,18,.32);border:1px solid rgba(130,177,219,.14)}
    .status-label{display:block;color:var(--muted);font-size:.62rem;margin-bottom:3px;letter-spacing:.12em;text-transform:uppercase}
    .status-value{font-size:.78rem;line-height:1.15}
    .live-screen{display:grid;grid-template-columns:minmax(0,1.45fr) minmax(160px,.6fr);gap:10px;padding:10px;border-radius:20px;background:
      linear-gradient(135deg,rgba(8,20,32,.95),rgba(10,31,49,.9)),
      repeating-linear-gradient(-40deg,rgba(85,192,255,.06) 0 18px,rgba(255,255,255,.02) 18px 36px);border:1px solid var(--line)}
    .live-screen.running{background:linear-gradient(135deg,rgba(8,28,24,.98),rgba(8,23,37,.9)),repeating-linear-gradient(-40deg,rgba(77,227,141,.07) 0 18px,rgba(255,255,255,.02) 18px 36px)}
    .live-screen.purging{background:linear-gradient(135deg,rgba(34,16,8,.98),rgba(15,29,46,.92)),repeating-linear-gradient(-40deg,rgba(255,212,99,.08) 0 18px,rgba(255,255,255,.02) 18px 36px)}
    .live-stage{display:grid;gap:12px}.live-stage-meta{display:flex;flex-wrap:wrap;gap:8px}
    .live-rpm-wrap,.gauge-card,.wifi-card{padding:12px 14px;border-radius:18px;border:1px solid rgba(130,177,219,.14);background:rgba(4,10,18,.35)}
    .live-kicker,.gauge-label,.field label{display:block;color:var(--muted);text-transform:uppercase;letter-spacing:.16em;font-size:.8rem}
    .live-rpm{font-family:var(--font-display);font-size:clamp(3rem,9vw,5.2rem);line-height:.9;letter-spacing:.04em;margin-top:6px}
    .live-target{margin-top:6px;color:var(--muted);font-size:.92rem}
    .live-bars{display:grid;gap:10px}.bar-block{display:grid;gap:8px}.bar-head{display:flex;justify-content:space-between;gap:12px;color:var(--muted);font-size:.86rem}
    .bar-head strong{color:var(--text)} .bar-track{overflow:hidden;height:18px;border-radius:999px;background:rgba(255,255,255,.08);border:1px solid rgba(130,177,219,.12)}
    .bar-fill{width:0;height:100%;border-radius:inherit;background:linear-gradient(90deg,#0aa6ff,#55c0ff);transition:width .18s ease}.bar-fill.rpm{background:linear-gradient(90deg,#2fd67b,#7df2b1)}
    .live-rpm-controls{display:grid;gap:8px}
    .mini-field{display:grid;gap:6px}
    .mini-field label{display:block;color:var(--muted);text-transform:uppercase;letter-spacing:.16em;font-size:.72rem}
    .mini-form{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;align-items:center}
    .mini-form button,.live-step-grid button{padding:8px 10px;border-radius:10px;font-size:.84rem}
    .live-step-grid{display:grid;grid-template-columns:repeat(6,minmax(0,1fr));gap:6px}
    .live-side{display:grid;grid-template-rows:auto 1fr;gap:10px}
    .live-run-card{display:flex;align-items:center;justify-content:center;min-height:108px;padding:8px;border-radius:18px;border:1px solid rgba(130,177,219,.14);background:rgba(4,10,18,.35)}
    .live-gauge-wrap{display:grid;grid-template-columns:1fr;gap:8px}.gauge-card{display:flex;align-items:center;justify-content:center;min-height:118px}
    .gauge{--progress:0deg;width:min(150px,100%);aspect-ratio:1;border-radius:50%;display:grid;place-items:center;background:
      radial-gradient(circle at center,rgba(4,10,18,.96) 0 56%,transparent 57%),
      conic-gradient(from -90deg,#0aa6ff var(--progress),rgba(255,255,255,.08) 0);border:1px solid rgba(130,177,219,.16)}
    .gauge.weight{background:radial-gradient(circle at center,rgba(4,10,18,.96) 0 56%,transparent 57%),conic-gradient(from -90deg,#4de38d var(--progress),rgba(255,255,255,.08) 0)}
    .gauge-inner{display:grid;gap:4px;text-align:center}.gauge-value{font-family:var(--font-display);font-size:1.8rem;line-height:1}
    .field{display:grid;gap:8px;margin-bottom:12px} input{width:100%;min-height:44px;border-radius:14px;border:1px solid rgba(130,177,219,.22);background:rgba(255,255,255,.06);color:var(--text);padding:0 14px}
    .inline-form,.button-grid,.wifi-actions,.menu-item .actions,.menu-item .value-row,.step-grid,.mode-grid,.primary-grid{display:flex;gap:10px;flex-wrap:wrap}
    .button-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}
    .mode-grid,.primary-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));margin-bottom:12px}
    .step-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr))}
    .control-stack{display:grid;gap:8px}
    .control-top-grid{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(0,.9fr);gap:8px}
    .control-card{padding:8px 10px;border-radius:14px;background:var(--panel2);border:1px solid var(--line)}
    .control-card h3{margin:0 0 6px;font-size:.82rem;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}
    .control-card.compact h3{margin-bottom:4px}
    .button-muted{color:var(--text);background:rgba(255,255,255,.08);border:1px solid rgba(130,177,219,.16)}
    .button-active{color:#04180e;background:var(--green)}
    .run-toggle-wrap{display:flex;justify-content:center;padding:4px 0 2px}
    .run-toggle{width:96px;height:96px;border-radius:50%;padding:0;display:grid;place-items:center;font-family:var(--font-display);font-size:1.15rem;letter-spacing:.08em;background:linear-gradient(135deg,#55c0ff,#0aa6ff);box-shadow:inset 0 1px 0 rgba(255,255,255,.2),0 10px 24px rgba(10,166,255,.25)}
    .run-toggle.stop{background:linear-gradient(135deg,#ffd463,#ff8a3d)}
    .tabs{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:16px}.tab{color:var(--text);background:rgba(255,255,255,.06)}.tab.active{color:#04121e;background:linear-gradient(135deg,#70d5ff,#07a7ff)}
    .menu-content{display:grid;gap:14px}.menu-item{display:grid;gap:10px;padding:16px;border-radius:18px;border:1px solid var(--line);background:var(--panel2)}
    .menu-head{display:flex;align-items:baseline;justify-content:space-between;gap:16px}.menu-title{margin:0;font-size:1.05rem}.menu-meta{color:var(--muted);font-size:.86rem}
    .chip{color:var(--muted);background:rgba(255,255,255,.06)} .chip.active{color:#04180e;background:var(--green)}
    .log-output{min-height:220px;max-height:52vh;overflow:auto;margin:0;padding:14px;border-radius:16px;background:rgba(3,10,18,.92);border:1px solid rgba(130,177,219,.12);color:#9fd6ff;font-family:"SFMono-Regular",Consolas,monospace;font-size:.85rem;line-height:1.45}
    @media (max-width:1100px){.panel-status,.panel-controls,.panel-wifi,.panel-menus,.panel-log{grid-column:1 / -1}.control-top-grid{grid-template-columns:1fr}.live-screen{grid-template-columns:minmax(0,1.6fr) minmax(124px,.72fr)}.live-side{grid-template-rows:auto 1fr}.live-gauge-wrap{grid-template-columns:1fr}}
    @media (max-width:700px){.shell{width:min(100vw - 6px,560px);padding:4px 0 106px}.wizard-actions{grid-template-columns:1fr}.panel{padding:8px;border-radius:14px}.live-screen{grid-template-columns:minmax(0,1.75fr) 100px;gap:8px;padding:8px}.live-stage{gap:6px}.live-rpm-wrap,.gauge-card,.wifi-card{padding:8px 10px}.live-rpm{font-size:clamp(1.9rem,11vw,3rem)}.live-target{font-size:.8rem}.bar-track{height:12px}.bar-head{font-size:.76rem}.gauge-card{min-height:62px;padding:6px}.gauge{width:min(62px,100%)}.gauge-value{font-size:1rem}.button-grid,.mode-grid,.primary-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.step-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.status-grid{grid-template-columns:repeat(3,minmax(0,1fr));gap:4px}.status-card{padding:6px 7px}.status-label{font-size:.58rem}.status-value{font-size:.72rem}.dock-meta{align-items:flex-start;flex-direction:column}.dock-badges,.dock-actions{width:100%}.run-toggle{width:72px;height:72px;font-size:.92rem}.live-run-card{min-height:82px;padding:6px}.live-gauge-wrap{grid-template-columns:1fr;gap:6px}.mini-form{grid-template-columns:minmax(0,1fr) 56px}.live-step-grid{grid-template-columns:repeat(3,minmax(0,1fr));gap:5px}.live-step-grid button{padding:7px 0;font-size:.76rem}.field{margin-bottom:8px}.control-card{padding:7px 8px}}
  </style>
</head>
<body>
  <div class="shell">
    <section id="wizardChoicePanel" class="panel wizard-panel hidden">
      <h2>Board Setup</h2>
      <p>Choose how you want to use the board. You can keep using the board hotspot directly, or join your home Wi-Fi and browse the board from your normal network.</p>
      <div class="wizard-actions">
        <button id="wizardJoinButton">Join Home Wi-Fi</button>
        <button id="wizardApOnlyButton" class="secondary">Use AP Only</button>
      </div>
    </section>

    <main id="controlView" class="grid">
      <section id="panelLive" class="panel panel-live">
        <div id="liveScreen" class="live-screen idle">
          <div class="live-stage">
            <div class="live-stage-meta">
              <span id="modeChip" class="chip">Idle</span>
              <span id="driveChip" class="chip">--</span>
              <span id="commChip" class="chip">--</span>
            </div>
            <div class="live-rpm-wrap">
              <span id="rpmKicker" class="live-kicker">Live RPM</span>
              <div id="liveRpm" class="live-rpm">0</div>
              <div id="targetRpm" class="live-target">Target 0</div>
            </div>
            <div class="live-bars">
              <div class="bar-block">
                <div class="bar-head"><span>Weight</span><strong id="weightText">0 / 0 mg</strong></div>
                <div class="bar-track"><div id="weightFill" class="bar-fill"></div></div>
              </div>
              <div class="bar-block">
                <div class="bar-head"><span>RPM Match</span><strong id="rpmMatchText">0%</strong></div>
                <div class="bar-track"><div id="rpmFill" class="bar-fill rpm"></div></div>
              </div>
            </div>
            <div class="live-rpm-controls">
              <div class="mini-field">
                <label for="rpmInput">Set RPM</label>
                <div class="mini-form">
                  <input id="rpmInput" type="number" min="0" step="10">
                  <button id="rpmApply">Set</button>
                </div>
              </div>
              <div class="live-step-grid">
                <button data-rpm-step="-10" class="button-muted">-10</button>
                <button data-rpm-step="-100" class="button-muted">-100</button>
                <button data-rpm-step="-500" class="button-muted">-500</button>
                <button data-rpm-step="10">+10</button>
                <button data-rpm-step="100">+100</button>
                <button data-rpm-step="500">+500</button>
              </div>
            </div>
          </div>
          <div class="live-side">
            <div class="live-run-card">
              <button id="runToggleButton" class="run-toggle">Start</button>
            </div>
            <div class="live-gauge-wrap">
              <div class="gauge-card"><div id="torqueGauge" class="gauge"><div class="gauge-inner"><span class="gauge-label">Torque</span><strong id="torqueText" class="gauge-value">0%</strong></div></div></div>
              <div class="gauge-card"><div id="doseGauge" class="gauge weight"><div class="gauge-inner"><span class="gauge-label">Dose</span><strong id="doseText" class="gauge-value">0%</strong></div></div></div>
            </div>
          </div>
        </div>
      </section>

      <section id="panelControls" class="panel panel-controls">
        <div class="control-stack">
          <div class="control-top-grid">
            <div class="control-card compact">
              <h3>Status</h3>
              <div id="statusGrid" class="status-grid"></div>
            </div>
            <div class="control-card compact">
              <h3>Mode</h3>
              <div class="mode-grid">
                <button id="modeNormalButton" class="button-muted" data-action="idle">Normal</button>
                <button id="modeGbwButton" class="button-muted" data-action="idle_gbw">GbW</button>
              </div>
            </div>
          </div>
          <div class="control-card compact">
            <h3>Dose</h3>
            <div class="field"><label for="weightInput">Set Weight (mg)</label><div class="inline-form"><input id="weightInput" type="number" min="0" step="100"><button id="weightApply">Set</button></div></div>
          </div>
        </div>
      </section>

      <section id="panelWifi" class="panel panel-wifi">
        <div class="panel-head"><h2>WiFi</h2></div>
        <div class="wifi-card">
          <div id="wizardWifiNote" class="wizard-note hidden">Step 2 of 3: choose your home Wi-Fi, enter the password, then tap Save + Connect.</div>
          <div class="field"><label>Board AP</label><div id="apInfo">--</div></div>
          <div class="field"><label>Home WiFi</label><div id="staInfo">--</div></div>
          <div class="field"><label>Provisioning Mode</label><div id="modeInfo">AP-only setup page. Pick a scanned SSID or stay on the board hotspot.</div></div>
          <div class="field"><label for="ssidInput">Home SSID</label><input id="ssidInput" type="text" autocomplete="off"></div>
          <div class="field"><label for="passwordInput">Home Password</label><input id="passwordInput" type="password" autocomplete="new-password"></div>
          <div class="wifi-actions">
            <button id="wifiSaveButton">Save + Connect</button>
            <button id="wifiApOnlyButton" class="secondary">Use AP Only</button>
            <button id="wifiForgetButton" class="secondary">Forget Saved</button>
          </div>
          <div class="field">
            <label>Nearby Networks</label>
            <div class="wifi-actions">
              <button id="scanButton" class="secondary">Scan SSIDs</button>
            </div>
            <div id="scanStatus" class="menu-meta">No scan yet.</div>
            <div id="scanResults" class="menu-content"></div>
          </div>
        </div>
      </section>

      <section id="panelMenus" class="panel panel-menus">
        <div class="panel-head"><h2>Menus</h2></div>
        <div id="menuTabs" class="tabs"></div>
        <div id="menuContent" class="menu-content"></div>
      </section>

    </main>

    <main id="consoleView" class="grid hidden">
      <section id="panelConsole" class="panel panel-log">
        <div class="panel-head"><h2>Serial Console</h2><button id="clearLogButton" class="secondary">Clear</button></div>
        <pre id="logOutput" class="log-output"></pre>
      </section>
    </main>
    <footer class="bottom-dock">
      <div class="dock-meta">
        <div id="heroTitle" class="dock-title">Onboard WiFi Console</div>
        <div class="dock-badges">
          <div id="apBadge" class="badge bad">AP offline</div>
          <div id="staBadge" class="badge bad">Home WiFi offline</div>
        </div>
      </div>
      <div class="page-nav" id="pageNav">
        <button id="navControl" class="page-btn">Control</button>
        <button id="navWifi" class="page-btn">WiFi</button>
        <button id="navMenus" class="page-btn">Menus</button>
        <button id="navConsole" class="page-btn">Console</button>
      </div>
    </footer>
  </div>
  <div id="setupOverlay" class="setup-overlay hidden">
    <div class="setup-card">
      <h2 id="setupTitle">Setup Complete</h2>
      <p id="setupMessage">The board is ready.</p>
      <div id="setupUrl" class="setup-url hidden"></div>
      <div class="wifi-actions">
        <button id="setupContinueButton">Continue</button>
        <button id="setupDismissButton" class="secondary">Dismiss</button>
      </div>
    </div>
  </div>
  <script>
    const state = {
      snapshot: null,
      activeMenu: 1,
      pollTimer: null,
      view: 'control',
      page: 'wifi',
      pageInitialized: false,
      wizard: { choice: '' },
      scanPollTimer: null,
      scan: null,
      wifiDraft: { ssid: '', password: '', dirty: false },
      pendingWifiSave: false,
      setupCloseCaptive: false,
      setupAutoCloseTimer: null,
    };
    const el = (id) => document.getElementById(id);
    const active = () => document.activeElement;

    function isEditingSelector(selector) {
      const node = active();
      return !!(node && selector && node.matches(selector));
    }

    function isEditingWithin(selector) {
      const node = active();
      return !!(node && node.closest(selector));
    }

    function log(msg) {
      const out = el('logOutput');
      out.textContent += `${msg}\n`;
      out.scrollTop = out.scrollHeight;
    }

    function isBoardApSession() {
      const host = window.location.hostname || '';
      return host === '192.168.4.1' || host === 'captive.apple.com' || host === 'www.apple.com';
    }

    function captiveSuccessUrl() {
      return 'http://captive.apple.com/hotspot-detect.html';
    }

    function clearSetupAutoClose() {
      if (state.setupAutoCloseTimer) {
        clearTimeout(state.setupAutoCloseTimer);
        state.setupAutoCloseTimer = null;
      }
    }

    function closeCaptiveSetup() {
      clearSetupAutoClose();
      window.location.assign(captiveSuccessUrl());
    }

    function showSetupOverlay(title, message, url = '', closeCaptive = false) {
      clearSetupAutoClose();
      state.setupCloseCaptive = closeCaptive;
      el('setupTitle').textContent = title;
      el('setupMessage').textContent = message;
      el('setupUrl').classList.toggle('hidden', !url);
      el('setupUrl').textContent = url;
      el('setupContinueButton').textContent = closeCaptive ? 'Close Setup' : 'Continue';
      el('setupOverlay').classList.remove('hidden');
      if (closeCaptive && isBoardApSession()) {
        state.setupAutoCloseTimer = setTimeout(() => closeCaptiveSetup(), 1400);
      }
    }

    function hideSetupOverlay() {
      clearSetupAutoClose();
      state.setupCloseCaptive = false;
      el('setupOverlay').classList.add('hidden');
    }

    function setWifiDraft(ssid, password = '', dirty = true) {
      state.wifiDraft.ssid = ssid || '';
      state.wifiDraft.password = password || '';
      state.wifiDraft.dirty = dirty;
      el('ssidInput').value = state.wifiDraft.ssid;
      el('passwordInput').value = state.wifiDraft.password;
    }

    function syncWifiDraftFromInputs() {
      state.wifiDraft.ssid = el('ssidInput').value || '';
      state.wifiDraft.password = el('passwordInput').value || '';
      state.wifiDraft.dirty = true;
    }

    function renderView() {
      const control = state.view !== 'console';
      el('controlView').classList.toggle('hidden', !control);
      el('consoleView').classList.toggle('hidden', control);
    }

    function renderHero() {
      const onboard = !state.snapshot?.wifi?.setupDone;
      if (onboard) {
        el('heroTitle').textContent = 'Onboard WiFi Console';
        return;
      }
      el('heroTitle').textContent = 'SG98 Control';
    }

    function setPage(page) {
      state.page = page;
      renderPages();
    }

    function renderPages() {
      const onboardingChoice = !!state.snapshot && !state.snapshot.wifi?.setupDone && !state.wizard.choice;
      const onboardingJoin = !!state.snapshot && !state.snapshot.wifi?.setupDone && state.wizard.choice === 'join';
      if (state.snapshot?.wifi?.setupDone && !state.pageInitialized) {
        state.page = 'control';
        state.pageInitialized = true;
      }
      if (!state.snapshot?.wifi?.setupDone) {
        state.pageInitialized = false;
      }
      el('wizardChoicePanel').classList.toggle('hidden', !onboardingChoice);
      el('pageNav').classList.toggle('hidden', !state.snapshot?.wifi?.setupDone);
      el('wizardWifiNote').classList.toggle('hidden', !onboardingJoin);

      if (onboardingChoice) {
        el('controlView').classList.add('hidden');
        el('consoleView').classList.add('hidden');
        return;
      }

      if (onboardingJoin) {
        state.page = 'wifi';
      }

      const panels = {
        control: ['panelLive', 'panelControls'],
        wifi: ['panelWifi'],
        menus: ['panelMenus'],
        console: ['panelConsole'],
      };
      const show = new Set(onboardingJoin ? ['panelWifi'] : (panels[state.page] || panels.wifi));
      ['panelLive', 'panelControls', 'panelWifi', 'panelMenus'].forEach((id) => {
        el(id).classList.toggle('hidden', !show.has(id));
      });
      el('consoleView').classList.toggle('hidden', state.page !== 'console');
      el('controlView').classList.toggle('hidden', state.page === 'console');
      [
        ['navControl','control'],
        ['navWifi','wifi'],
        ['navMenus','menus'],
        ['navConsole','console'],
      ].forEach(([id, page]) => {
        el(id).classList.toggle('active', state.page === page);
      });
    }

    async function getJson(url) {
      const response = await fetch(url, { cache: 'no-store' });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      return response.json();
    }

    async function postForm(url, body) {
      const response = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body: new URLSearchParams(body),
      });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      return response.json();
    }

    function clampPercent(value) { return Math.max(0, Math.min(100, Number.isFinite(value) ? value : 0)); }

    function renderStatusGrid(status) {
      const grid = el('statusGrid');
      const cards = [
        ['State', status.state],
        ['Drive', status.drive],
        ['Comm', status.comm],
        ['Motor', status.motor],
        ['Scale', status.scale],
        ['Error', `${status.error} ${status.errorText}`.trim()],
      ];
      grid.innerHTML = cards.map(([label, value]) => `<div class="status-card"><span class="status-label">${label}</span><strong class="status-value">${value}</strong></div>`).join('');
    }

    function renderLive(status) {
      const running = ['GRINDING','GRINDING_GBW','CALIBRATING'].includes(status.state);
      const purging = status.state === 'PURGING';
      el('liveScreen').className = `live-screen ${purging ? 'purging' : running ? 'running' : 'idle'}`;
      el('modeChip').textContent = status.state;
      el('driveChip').textContent = `${status.drive} ${status.motor}`;
      el('commChip').textContent = `${status.uart} / ${status.comm}`;
      const liveRpm = Math.abs(Number(status.currentRPM || 0));
      const targetRpm = Math.abs(Number(status.motorSetRPM || status.setRPM || 0));
      const torquePercent = clampPercent(Number(status.torquePercent || 0));
      const weight = Math.max(0, Number(status.weight || 0));
      const targetWeight = Math.max(0, Number(status.setWeight || 0));
      const weightPercent = targetWeight > 0 ? clampPercent((weight / targetWeight) * 100) : 0;
      const rpmMatch = targetRpm > 0 ? clampPercent((liveRpm / targetRpm) * 100) : 0;
      const showLivePrimary = running || purging;
      el('rpmKicker').textContent = showLivePrimary ? 'Live RPM' : 'Target RPM';
      el('liveRpm').textContent = showLivePrimary ? liveRpm : targetRpm;
      el('targetRpm').textContent = showLivePrimary ? `Target ${targetRpm}` : `Live ${liveRpm}`;
      el('weightText').textContent = `${weight} / ${targetWeight} mg`;
      el('rpmMatchText').textContent = `${Math.round(rpmMatch)}%`;
      el('torqueText').textContent = `${Math.round(torquePercent)}%`;
      el('doseText').textContent = `${Math.round(weightPercent)}%`;
      el('weightFill').style.width = `${weightPercent}%`;
      el('rpmFill').style.width = `${rpmMatch}%`;
      el('torqueGauge').style.setProperty('--progress', `${(torquePercent / 100) * 360}deg`);
      el('doseGauge').style.setProperty('--progress', `${(weightPercent / 100) * 360}deg`);
      if (!isEditingSelector('#rpmInput')) el('rpmInput').value = status.setRPM || 0;
      if (!isEditingSelector('#weightInput')) el('weightInput').value = status.setWeight || 0;
      const normalActive = status.state === 'IDLE' || status.state === 'GRINDING' || status.state === 'PURGING';
      const gbwActive = status.state === 'IDLE_GBW' || status.state === 'GRINDING_GBW';
      el('modeNormalButton').className = normalActive ? 'button-active' : 'button-muted';
      el('modeGbwButton').className = gbwActive ? 'button-active' : 'button-muted';
      el('runToggleButton').textContent = (running || purging) ? 'Stop' : 'Start';
      el('runToggleButton').className = (running || purging) ? 'run-toggle stop' : 'run-toggle';
    }

    function renderWifi(wifi) {
      el('apBadge').className = 'badge ok';
      el('apBadge').textContent = `AP ${wifi.apSsid}`;
      el('staBadge').className = `badge ${wifi.staConnected ? 'ok' : 'bad'}`;
      el('staBadge').textContent = wifi.staConnected ? `LAN ${wifi.staIp}` : 'Home WiFi offline';
      el('apInfo').textContent = `${wifi.apSsid} @ ${wifi.apIp}`;
      el('staInfo').textContent = wifi.staConnected ? `${wifi.staSsid} @ ${wifi.staIp}` : (wifi.staEnabled ? `${wifi.staSsid} (${wifi.staStatus})` : 'Not configured');
      el('modeInfo').textContent = wifi.staConnected ? `Connected to ${wifi.staSsid}. Browse the board at ${wifi.staIp}.` : 'AP-only setup page. Pick a scanned SSID or stay on the board hotspot.';
      if (state.wifiDraft.dirty || isEditingSelector('#ssidInput') || isEditingSelector('#passwordInput')) {
        if (!isEditingSelector('#ssidInput')) el('ssidInput').value = state.wifiDraft.ssid || '';
        if (!isEditingSelector('#passwordInput')) el('passwordInput').value = state.wifiDraft.password || '';
        return;
      }
      el('ssidInput').value = wifi.staSsid || '';
      el('passwordInput').value = '';
    }

    function authLabel(auth) {
      if (auth === 'OPEN') return 'Open';
      return auth;
    }

    function renderScan(scan) {
      state.scan = scan;
      el('scanStatus').textContent = scan.scanning
        ? 'Scanning nearby networks...'
        : `${scan.count || 0} network${scan.count === 1 ? '' : 's'} found`;
      const results = el('scanResults');
      const networks = scan.networks || [];
      results.innerHTML = networks.length
        ? networks.map((net) => `
            <div class="menu-item">
              <div class="menu-head">
                <h3 class="menu-title">${net.ssid || '(hidden)'}</h3>
                <div class="menu-meta">RSSI ${net.rssi} dBm · CH ${net.channel} · ${authLabel(net.auth)}</div>
              </div>
              <div class="actions">
                <button data-ssid="${net.ssid || ''}">Use This SSID</button>
              </div>
            </div>`).join('')
        : '<div class="menu-item">No networks listed yet.</div>';
      results.querySelectorAll('[data-ssid]').forEach((button) => {
        button.onclick = () => {
          setWifiDraft(button.dataset.ssid || '', el('passwordInput').value || '', true);
          el('passwordInput').focus();
        };
      });
    }

    async function refreshScan(force = false) {
      const scan = await getJson(`/api/wifi_scan${force ? '?force=1' : ''}`);
      renderScan(scan);
      if (state.scanPollTimer) {
        clearTimeout(state.scanPollTimer);
        state.scanPollTimer = null;
      }
      if (scan.scanning) {
        state.scanPollTimer = setTimeout(() => refreshScan(false).catch((error) => log(`[scan] ${error.message}`)), 1500);
      }
    }

    function menuById(id) { return (state.snapshot?.menus || []).find((menu) => menu.menu === id); }

    function renderMenuTabs() {
      const tabs = el('menuTabs');
      const menus = state.snapshot?.menus || [];
      if (!menus.find((menu) => menu.menu === state.activeMenu) && menus.length) state.activeMenu = menus[0].menu;
      tabs.innerHTML = menus.map((menu) => `<button class="tab ${state.activeMenu === menu.menu ? 'active' : ''}" data-menu="${menu.menu}">${menu.label}</button>`).join('');
      tabs.querySelectorAll('button[data-menu]').forEach((button) => {
        button.onclick = () => { state.activeMenu = Number(button.dataset.menu); renderMenus(); };
      });
    }

    function menuItemCard(menuId, item) {
      if (item.kind === 'nav' || item.kind === 'submenu') return '';
      const meta = item.kind === 'value'
        ? `${item.min}..${item.max}${item.unit ? ' ' + item.unit : ''} step ${item.scalar || 1}`
        : item.kind === 'bool' ? 'On / off'
        : item.kind === 'baud_action' ? `Target ${item.baud}`
        : item.kind;
      let body = '';
      if (item.kind === 'value') {
        body = `<div class="value-row"><input type="number" id="menu-${menuId}-${item.index}" min="${item.min}" max="${item.max}" step="${item.scalar || 1}" value="${item.value}"><button data-save="${menuId}:${item.index}">Save</button></div>`;
      } else if (item.kind === 'bool') {
        body = `<div class="actions"><label><input type="checkbox" id="menu-${menuId}-${item.index}" ${Number(item.value) ? 'checked' : ''}> Enabled</label><button data-save="${menuId}:${item.index}">Save</button></div>`;
      } else if (item.kind === 'action') {
        body = `<div class="actions"><button data-action="${item.action}">Run</button></div>`;
      } else if (item.kind === 'danger_action') {
        body = `<div class="actions"><button data-reset="1">Reset Defaults</button></div>`;
      } else if (item.kind === 'baud_action') {
        body = `<div class="actions"><button data-jmc-baud="${item.baud}">Switch</button><span class="chip ${item.activeBaud ? 'active' : ''}">${item.activeBaud ? '✓ ' : ''}${item.baud}</span></div>`;
      }
      return `<div class="menu-item"><div class="menu-head"><h3 class="menu-title">${item.name}</h3><div class="menu-meta">${meta}</div></div>${body}</div>`;
    }

    function renderMenus() {
      renderMenuTabs();
      const menu = menuById(state.activeMenu);
      el('menuContent').innerHTML = menu ? menu.items.map((item) => menuItemCard(menu.menu, item)).join('') : '<div class="menu-item">No menu data.</div>';

      document.querySelectorAll('[data-save]').forEach((button) => {
        button.onclick = async () => {
          const [menuId, itemIdx] = button.dataset.save.split(':').map(Number);
          const input = el(`menu-${menuId}-${itemIdx}`);
          const value = input.type === 'checkbox' ? (input.checked ? 1 : 0) : input.value;
          const response = await postForm('/api/set', { menu: menuId, item: itemIdx, value });
          log(`[set] ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
          await refresh();
        };
      });

      document.querySelectorAll('[data-action]').forEach((button) => {
        button.onclick = async () => {
          const response = await postForm('/api/action', { action: button.dataset.action });
          log(`[action] ${button.dataset.action} ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
          await refresh();
        };
      });

      document.querySelectorAll('[data-jmc-baud]').forEach((button) => {
        button.onclick = async () => {
          const response = await postForm('/api/action', { action: 'jmc_baud', arg0: button.dataset.jmcBaud });
          log(`[jmc_baud] ${button.dataset.jmcBaud} ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
          await refresh();
        };
      });

      document.querySelectorAll('[data-reset]').forEach((button) => {
        button.onclick = async () => {
          if (!confirm('Reset defaults and reboot the board?')) return;
          const response = await postForm('/api/action', { action: 'reset_defaults', arg0: 'confirm' });
          log(`[reset] ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
        };
      });
    }

    async function refresh() {
      const snapshot = await getJson('/api/snapshot');
      state.snapshot = snapshot;
      renderPages();
      renderHero();
      renderStatusGrid(snapshot.status);
      renderLive(snapshot.status);
      renderWifi(snapshot.wifi);
      if (state.pendingWifiSave && snapshot.wifi?.staConnected && snapshot.wifi?.staIp) {
        state.pendingWifiSave = false;
        state.wizard.choice = '';
        showSetupOverlay(
          'Home Wi-Fi Ready',
          'The board joined your home network. The captive setup page will now close. After that, switch your phone back to your home Wi-Fi and open the board URL below in Safari.',
          `http://${snapshot.wifi.staIp}/`,
          true
        );
      }
      if (!isEditingWithin('#menuContent') && !isEditingWithin('#menuTabs')) {
        renderMenus();
      }
    }

    async function setRpm() {
      const response = await postForm('/api/setrpm', { value: el('rpmInput').value || 0 });
      log(`[setrpm] ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
      await refresh();
    }

    async function nudgeRpm(delta) {
      const current = Number(el('rpmInput').value || state.snapshot?.status?.setRPM || 0);
      el('rpmInput').value = current + delta;
      await setRpm();
    }

    async function setWeight() {
      const response = await postForm('/api/setweight', { value: el('weightInput').value || 0 });
      log(`[setweight] ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
      await refresh();
    }

    async function actionButton(action, arg0 = '') {
      const response = await postForm('/api/action', { action, arg0 });
      log(`[action] ${action} ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
      await refresh();
    }

    async function toggleRun() {
      const status = state.snapshot?.status;
      if (!status) return;
      const running = ['GRINDING','GRINDING_GBW','CALIBRATING','PURGING'].includes(status.state);
      await actionButton(running ? 'stop' : 'start');
    }

    async function saveWifi() {
      const response = await postForm('/api/wifi', {
        action: 'save',
        ssid: el('ssidInput').value || '',
        password: el('passwordInput').value || '',
      });
      log(`[wifi] ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
      if (response.ok) {
        state.wifiDraft = { ssid: el('ssidInput').value || '', password: '', dirty: false };
        el('passwordInput').value = '';
        state.pendingWifiSave = true;
        showSetupOverlay(
          'Joining Home Wi-Fi',
          'The board is applying the Wi-Fi setting. Stay on this screen until the board gets a LAN IP, then switch your phone back to your home Wi-Fi and use the shown URL.',
          ''
        );
      }
      await refresh();
    }

    async function useApOnly() {
      const response = await postForm('/api/wifi', { action: 'ap_only' });
      log(`[wifi] ap_only ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
      if (response.ok) {
        state.wifiDraft = { ssid: '', password: '', dirty: false };
        el('ssidInput').value = '';
        el('passwordInput').value = '';
        state.pendingWifiSave = false;
        state.wizard.choice = '';
        showSetupOverlay(
          'AP-Only Mode Active',
          'The board will stay on its own hotspot. The captive setup page will now close. After that, open the board AP URL in Safari whenever you want to control it.',
          'http://192.168.4.1/',
          true
        );
      }
      await refresh();
    }

    async function forgetWifi() {
      const response = await postForm('/api/wifi', { action: 'forget' });
      log(`[wifi] forget ${response.ok ? 'ok' : 'fail'} ${response.message || ''}`.trim());
      if (response.ok) {
        state.wifiDraft = { ssid: '', password: '', dirty: false };
        el('ssidInput').value = '';
        el('passwordInput').value = '';
        state.wizard.choice = '';
      }
      await refresh();
    }

    el('wizardJoinButton').onclick = () => { state.wizard.choice = 'join'; setPage('wifi'); };
    el('wizardApOnlyButton').onclick = () => useApOnly().catch((error) => log(`[wifi] ${error.message}`));
    el('navControl').onclick = () => setPage('control');
    el('navWifi').onclick = () => setPage('wifi');
    el('navMenus').onclick = () => setPage('menus');
    el('navConsole').onclick = () => setPage('console');
    el('clearLogButton').onclick = () => { el('logOutput').textContent = ''; };
    el('setupDismissButton').onclick = () => hideSetupOverlay();
    el('setupContinueButton').onclick = () => {
      if (state.setupCloseCaptive && isBoardApSession()) {
        closeCaptiveSetup();
        return;
      }
      hideSetupOverlay();
      setPage('control');
    };
    el('rpmApply').onclick = () => setRpm().catch((error) => log(`[setrpm] ${error.message}`));
    el('weightApply').onclick = () => setWeight().catch((error) => log(`[setweight] ${error.message}`));
    el('runToggleButton').onclick = () => toggleRun().catch((error) => log(`[action] ${error.message}`));
    el('wifiSaveButton').onclick = () => saveWifi().catch((error) => log(`[wifi] ${error.message}`));
    el('wifiApOnlyButton').onclick = () => useApOnly().catch((error) => log(`[wifi] ${error.message}`));
    el('wifiForgetButton').onclick = () => forgetWifi().catch((error) => log(`[wifi] ${error.message}`));
    el('scanButton').onclick = () => refreshScan(true).catch((error) => log(`[scan] ${error.message}`));
    el('ssidInput').addEventListener('input', syncWifiDraftFromInputs);
    el('passwordInput').addEventListener('input', syncWifiDraftFromInputs);
    document.querySelectorAll('[data-action]').forEach((button) => {
      button.onclick = () => actionButton(button.dataset.action).catch((error) => log(`[action] ${error.message}`));
    });
    document.querySelectorAll('[data-drive]').forEach((button) => {
      button.onclick = () => actionButton('drive', button.dataset.drive).catch((error) => log(`[drive] ${error.message}`));
    });
    document.querySelectorAll('[data-rpm-step]').forEach((button) => {
      button.onclick = () => nudgeRpm(Number(button.dataset.rpmStep || 0)).catch((error) => log(`[setrpm] ${error.message}`));
    });

    document.querySelectorAll('[data-action],[data-drive]').forEach((button) => {
      button.onclick = () => actionButton(button.dataset.action || 'drive', button.dataset.drive || '').catch((error) => log(`[action] ${error.message}`));
    });

    renderView();
    renderPages();
    refresh().catch((error) => log(`[startup] ${error.message}`));
    refreshScan(true).catch((error) => log(`[scan] ${error.message}`));
    state.pollTimer = setInterval(() => refresh().catch((error) => log(`[poll] ${error.message}`)), 1200);
  </script>
</body>
</html>
)HTML";

String jsonEscape(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for(size_t i = 0; i < value.length(); ++i) {
        const char ch = value.charAt(i);
        switch(ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

String wifiStatusText() {
    switch(WiFi.status()) {
        case WL_CONNECTED: return "connected";
        case WL_NO_SSID_AVAIL: return "no_ssid";
        case WL_CONNECT_FAILED: return "connect_failed";
        case WL_CONNECTION_LOST: return "connection_lost";
        case WL_DISCONNECTED: return "disconnected";
        case WL_IDLE_STATUS: return "idle";
        default: return "unknown";
    }
}

String apIpString() {
    return WiFi.softAPIP().toString();
}

String staIpString() {
    return WiFi.localIP().toString();
}

void loadStaConfig() {
    wifiPrefs.begin("wifiui", false);
    staEnabled = wifiPrefs.getBool("staEnabled", false);
    setupDone = wifiPrefs.getBool("setupDone", false);
    staSsid = wifiPrefs.isKey("ssid") ? wifiPrefs.getString("ssid", "") : "";
    staPassword = wifiPrefs.isKey("password") ? wifiPrefs.getString("password", "") : "";
}

void saveStaConfig() {
    wifiPrefs.putBool("staEnabled", staEnabled);
    wifiPrefs.putBool("setupDone", setupDone);
    wifiPrefs.putString("ssid", staSsid);
    wifiPrefs.putString("password", staPassword);
}

void beginStaConnect(bool forceRetry = false) {
    if(!staEnabled || staSsid.isEmpty()) {
        return;
    }
    if(!forceRetry && WiFi.status() == WL_CONNECTED) {
        return;
    }
    if(!forceRetry && lastStaAttemptAt + STA_RETRY_MS > millis()) {
        return;
    }

    lastStaAttemptAt = millis();
    WiFi.begin(staSsid.c_str(), staPassword.c_str());
}

void stopSta() {
    WiFi.disconnect(false, false);
}

String wifiJson() {
    String out;
    out.reserve(320);
    out += '{';
    out += "\"apSsid\":\"";
    out += jsonEscape(apSsid);
    out += "\",\"apIp\":\"";
    out += jsonEscape(apIpString());
    out += "\",\"staEnabled\":";
    out += (staEnabled ? "true" : "false");
    out += ",\"setupDone\":";
    out += (setupDone ? "true" : "false");
    out += ",\"staSsid\":\"";
    out += jsonEscape(staSsid);
    out += "\",\"staConnected\":";
    out += (WiFi.status() == WL_CONNECTED ? "true" : "false");
    out += ",\"staIp\":\"";
    out += jsonEscape(staIpString());
    out += "\",\"staStatus\":\"";
    out += jsonEscape(wifiStatusText());
    out += "\"}";
    return out;
}

const char* authModeText(uint8_t authMode) {
    switch(authMode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "SECURED";
    }
}

int startWifiScan(bool force) {
    int scanState = WiFi.scanComplete();
    if(force && scanState >= 0) {
        WiFi.scanDelete();
        scanState = WiFi.scanComplete();
    }
    if(scanState == WIFI_SCAN_RUNNING) {
        return scanState;
    }
    if(force || scanState < 0) {
        scanState = WiFi.scanNetworks(true, true, false, 300, 0);
        if(scanState == WIFI_SCAN_RUNNING) {
            return scanState;
        }
    }
    return scanState;
}

String wifiScanJson(bool force = false) {
    int scanState = startWifiScan(force);
    if(scanState != WIFI_SCAN_RUNNING) {
        scanState = WiFi.scanComplete();
    }

    String out;
    out.reserve(1536);
    out += '{';
    out += "\"scanning\":";
    out += (scanState == WIFI_SCAN_RUNNING ? "true" : "false");
    out += ",\"count\":";
    out += (scanState > 0 ? String(scanState) : "0");
    out += ",\"networks\":[";

    if(scanState > 0) {
        for(int i = 0; i < scanState; ++i) {
            if(i > 0) out += ',';
            out += '{';
            out += "\"ssid\":\"";
            out += jsonEscape(WiFi.SSID(i));
            out += "\",\"rssi\":";
            out += String(WiFi.RSSI(i));
            out += ",\"channel\":";
            out += String(WiFi.channel(i));
            out += ",\"auth\":\"";
            out += authModeText(WiFi.encryptionType(i));
            out += "\"}";
        }
    }

    out += "]}";
    return out;
}

String snapshotJson() {
    String snapshot = diag_api_snapshot_json();
    if(snapshot.endsWith("}")) {
      snapshot.remove(snapshot.length() - 1);
      snapshot += ",\"wifi\":";
      snapshot += wifiJson();
      snapshot += "}";
    }
    return snapshot;
}

void sendJson(const String& body, int code = 200) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

void sendAck(bool ok, const String& message) {
    String out;
    out.reserve(128);
    out += "{\"ok\":";
    out += (ok ? "true" : "false");
    out += ",\"message\":\"";
    out += jsonEscape(message);
    out += "\"}";
    sendJson(out, ok ? 200 : 400);
}

void serveAppleSuccess() {
    static const char kAppleSuccessHtml[] PROGMEM =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kAppleSuccessHtml);
}

void serveCaptiveSuccess() {
    serveAppleSuccess();
}

void serveAndroidSuccess() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(204, "text/plain", "");
}

void serveNcsiSuccess() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", "Microsoft Connect Test");
}

void servePortalPage() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPage);
}

void serveCaptiveAwarePage() {
    if(setupDone) {
        serveCaptiveSuccess();
        return;
    }
    servePortalPage();
}

void handleSnapshot() {
    sendJson(snapshotJson());
}

void handleSet() {
    String message;
    const bool ok = diag_api_set_menu_value(static_cast<uint8_t>(server.arg("menu").toInt()),
                                            static_cast<uint8_t>(server.arg("item").toInt()),
                                            server.arg("value").toInt(),
                                            &message);
    sendAck(ok, message);
}

void handleSetRpm() {
    String message;
    const bool ok = diag_api_set_rpm(server.arg("value").toInt(), &message);
    sendAck(ok, message);
}

void handleSetWeight() {
    String message;
    const bool ok = diag_api_set_weight(server.arg("value").toInt(), &message);
    sendAck(ok, message);
}

void handleAction() {
    String message;
    const bool ok = diag_api_run_action(server.arg("action").c_str(),
                                        server.arg("arg0").c_str(),
                                        server.arg("arg1").c_str(),
                                        &message);
    sendAck(ok, message);
}

void handleWifi() {
    const String action = server.arg("action");
    if(server.method() == HTTP_GET || action.isEmpty()) {
        sendJson(wifiJson());
        return;
    }

    if(action == "save") {
        staSsid = server.arg("ssid");
        staPassword = server.arg("password");
        staEnabled = !staSsid.isEmpty();
        setupDone = staEnabled;
        saveStaConfig();
        if(staEnabled) {
            beginStaConnect(true);
            sendAck(true, "saved and connecting");
        } else {
            stopSta();
            sendAck(true, "cleared");
        }
        return;
    }

    if(action == "forget") {
        staEnabled = false;
        setupDone = false;
        staSsid = "";
        staPassword = "";
        saveStaConfig();
        stopSta();
        sendAck(true, "forgotten");
        return;
    }

    if(action == "ap_only") {
        staEnabled = false;
        setupDone = true;
        saveStaConfig();
        stopSta();
        sendAck(true, "ap only");
        return;
    }

    if(action == "reconnect") {
        beginStaConnect(true);
        sendAck(true, "reconnecting");
        return;
    }

    sendAck(false, "unknown wifi action");
}

void configureRoutes() {
    if(wifiUiRoutesConfigured) {
        return;
    }

    server.on("/", HTTP_GET, servePortalPage);
    server.on("/hotspot-detect.html", HTTP_GET, serveCaptiveAwarePage);
    server.on("/library/test/success.html", HTTP_GET, serveCaptiveAwarePage);
    server.on("/generate_204", HTTP_GET, []() { if(setupDone) serveAndroidSuccess(); else servePortalPage(); });
    server.on("/gen_204", HTTP_GET, []() { if(setupDone) serveAndroidSuccess(); else servePortalPage(); });
    server.on("/connecttest.txt", HTTP_GET, []() { if(setupDone) serveCaptiveSuccess(); else servePortalPage(); });
    server.on("/redirect", HTTP_GET, serveCaptiveAwarePage);
    server.on("/canonical.html", HTTP_GET, serveCaptiveAwarePage);
    server.on("/ncsi.txt", HTTP_GET, []() { if(setupDone) serveNcsiSuccess(); else servePortalPage(); });
    server.on("/success.txt", HTTP_GET, []() { if(setupDone) serveCaptiveSuccess(); else servePortalPage(); });
    server.on("/api/status", HTTP_GET, []() { sendJson(diag_api_status_json()); });
    server.on("/api/snapshot", HTTP_GET, handleSnapshot);
    server.on("/api/set", HTTP_POST, handleSet);
    server.on("/api/setrpm", HTTP_POST, handleSetRpm);
    server.on("/api/setweight", HTTP_POST, handleSetWeight);
    server.on("/api/action", HTTP_POST, handleAction);
    server.on("/api/wifi", HTTP_ANY, handleWifi);
    server.on("/api/wifi_scan", HTTP_GET, []() { sendJson(wifiScanJson(server.hasArg("force"))); });
    server.onNotFound([]() {
        if(server.uri().startsWith("/api/")) {
            sendAck(false, "not found");
            return;
        }
        servePortalPage();
    });
    wifiUiRoutesConfigured = true;
}
}

void wifi_ui_init() {
    const uint64_t chipId = ESP.getEfuseMac();
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06llX", static_cast<unsigned long long>(chipId & 0xFFFFFFULL));
    apSsid = String("SG98-") + suffix;

    loadStaConfig();
    configureRoutes();

    if(staEnabled && !staSsid.isEmpty()) {
        Serial.printf("[wifi] sta_saved=%s\n", staSsid.c_str());
    } else {
        Serial.println("[wifi] sta_saved=<none>");
    }
}

bool wifi_ui_start() {
    if(wifiUiActive) {
        return true;
    }

    configureRoutes();
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    // BLE is always initialized for the scale/console path, so WiFi must keep
    // modem sleep enabled for ESP32-S3 BT/WiFi coexistence.
    WiFi.setSleep(true);
    WiFi.softAP(apSsid.c_str());
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    beginStaConnect(true);
    startWifiScan(true);
    server.begin();
    remote_ws_start();
    wifiUiActive = true;

    Serial.printf("[wifi] ap_ssid=%s ap_ip=%s\n", apSsid.c_str(), apIpString().c_str());
    Serial.println("[wifi] modem_sleep=on for BLE coexistence");
    if(staEnabled && !staSsid.isEmpty()) {
        Serial.printf("[wifi] sta_saved=%s status=%s\n", staSsid.c_str(), wifiStatusText().c_str());
    } else {
        Serial.println("[wifi] sta_saved=<none>");
    }

    return true;
}

void wifi_ui_stop() {
    if(!wifiUiActive) {
        return;
    }

    dnsServer.stop();
    server.stop();
    remote_ws_stop();
    WiFi.scanDelete();
    WiFi.softAPdisconnect(false);
    stopSta();
    WiFi.mode(WIFI_MODE_NULL);
    lastStaAttemptAt = 0;
    wifiUiActive = false;
}

void wifi_ui_poll() {
    if(!wifiUiActive) {
        return;
    }

    dnsServer.processNextRequest();
    server.handleClient();
    remote_ws_poll();
    if(staEnabled && WiFi.status() != WL_CONNECTED) {
        beginStaConnect(false);
    }
}

bool wifi_ui_is_active() {
    return wifiUiActive;
}

bool wifi_ui_sta_connected() {
    if(!wifiUiActive) {
        return false;
    }
    return staEnabled && WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
}

String wifi_ui_sta_ip() {
    return wifi_ui_sta_connected() ? staIpString() : "";
}

String wifi_ui_lcd_status() {
    if(!wifiUiActive) {
        return "Web off";
    }
    return wifi_ui_sta_connected() ? staIpString() : "AP only";
}

void wifi_ui_reset_network_settings() {
    staEnabled = false;
    setupDone = false;
    staSsid = "";
    staPassword = "";
    saveStaConfig();
    stopSta();
    lastStaAttemptAt = 0;
    if(wifiUiActive) {
        startWifiScan(true);
    }
    Serial.println("[wifi] network settings reset to onboarding defaults");
}
