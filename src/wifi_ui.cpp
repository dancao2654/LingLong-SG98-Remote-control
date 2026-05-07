#include "wifi_ui.h"

#include <cstring>

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_interface.h>
#include <esp_netif.h>

#include "diag_console.h"
#include "espnow_control.h"
#include "gbw.h"
#include "remote_link.h"
#include "remote_ws.h"

esp_netif_t* get_esp_interface_netif(esp_interface_t interface);

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
bool apRunning = false;
bool dnsRunning = false;
bool serverStarted = false;
unsigned long lastWebActivityAt = 0;
bool pendingStaConnect = false;
bool pendingStaConfigBroadcast = false;
unsigned long pendingStaConnectAt = 0;
bool staConnectActive = false;
unsigned long staConnectStartedAt = 0;
uint32_t staConnectAttempts = 0;
uint8_t staDisconnectReason = 0;
char lastStaEvent[20] = "";
bool staDisconnectCleanupPending = false;
unsigned long staDisconnectCleanupAt = 0;
bool homeStaFirstActive = false;
unsigned long homeStaFirstUntil = 0;
bool pendingHttpServiceRefresh = false;
unsigned long pendingHttpServiceRefreshAt = 0;
bool captiveSetupComplete = false;
uint32_t captiveProbeHits = 0;
uint32_t captiveApiHits = 0;
uint32_t captiveSetupHits = 0;
uint32_t captiveRedirectHits = 0;
uint32_t rootHits = 0;
uint32_t controlHits = 0;
uint32_t wifiApiHits = 0;
uint32_t wifiScanDeferredHits = 0;
unsigned long wifiScanStartedAt = 0;
uint32_t apConnectEvents = 0;
uint32_t apDisconnectEvents = 0;
uint32_t apIpAssignEvents = 0;
uint32_t apDhcpWatchdogRestarts = 0;
unsigned long lastApClientConnectAt = 0;
unsigned long lastApClientIpAt = 0;
unsigned long lastApRestartAt = 0;
String lastCaptivePath;
static char captivePortalApiUri[80] = {};
char lastApClientMac[18] = "";
char lastApLeaseIp[16] = "";
char lastApEvent[16] = "";
bool apClientLeaseSeen = false;

constexpr uint32_t STA_RETRY_MS = 30000;
constexpr uint32_t STA_CONNECT_IN_FLIGHT_MS = 30000;
constexpr uint32_t STA_DISCONNECT_CLEANUP_DELAY_MS = 100;
constexpr uint32_t HOME_STA_FIRST_MS = 20000;
constexpr uint32_t WEB_ACTIVITY_HOLD_MS = 120000;
constexpr uint32_t STA_CONNECT_AFTER_HTTP_ACK_MS = 1200;
constexpr uint32_t AP_DHCP_LEASE_WATCHDOG_MS = 8000;
constexpr uint32_t AP_RESTART_COOLDOWN_MS = 15000;
constexpr uint32_t REMOTE_WS_MIN_FREE_HEAP = 10000;
constexpr uint16_t WIFI_SCAN_MAX_MS_PER_CHANNEL = 160;
constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 10000;
constexpr uint16_t DNS_PORT = 53;
constexpr int WIFI_SCAN_DEFERRED = -4;
constexpr bool kDisableWifiModemSleepForTesting = true;
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApSubnet(255, 255, 255, 0);
const IPAddress kApFirstLease(192, 168, 4, 2);
bool apEventsRegistered = false;

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
        : scan.deferred
          ? 'Scan paused while setup client is connected. Enter SSID manually.'
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
        : item.kind === 'bool' ? (item.name === 'Motor direction' ? 'Normal / reversed' : 'On / off')
        : item.kind === 'baud_action' ? `Target ${item.baud}`
        : item.kind;
      let body = '';
      if (item.kind === 'value') {
        body = `<div class="value-row"><input type="number" id="menu-${menuId}-${item.index}" min="${item.min}" max="${item.max}" step="${item.scalar || 1}" value="${item.value}"><button data-save="${menuId}:${item.index}">Save</button></div>`;
      } else if (item.kind === 'bool') {
        body = `<div class="actions"><label><input type="checkbox" id="menu-${menuId}-${item.index}" ${Number(item.value) ? 'checked' : ''}> ${item.name === 'Motor direction' ? 'Reversed' : 'Enabled'}</label><button data-save="${menuId}:${item.index}">Save</button></div>`;
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
      if (state.pendingWifiSave && snapshot.wifi?.staConnected && !snapshot.wifi?.captiveComplete && snapshot.wifi?.staIp) {
        showSetupOverlay(
          'Syncing Link',
          'Home Wi-Fi is online. Waiting for ESP-NOW and the first data sync before closing setup.',
          `http://${snapshot.wifi.staIp}/`,
          false
        );
      }
      if (state.pendingWifiSave && snapshot.wifi?.captiveComplete && snapshot.wifi?.staIp) {
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
          'The board will stay on its own hotspot. The setup page remains available from the board AP, and the control console is available at the URL below.',
          'http://192.168.4.1/control',
          false
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
    state.pollTimer = setInterval(() => refresh().catch((error) => log(`[poll] ${error.message}`)), 1200);
  </script>
</body>
</html>
)HTML";

const char kWifiSetupPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SG98 WiFi Setup</title>
  <style>
    :root{
      --bg:#09111a;--panel:#102033;--line:rgba(107,185,255,.22);
      --text:#eef6ff;--muted:#97afc5;--blue:#5bc3ff;--green:#55de95;
      --font-body:"Avenir Next","Segoe UI",sans-serif;--font-display:"DIN Condensed","Avenir Next Condensed","Arial Narrow",sans-serif;
    }
    *{box-sizing:border-box}
    body{margin:0;color:var(--text);font-family:var(--font-body);background:
      radial-gradient(circle at top left,rgba(40,144,255,.22),transparent 34%),
      linear-gradient(160deg,#08111b 0%,#0f2236 42%,#08111a 100%)}
    .shell{width:min(720px,calc(100vw - 20px));margin:0 auto;padding:18px 0 32px}
    .panel{padding:18px;border-radius:22px;background:rgba(13,24,40,.94);border:1px solid var(--line);box-shadow:0 24px 54px rgba(0,0,0,.32)}
    .eyebrow{margin:0 0 6px;color:var(--blue);letter-spacing:.22em;text-transform:uppercase;font-size:.78rem}
    h1{margin:0 0 10px;font-family:var(--font-display);font-size:clamp(2rem,7vw,3.2rem);line-height:.9;letter-spacing:.05em}
    p{margin:0 0 14px;color:var(--muted);line-height:1.55}
    .status-grid,.scan-list{display:grid;gap:10px}
    .status-grid{grid-template-columns:repeat(auto-fit,minmax(180px,1fr));margin:18px 0}
    .card,.net{padding:14px;border-radius:18px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08)}
    .label{display:block;color:var(--muted);letter-spacing:.14em;text-transform:uppercase;font-size:.72rem;margin-bottom:6px}
    .value{font-size:1rem;word-break:break-word}
    .field{display:grid;gap:8px;margin-bottom:14px}
    input,button{font:inherit}
    input{width:100%;min-height:46px;padding:0 14px;border-radius:14px;border:1px solid rgba(134,186,234,.22);background:rgba(255,255,255,.05);color:var(--text)}
    .actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:16px 0}
    button,a.button{border:0;border-radius:14px;min-height:46px;padding:0 14px;background:var(--blue);color:#04121d;font-weight:800;cursor:pointer}
    a.button{display:grid;place-items:center;text-decoration:none}
    button.secondary,a.button.secondary{background:rgba(255,255,255,.08);color:var(--text);border:1px solid rgba(134,186,234,.18)}
    .net-head{display:flex;justify-content:space-between;gap:12px;align-items:baseline}
    .net-title{font-size:1rem}.net-meta{color:var(--muted);font-size:.86rem}
    .hint{padding:12px 14px;border-radius:16px;background:rgba(91,195,255,.08);border:1px solid rgba(91,195,255,.16);color:var(--text)}
    @media (max-width:620px){.shell{width:min(100vw - 12px,720px);padding:10px 0 24px}.panel{padding:14px;border-radius:16px}.actions{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <div class="shell">
    <section class="panel">
      <div class="eyebrow">Controller Setup</div>
      <h1>SG98 Controller WiFi</h1>
      <p>Join the controller to your home WiFi. Once it is on your network, use the controller URL shown below for normal control.</p>
      <div class="hint">This captive page is only for network setup.</div>
      <div class="status-grid">
        <div class="card"><span class="label">Controller AP</span><div id="apInfo" class="value">--</div></div>
        <div class="card"><span class="label">Home WiFi</span><div id="staInfo" class="value">--</div></div>
        <div class="card"><span class="label">Status</span><div id="statusInfo" class="value">--</div></div>
      </div>
      <div class="field"><label class="label" for="ssid">Home SSID</label><input id="ssid" type="text" autocomplete="off"></div>
      <div class="field"><label class="label" for="password">Home Password</label><input id="password" type="password" autocomplete="new-password"></div>
      <div class="actions">
        <button id="saveButton">Save + Connect</button>
        <button id="apOnlyButton" class="secondary">Use AP Only</button>
        <button id="forgetButton" class="secondary">Forget Saved</button>
        <button id="scanButton" class="secondary">Scan Networks</button>
        <a class="button secondary" href="http://192.168.4.1/control">Control UI</a>
      </div>
      <div id="scanState" class="hint">No scan yet.</div>
      <div id="scanResults" class="scan-list"></div>
    </section>
  </div>
  <script>
    const el=(id)=>document.getElementById(id);
    let formEditUntil=0;
    let captiveCloseArmed=false;
    function markFormEditing(ms=6000){formEditUntil=Date.now()+ms}
    function formEditingActive(){const active=document.activeElement;return Date.now()<formEditUntil||active===el('ssid')||active===el('password')}
    function closeCaptiveWhenDone(){if(captiveCloseArmed)return;captiveCloseArmed=true;setTimeout(()=>{window.location.assign('http://captive.apple.com/hotspot-detect.html')},1200)}
    async function getJson(url){const response=await fetch(url,{cache:'no-store'});if(!response.ok)throw new Error(`${response.status} ${response.statusText}`);return response.json()}
    async function postForm(url,body){const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:new URLSearchParams(body)});if(!response.ok)throw new Error(`${response.status} ${response.statusText}`);return response.json()}
    function renderStatus(status){
      el('apInfo').textContent=`${status.apSsid} @ ${status.apIp}`;
      el('staInfo').textContent=status.staConnected?`${status.staSsid} @ ${status.staIp}`:(status.staEnabled?`${status.staSsid} (${status.staStatus})`:'Not configured');
      el('statusInfo').textContent=status.captiveComplete?'Ready. Closing setup...':(status.staConnected?`Connected: http://${status.staIp}/`:(status.setupDone?'Saved, waiting for LAN link':'Onboarding'));
      if(status.captiveComplete)closeCaptiveWhenDone();
      if(!formEditingActive())el('ssid').value=status.staSsid||'';
    }
    let scanRetryTimer=0;
    function scheduleScanRetry(delay=1700){if(scanRetryTimer)clearTimeout(scanRetryTimer);scanRetryTimer=setTimeout(()=>refreshScan(false),delay)}
    function renderScan(scan){
      if(scan.scanning)el('scanState').textContent='Scanning nearby networks...';
      else if(scan.deferred)el('scanState').textContent='Scan paused while setup client is connected. Enter SSID manually.';
      else if(scan.code<0)el('scanState').textContent=`Scan failed (${scan.code}). Tap Scan Networks again.`;
      else el('scanState').textContent=`${scan.count||0} network${scan.count===1?'':'s'} found`;
      const results=el('scanResults');
      const networks=scan.networks||[];
      results.innerHTML=networks.length?networks.map((net)=>`
        <div class="net">
          <div class="net-head"><strong class="net-title">${net.ssid||'(hidden)'}</strong><span class="net-meta">RSSI ${net.rssi} dBm</span></div>
          <div class="net-meta">CH ${net.channel} / ${net.auth}</div>
          <div class="actions"><button data-ssid="${net.ssid||''}">Use This SSID</button></div>
        </div>`).join(''):'<div class="card">No scan results yet.</div>';
      results.querySelectorAll('[data-ssid]').forEach((button)=>{
        button.onclick=()=>{el('ssid').value=button.dataset.ssid||'';el('password').focus()}
      });
      if(scan.scanning)scheduleScanRetry();
    }
    async function refreshStatus(){renderStatus(await getJson('/api/wifi'))}
    async function refreshScan(force){try{renderScan(await getJson(`/api/wifi_scan${force?'?force=1':''}`))}catch(error){el('scanState').textContent='Scanning... reconnecting to setup page';scheduleScanRetry(2200)}}
    async function saveWifi(){markFormEditing(0);await postForm('/api/wifi',{action:'save',ssid:el('ssid').value||'',password:el('password').value||''});el('password').value='';await refreshStatus()}
    async function useApOnly(){markFormEditing(0);await postForm('/api/wifi',{action:'ap_only'});el('password').value='';await refreshStatus()}
    async function forgetWifi(){markFormEditing(0);await postForm('/api/wifi',{action:'forget'});el('ssid').value='';el('password').value='';await refreshStatus()}
    ['ssid','password'].forEach((id)=>{el(id).addEventListener('focus',()=>markFormEditing());el(id).addEventListener('input',()=>markFormEditing());el(id).addEventListener('keydown',()=>markFormEditing())});
    el('saveButton').onclick=()=>saveWifi().catch(()=>{});
    el('apOnlyButton').onclick=()=>useApOnly().catch(()=>{});
    el('forgetButton').onclick=()=>forgetWifi().catch(()=>{});
    el('scanButton').onclick=()=>refreshScan(true);
    refreshStatus().catch(()=>{});
    setInterval(()=>refreshStatus().catch(()=>{}),1500);
  </script>
</body>
</html>
)HTML";

const char kWifiSetupPageLite[] = R"HTML(
<!doctype html><meta name=viewport content=width=device-width><style>body{background:#07111b;color:#eef;font:16px system-ui;padding:18px}input,button{width:100%;min-height:42px;margin:6px 0;font:inherit}button{background:#68c7ff}.n{background:#162536;color:#eef;text-align:left}#r{color:#9fb2c5}</style><h2>SG98 WiFi</h2><iframe name=x hidden></iframe><form id=f method=post action=/api/wifi target=x onsubmit="r.textContent='Saved. Connecting...'"><input type=hidden name=action value=save><input name=ssid placeholder=SSID><input name=password type=password placeholder=Password><button>Save + Connect</button></form><button id=b>Scan SSIDs</button><div id=r></div><script>async function q(v){r.textContent='Scanning...';try{let j=await(await fetch('/api/wifi_scan'+(v?'?force=1':''))).json();if(j.scanning)return setTimeout(q,900);let a=j.networks||[];r.innerHTML=a.length?a.map(n=>'<button class=n>'+n.ssid+'</button>').join(''):'No SSIDs found';document.querySelectorAll('.n').forEach(e=>e.onclick=()=>f.ssid.value=e.textContent)}catch(e){r.textContent='Scan failed'}}b.onclick=()=>q(1)</script>
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
    const IPAddress apIp = WiFi.softAPIP();
    return apIp[0] != 0 ? apIp.toString() : kApIp.toString();
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

void disconnectSta(bool eraseSdkConfig);
const char* staDisconnectReasonText(uint8_t reason);
bool timeReached(unsigned long dueAt);

void setLastStaEvent(const char* event) {
    strncpy(lastStaEvent, event != nullptr ? event : "", sizeof(lastStaEvent) - 1);
    lastStaEvent[sizeof(lastStaEvent) - 1] = '\0';
}

void scheduleStaDisconnectCleanup() {
    staDisconnectCleanupPending = true;
    staDisconnectCleanupAt = millis() + STA_DISCONNECT_CLEANUP_DELAY_MS;
}

bool staConnectInFlight() {
    return staConnectActive &&
           WiFi.status() != WL_CONNECTED &&
           (millis() - staConnectStartedAt) < STA_CONNECT_IN_FLIGHT_MS;
}

bool staConnectRetryCoolingDown() {
    return lastStaAttemptAt != 0 &&
           (millis() - lastStaAttemptAt) < STA_RETRY_MS;
}

bool savedHomeConfigured() {
    return staEnabled && setupDone && !staSsid.isEmpty();
}

bool homeStaFirstWindowActive() {
    return homeStaFirstActive && !timeReached(homeStaFirstUntil);
}

void beginStaConnect(bool forceRetry = false) {
    if(!staEnabled || staSsid.isEmpty()) {
        return;
    }
    const wl_status_t status = WiFi.status();
    if(!forceRetry && status == WL_CONNECTED) {
        staConnectActive = false;
        return;
    }
    if(!forceRetry && staConnectInFlight()) {
        return;
    }
    if(!forceRetry && staConnectRetryCoolingDown()) {
        return;
    }
    if((forceRetry && staConnectActive && status != WL_CONNECTED) ||
       (!forceRetry && lastStaAttemptAt != 0 && status != WL_CONNECTED)) {
        staDisconnectCleanupPending = false;
        WiFi.disconnect(false, false);
        delay(20);
    }

    lastStaAttemptAt = millis();
    staConnectActive = true;
    staDisconnectCleanupPending = false;
    staConnectStartedAt = lastStaAttemptAt;
    ++staConnectAttempts;
    staDisconnectReason = 0;
    setLastStaEvent("begin");
    WiFi.mode(apRunning ? WIFI_AP_STA : WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(!kDisableWifiModemSleepForTesting);
    Serial.printf("[wifi] sta_begin ssid=%s pass_len=%u force=%s status=%s\n",
                  staSsid.c_str(),
                  static_cast<unsigned>(staPassword.length()),
                  forceRetry ? "true" : "false",
                  wifiStatusText().c_str());
    WiFi.begin(staSsid.c_str(), staPassword.c_str());
}

void disconnectSta(bool eraseSdkConfig) {
    WiFi.setAutoReconnect(false);
    staConnectActive = false;
    staDisconnectCleanupPending = false;
    const wifi_mode_t mode = WiFi.getMode();
    if(mode == WIFI_MODE_NULL || mode == WIFI_AP) {
        return;
    }
    WiFi.disconnect(false, eraseSdkConfig);
}

void stopSta() {
    disconnectSta(false);
}

void forgetStaConnection() {
    pendingStaConnect = false;
    pendingStaConfigBroadcast = false;
    pendingHttpServiceRefresh = false;
    lastStaAttemptAt = 0;
    disconnectSta(true);
}

void scheduleStaConnectAfterAck(bool broadcastConfig) {
    pendingStaConnect = true;
    pendingStaConfigBroadcast = broadcastConfig;
    pendingStaConnectAt = millis() + STA_CONNECT_AFTER_HTTP_ACK_MS;
    lastStaAttemptAt = 0;
}

bool timeReached(unsigned long dueAt) {
    return static_cast<int32_t>(millis() - dueAt) >= 0;
}

void noteCaptiveRequest(const char* label) {
    lastCaptivePath = label != nullptr ? label : "";
    Serial.printf("[wifi] captive_req=%s uri=%s host=%s\n",
                  lastCaptivePath.c_str(),
                  server.uri().c_str(),
                  server.hostHeader().c_str());
}

String wifiJson() {
    String out;
    out.reserve(512);
    out += '{';
    out += "\"apSsid\":\"";
    out += jsonEscape(apSsid);
    out += "\",\"apIp\":\"";
    out += jsonEscape(apIpString());
    out += "\",\"active\":";
    out += (wifiUiActive ? "true" : "false");
    out += ",\"apRunning\":";
    out += (apRunning ? "true" : "false");
    out += ",\"dnsRunning\":";
    out += (dnsRunning ? "true" : "false");
    out += ",\"serverStarted\":";
    out += (serverStarted ? "true" : "false");
    out += ",\"wsActive\":";
    out += (remote_ws_is_active() ? "true" : "false");
    out += ",\"apStations\":";
    out += String(WiFi.softAPgetStationNum());
    out += ",\"apConnectEvents\":";
    out += String(apConnectEvents);
    out += ",\"apDisconnectEvents\":";
    out += String(apDisconnectEvents);
    out += ",\"apIpAssignEvents\":";
    out += String(apIpAssignEvents);
    out += ",\"apDhcpWatchdogRestarts\":";
    out += String(apDhcpWatchdogRestarts);
    out += ",\"lastApClientMac\":\"";
    out += jsonEscape(String(lastApClientMac));
    out += "\",\"lastApLeaseIp\":\"";
    out += jsonEscape(String(lastApLeaseIp));
    out += "\",\"lastApEvent\":\"";
    out += jsonEscape(String(lastApEvent));
    out += "\",\"apClientLeaseSeen\":";
    out += (apClientLeaseSeen ? "true" : "false");
    out += ",\"espnowHold\":";
    out += (espnow_control_is_wifi_client_active() ? "true" : "false");
    out += ",\"staEnabled\":";
    out += (staEnabled ? "true" : "false");
    out += ",\"setupDone\":";
    out += (setupDone ? "true" : "false");
    out += ",\"captiveComplete\":";
    out += (captiveSetupComplete ? "true" : "false");
    out += ",\"homeStaFirst\":";
    out += (homeStaFirstWindowActive() ? "true" : "false");
    out += ",\"staConnecting\":";
    out += (staConnectInFlight() ? "true" : "false");
    out += ",\"staConnectAttempts\":";
    out += String(static_cast<unsigned long>(staConnectAttempts));
    out += ",\"staPasswordLen\":";
    out += String(static_cast<unsigned>(staPassword.length()));
    out += ",\"staDisconnectReason\":";
    out += String(static_cast<unsigned>(staDisconnectReason));
    out += ",\"staDisconnectReasonText\":\"";
    out += jsonEscape(String(staDisconnectReasonText(staDisconnectReason)));
    out += "\"";
    out += ",\"lastStaEvent\":\"";
    out += jsonEscape(String(lastStaEvent));
    out += "\",\"staSsid\":\"";
    out += jsonEscape(staSsid);
    out += "\",\"staConnected\":";
    out += (WiFi.status() == WL_CONNECTED ? "true" : "false");
    out += ",\"staIp\":\"";
    out += jsonEscape(staIpString());
    out += "\",\"staStatus\":\"";
    out += jsonEscape(wifiStatusText());
    out += "\",\"captiveProbeHits\":";
    out += String(captiveProbeHits);
    out += ",\"captiveApiHits\":";
    out += String(captiveApiHits);
    out += ",\"captiveSetupHits\":";
    out += String(captiveSetupHits);
    out += ",\"captiveRedirectHits\":";
    out += String(captiveRedirectHits);
    out += ",\"rootHits\":";
    out += String(rootHits);
    out += ",\"controlHits\":";
    out += String(controlHits);
    out += ",\"wifiApiHits\":";
    out += String(wifiApiHits);
    out += ",\"wifiScanDeferredHits\":";
    out += String(wifiScanDeferredHits);
    out += ",\"lastCaptivePath\":\"";
    out += jsonEscape(lastCaptivePath);
    out += "\",\"freeHeap\":";
    out += String(ESP.getFreeHeap());
    out += ",\"minFreeHeap\":";
    out += String(ESP.getMinFreeHeap());
    out += "}";
    return out;
}

void formatMac(const uint8_t* mac, char* out, size_t outLen) {
    if(mac == nullptr || out == nullptr || outLen == 0) {
        return;
    }
    snprintf(out,
             outLen,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
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
    const bool apClientActive = apRunning && WiFi.softAPgetStationNum() > 0;
    if(scanState == WIFI_SCAN_RUNNING &&
       wifiScanStartedAt != 0 &&
       (millis() - wifiScanStartedAt) > WIFI_SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        wifiScanStartedAt = 0;
        scanState = WIFI_SCAN_FAILED;
        Serial.println("[wifi] scan_timeout_reset");
    }
    if(force) {
        WiFi.scanDelete();
        wifiScanStartedAt = 0;
        scanState = WiFi.scanComplete();
    }
    if(scanState == WIFI_SCAN_RUNNING) {
        return scanState;
    }
    if(scanState >= 0) {
        wifiScanStartedAt = 0;
    }
    if(apClientActive && !force) {
        return scanState >= 0 ? scanState : WIFI_SCAN_DEFERRED;
    }
    if(force || scanState < 0) {
        if(force && WiFi.status() != WL_CONNECTED) {
            staDisconnectCleanupPending = false;
            WiFi.disconnect(false, false);
            delay(20);
        }
        if(apClientActive) {
            ++wifiScanDeferredHits;
            Serial.println("[wifi] scan_async ap_client_active");
        }
        WiFi.mode(WIFI_AP_STA);
        scanState = WiFi.scanNetworks(true, true, false, apClientActive ? 80 : WIFI_SCAN_MAX_MS_PER_CHANNEL, 0);
        wifiScanStartedAt = (scanState == WIFI_SCAN_RUNNING) ? millis() : 0;
    }
    return scanState;
}

void ensureApEventLogging() {
    if(apEventsRegistered) {
        return;
    }
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t info) {
            const auto& event = info.wifi_ap_staconnected;
            ++apConnectEvents;
            lastApClientConnectAt = millis();
            lastWebActivityAt = lastApClientConnectAt;
            if(WiFi.softAPgetStationNum() <= 1) {
                apClientLeaseSeen = false;
            }
            formatMac(event.mac, lastApClientMac, sizeof(lastApClientMac));
            strncpy(lastApEvent, "connect", sizeof(lastApEvent) - 1);
            lastApEvent[sizeof(lastApEvent) - 1] = '\0';
            espnow_control_set_wifi_client_active(true);
            Serial.printf("[wifi] ap_client_connect mac=%s aid=%u\n",
                          lastApClientMac,
                          static_cast<unsigned>(event.aid));
        },
        WiFiEvent_t::ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t info) {
            const auto& event = info.wifi_ap_stadisconnected;
            ++apDisconnectEvents;
            formatMac(event.mac, lastApClientMac, sizeof(lastApClientMac));
            strncpy(lastApEvent, "disconnect", sizeof(lastApEvent) - 1);
            lastApEvent[sizeof(lastApEvent) - 1] = '\0';
            if(WiFi.softAPgetStationNum() == 0) {
                apClientLeaseSeen = false;
                espnow_control_set_wifi_client_active(false);
            }
            Serial.printf("[wifi] ap_client_disconnect mac=%s aid=%u reason=%u\n",
                          lastApClientMac,
                          static_cast<unsigned>(event.aid),
                          static_cast<unsigned>(event.reason));
        },
        WiFiEvent_t::ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t info) {
            const auto& event = info.wifi_ap_staipassigned;
            ++apIpAssignEvents;
            lastApClientIpAt = millis();
            lastWebActivityAt = lastApClientIpAt;
            apClientLeaseSeen = true;
            formatMac(event.mac, lastApClientMac, sizeof(lastApClientMac));
            strncpy(lastApLeaseIp, IPAddress(event.ip.addr).toString().c_str(), sizeof(lastApLeaseIp) - 1);
            lastApLeaseIp[sizeof(lastApLeaseIp) - 1] = '\0';
            strncpy(lastApEvent, "lease", sizeof(lastApEvent) - 1);
            lastApEvent[sizeof(lastApEvent) - 1] = '\0';
            espnow_control_set_wifi_client_active(true);
            Serial.printf("[wifi] dhcp_lease ip=%s mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                          lastApLeaseIp,
                          event.mac[0],
                          event.mac[1],
                          event.mac[2],
                          event.mac[3],
                          event.mac[4],
                          event.mac[5]);
        },
        WiFiEvent_t::ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t info) {
            staConnectActive = true;
            staDisconnectReason = 0;
            setLastStaEvent("connected");
            Serial.printf("[wifi] sta_connected channel=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                          static_cast<unsigned>(info.wifi_sta_connected.channel),
                          info.wifi_sta_connected.bssid[0],
                          info.wifi_sta_connected.bssid[1],
                          info.wifi_sta_connected.bssid[2],
                          info.wifi_sta_connected.bssid[3],
                          info.wifi_sta_connected.bssid[4],
                          info.wifi_sta_connected.bssid[5]);
        },
        WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t) {
            staConnectActive = false;
            homeStaFirstActive = false;
            staDisconnectReason = 0;
            setLastStaEvent("got_ip");
            Serial.printf("[wifi] sta_got_ip ip=%s rssi=%ld\n",
                          WiFi.localIP().toString().c_str(),
                          static_cast<long>(WiFi.RSSI()));
        },
        WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t info) {
            const uint8_t reason = info.wifi_sta_disconnected.reason;
            staConnectActive = false;
            if(reason != 8 || staDisconnectReason == 0) {
                staDisconnectReason = reason;
            }
            setLastStaEvent("disconnect");
            scheduleStaDisconnectCleanup();
            Serial.printf("[wifi] sta_disconnect reason=%u preserved=%u status=%s\n",
                          static_cast<unsigned>(reason),
                          static_cast<unsigned>(staDisconnectReason),
                          wifiStatusText().c_str());
        },
        WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    apEventsRegistered = true;
}

String wifiScanJson(bool force = false) {
    int scanState = startWifiScan(force);
    if(scanState != WIFI_SCAN_RUNNING && scanState != WIFI_SCAN_DEFERRED) {
        scanState = WiFi.scanComplete();
    }

    constexpr uint8_t MAX_WIFI_SCAN_RESULTS = 12;
    struct ScanEntry {
        int index = -1;
        int rssi = -999;
        String ssid;
    };
    ScanEntry entries[MAX_WIFI_SCAN_RESULTS];
    uint8_t networkCount = 0;

    if(scanState > 0) {
        for(int i = 0; i < scanState; ++i) {
            String ssid = WiFi.SSID(i);
            ssid.trim();
            if(ssid.isEmpty()) {
                continue;
            }

            const int rssi = WiFi.RSSI(i);
            int existing = -1;
            for(uint8_t j = 0; j < networkCount; ++j) {
                if(entries[j].ssid == ssid) {
                    existing = j;
                    break;
                }
            }

            if(existing >= 0) {
                if(rssi > entries[existing].rssi) {
                    entries[existing].index = i;
                    entries[existing].rssi = rssi;
                }
                continue;
            }

            if(networkCount < MAX_WIFI_SCAN_RESULTS) {
                entries[networkCount].index = i;
                entries[networkCount].rssi = rssi;
                entries[networkCount].ssid = ssid;
                ++networkCount;
                continue;
            }

            uint8_t weakest = 0;
            for(uint8_t j = 1; j < networkCount; ++j) {
                if(entries[j].rssi < entries[weakest].rssi) {
                    weakest = j;
                }
            }
            if(rssi > entries[weakest].rssi) {
                entries[weakest].index = i;
                entries[weakest].rssi = rssi;
                entries[weakest].ssid = ssid;
            }
        }

        for(uint8_t i = 0; i < networkCount; ++i) {
            for(uint8_t j = i + 1; j < networkCount; ++j) {
                if(entries[j].rssi > entries[i].rssi) {
                    const ScanEntry tmp = entries[i];
                    entries[i] = entries[j];
                    entries[j] = tmp;
                }
            }
        }
    }

    String out;
    out.reserve(1536);
    out += '{';
    out += "\"scanning\":";
    out += (scanState == WIFI_SCAN_RUNNING ? "true" : "false");
    out += ",\"deferred\":";
    out += (scanState == WIFI_SCAN_DEFERRED ? "true" : "false");
    out += ",\"status\":\"";
    out += (scanState == WIFI_SCAN_RUNNING ? "running" :
            (scanState == WIFI_SCAN_DEFERRED ? "deferred" :
             (scanState >= 0 ? "complete" : "failed")));
    out += "\",\"code\":";
    out += String(scanState);
    out += ",\"count\":";
    out += String(networkCount);
    out += ",\"networks\":[";

    if(networkCount > 0) {
        for(uint8_t i = 0; i < networkCount; ++i) {
            if(i > 0) out += ',';
            out += '{';
            out += "\"ssid\":\"";
            out += jsonEscape(entries[i].ssid);
            out += "\",\"rssi\":";
            out += String(entries[i].rssi);
            out += ",\"channel\":";
            out += String(WiFi.channel(entries[i].index));
            out += ",\"auth\":\"";
            out += authModeText(WiFi.encryptionType(entries[i].index));
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

void noteWebActivity() {
    lastWebActivityAt = millis();
    if(apRunning && WiFi.softAPgetStationNum() > 0) {
        lastApClientIpAt = lastWebActivityAt;
        apClientLeaseSeen = true;
    }
}

const char* staDisconnectReasonText(uint8_t reason) {
    switch(reason) {
        case 0: return "";
        case 2: return "auth_expire";
        case 4: return "assoc_expire";
        case 8: return "assoc_leave";
        case 15: return "4way_timeout";
        case 23: return "8021x_auth_failed";
        case 201: return "no_ap";
        case 202: return "auth_fail";
        case 203: return "assoc_fail";
        case 204: return "handshake_timeout";
        case 205: return "connection_fail";
        default: return "unknown";
    }
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

void serveEmptyIcon() {
    noteWebActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(204, "image/x-icon", "");
}

void serveNcsiSuccess() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", "Microsoft Connect Test");
}

bool shouldServeCaptivePortal();
void serveWifiSetupPage();

void serveAppleCaptiveResult(const char* label) {
    ++captiveProbeHits;
    noteCaptiveRequest(label);
    if(shouldServeCaptivePortal()) {
        serveWifiSetupPage();
        return;
    }
    serveAppleSuccess();
}

void serveAndroidCaptiveResult(const char* label) {
    ++captiveProbeHits;
    noteCaptiveRequest(label);
    if(shouldServeCaptivePortal()) {
        serveWifiSetupPage();
        return;
    }
    serveAndroidSuccess();
}

void serveNcsiCaptiveResult() {
    ++captiveProbeHits;
    noteCaptiveRequest("ncsi");
    if(shouldServeCaptivePortal()) {
        serveWifiSetupPage();
        return;
    }
    serveNcsiSuccess();
}

void serveCaptivePortalApi() {
    ++captiveApiHits;
    noteCaptiveRequest("api");
    server.sendHeader("Cache-Control", "no-store");
    if(!shouldServeCaptivePortal()) {
        server.send(200, "application/captive+json", "{\"captive\":false}");
        return;
    }
    server.send(200, "application/captive+json",
                "{\"captive\":true,\"user-portal-url\":\"http://192.168.4.1/setup\","
                "\"venue-info-url\":\"http://192.168.4.1/setup\"}");
}

bool writeHttpBodyWithRetry(const char* data, size_t length) {
    WiFiClient client = server.client();
    if(!client) {
        return false;
    }
    client.setNoDelay(true);
    size_t sent = 0;
    unsigned long lastProgressAt = millis();
    while(sent < length && (millis() - lastProgressAt) < 2500UL) {
        const size_t remaining = length - sent;
        const size_t chunk = remaining > 256 ? 256 : remaining;
        const size_t written = client.write(reinterpret_cast<const uint8_t*>(data + sent), chunk);
        if(written > 0) {
            sent += written;
            lastProgressAt = millis();
            continue;
        }
        delay(3);
        yield();
    }
    if(sent != length) {
        Serial.printf("[wifi] setup_body_short sent=%u len=%u\n",
                      static_cast<unsigned>(sent),
                      static_cast<unsigned>(length));
        return false;
    }
    return true;
}

void sendSetupHtml() {
    const size_t setupPageLength = sizeof(kWifiSetupPageLite) - 1;
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(setupPageLength);
    server.send(200, "text/html; charset=utf-8", "");
    server.setContentLength(CONTENT_LENGTH_NOT_SET);
    (void)writeHttpBodyWithRetry(kWifiSetupPageLite, setupPageLength);
}

void servePortalPage() {
    ++controlHits;
    lastCaptivePath = "control";
    Serial.printf("[wifi] http_req=control uri=%s host=%s\n", server.uri().c_str(), server.hostHeader().c_str());
    noteWebActivity();
    if(shouldServeCaptivePortal()) {
        serveWifiSetupPage();
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPage);
}

void serveWifiSetupPage() {
    ++captiveSetupHits;
    lastCaptivePath = "setup";
    Serial.printf("[wifi] http_req=setup uri=%s host=%s\n", server.uri().c_str(), server.hostHeader().c_str());
    noteWebActivity();
    sendSetupHtml();
}

bool shouldServeCaptivePortal() {
    const bool staConnected = staEnabled && WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
    if(captiveSetupComplete && setupDone && staConnected) {
        return false;
    }
    if(apRunning) {
        const IPAddress remoteIp = server.client().remoteIP();
        const IPAddress apIp = WiFi.softAPIP();
        if(remoteIp[0] == apIp[0] &&
           remoteIp[1] == apIp[1] &&
           remoteIp[2] == apIp[2]) {
            return true;
        }
    }
    return !setupDone || !staEnabled || staSsid.isEmpty() ||
           !(WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0);
}

void serveRootPage() {
    ++rootHits;
    lastCaptivePath = "root";
    Serial.printf("[wifi] http_req=root uri=%s host=%s\n", server.uri().c_str(), server.hostHeader().c_str());
    if(shouldServeCaptivePortal()) {
        serveWifiSetupPage();
        return;
    }
    servePortalPage();
}

void serveCaptivePortalPage() {
    sendSetupHtml();
}

void serveCaptiveSetupRedirect() {
    ++captiveRedirectHits;
    noteCaptiveRequest("setup");
    serveWifiSetupPage();
}

void serveCaptiveProbePage() {
    ++captiveProbeHits;
    noteCaptiveRequest("probe");
    if(shouldServeCaptivePortal()) {
        serveWifiSetupPage();
        return;
    }
    serveCaptiveSuccess();
}

void serveCaptiveAwarePage() {
    serveAppleCaptiveResult("aware");
}

void handleStatus() {
    noteWebActivity();
    remote_link_note_activity();
    sendJson(diag_api_status_json());
}

void handleSnapshot() {
    noteWebActivity();
    remote_link_note_activity();
    sendJson(snapshotJson());
}

void handleSet() {
    noteWebActivity();
    remote_link_note_activity();
    String message;
    const bool ok = diag_api_set_menu_value(static_cast<uint8_t>(server.arg("menu").toInt()),
                                            static_cast<uint8_t>(server.arg("item").toInt()),
                                            server.arg("value").toInt(),
                                            &message);
    sendAck(ok, message);
}

void handleSetRpm() {
    noteWebActivity();
    remote_link_note_activity();
    String message;
    const bool ok = diag_api_set_rpm(server.arg("value").toInt(), &message);
    sendAck(ok, message);
}

void handleSetWeight() {
    noteWebActivity();
    remote_link_note_activity();
    String message;
    const bool ok = diag_api_set_weight(server.arg("value").toInt(), &message);
    sendAck(ok, message);
}

void handleAction() {
    noteWebActivity();
    remote_link_note_activity();
    String message;
    const bool ok = diag_api_run_action(server.arg("action").c_str(),
                                        server.arg("arg0").c_str(),
                                        server.arg("arg1").c_str(),
                                        &message);
    sendAck(ok, message);
}

void handleWifi() {
    ++wifiApiHits;
    lastCaptivePath = "api_wifi";
    noteWebActivity();
    const String action = server.arg("action");
    if(server.method() == HTTP_GET || action.isEmpty()) {
        sendJson(wifiJson());
        return;
    }

    if(action == "save") {
        staSsid = server.arg("ssid");
        staSsid.trim();
        staPassword = server.arg("password");
        staEnabled = !staSsid.isEmpty();
        setupDone = staEnabled;
        captiveSetupComplete = false;
        pendingHttpServiceRefresh = false;
        saveStaConfig();
        if(staEnabled) {
            scheduleStaConnectAfterAck(true);
            sendAck(true, "saved and connecting");
        } else {
            forgetStaConnection();
            sendAck(true, "cleared");
        }
        return;
    }

    if(action == "forget") {
        staEnabled = false;
        setupDone = false;
        captiveSetupComplete = false;
        staSsid = "";
        staPassword = "";
        saveStaConfig();
        forgetStaConnection();
        sendAck(true, "forgotten");
        return;
    }

    if(action == "ap_only") {
        staEnabled = false;
        setupDone = true;
        captiveSetupComplete = false;
        saveStaConfig();
        forgetStaConnection();
        sendAck(true, "ap only");
        return;
    }

    if(action == "reconnect") {
        scheduleStaConnectAfterAck(false);
        sendAck(true, "reconnecting");
        return;
    }

    sendAck(false, "unknown wifi action");
}

void configureRoutes() {
    if(wifiUiRoutesConfigured) {
        return;
    }

    server.on("/", HTTP_GET, serveRootPage);
    server.on("/setup", HTTP_GET, serveWifiSetupPage);
    server.on("/control", HTTP_GET, servePortalPage);
    server.on("/hotspot-detect.html", HTTP_GET, []() { serveAppleCaptiveResult("hotspot"); });
    server.on("/library/test/success.html", HTTP_GET, []() { serveAppleCaptiveResult("apple"); });
    server.on("/generate_204", HTTP_GET, []() { serveAndroidCaptiveResult("generate_204"); });
    server.on("/gen_204", HTTP_GET, []() { serveAndroidCaptiveResult("gen_204"); });
    server.on("/connecttest.txt", HTTP_GET, []() { serveAppleCaptiveResult("connecttest"); });
    server.on("/redirect", HTTP_GET, []() { serveAppleCaptiveResult("redirect"); });
    server.on("/canonical.html", HTTP_GET, []() { serveAppleCaptiveResult("canonical"); });
    server.on("/ncsi.txt", HTTP_GET, serveNcsiCaptiveResult);
    server.on("/success.txt", HTTP_GET, []() { serveAppleCaptiveResult("success"); });
    server.on("/favicon.ico", HTTP_GET, serveEmptyIcon);
    server.on("/.well-known/captive-portal", HTTP_GET, serveCaptivePortalApi);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/snapshot", HTTP_GET, handleSnapshot);
    server.on("/api/set", HTTP_POST, handleSet);
    server.on("/api/setrpm", HTTP_POST, handleSetRpm);
    server.on("/api/setweight", HTTP_POST, handleSetWeight);
    server.on("/api/action", HTTP_POST, handleAction);
    server.on("/api/wifi", HTTP_ANY, handleWifi);
    server.on("/api/wifi_scan", HTTP_GET, []() {
        noteWebActivity();
        sendJson(wifiScanJson(server.hasArg("force")));
    });
    server.onNotFound([]() {
        if(server.uri().startsWith("/api/")) {
            sendAck(false, "not found");
            return;
        }
        serveRootPage();
    });
    wifiUiRoutesConfigured = true;
}

void configureSoftApNetwork() {
    if(!WiFi.softAPConfig(kApIp, kApIp, kApSubnet, kApFirstLease, kApIp)) {
        Serial.println("[wifi] ap_config_failed");
        return;
    }
    Serial.printf("[wifi] ap_config ip=%s lease_start=%s\n",
                  kApIp.toString().c_str(),
                  kApFirstLease.toString().c_str());
}

void enableDhcpCaptivePortalHint() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
    esp_netif_t* apNetif = get_esp_interface_netif(ESP_IF_WIFI_AP);
    if(apNetif == nullptr) {
        Serial.println("[wifi] dhcp_captive_hint=missing_netif");
        return;
    }

    snprintf(captivePortalApiUri,
             sizeof(captivePortalApiUri),
             "http://%s/.well-known/captive-portal",
             apIpString().c_str());

    esp_err_t err = esp_netif_dhcps_stop(apNetif);
    if(err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        Serial.printf("[wifi] dhcp_captive_hint=stop_failed err=0x%x\n", static_cast<unsigned>(err));
        return;
    }
    err = esp_netif_dhcps_option(apNetif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 captivePortalApiUri,
                                 strlen(captivePortalApiUri));
    if(err != ESP_OK) {
        Serial.printf("[wifi] dhcp_captive_hint=set_failed err=0x%x\n", static_cast<unsigned>(err));
        (void)esp_netif_dhcps_start(apNetif);
        return;
    }
    err = esp_netif_dhcps_start(apNetif);
    if(err != ESP_OK) {
        Serial.printf("[wifi] dhcp_captive_hint=start_failed err=0x%x\n", static_cast<unsigned>(err));
        return;
    }
    Serial.printf("[wifi] dhcp_captive_hint=%s\n", captivePortalApiUri);
#endif
}

bool startAccessPoint() {
    if(apRunning) {
        return true;
    }

    WiFi.mode(WIFI_AP_STA);
    ensureApEventLogging();
    configureSoftApNetwork();
    if(!WiFi.softAP(apSsid.c_str())) {
        return false;
    }
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    dnsRunning = true;
    apRunning = true;
    lastWebActivityAt = millis();
    strncpy(lastApEvent, "start", sizeof(lastApEvent) - 1);
    lastApEvent[sizeof(lastApEvent) - 1] = '\0';
    Serial.printf("[wifi] ap_on ssid=%s ap_ip=%s\n", apSsid.c_str(), apIpString().c_str());
    return true;
}

void stopHttpServices() {
    if(serverStarted) {
        server.stop();
        serverStarted = false;
    }
    remote_ws_stop();
}

void startHttpServices() {
    if(!serverStarted) {
        server.begin();
        serverStarted = true;
    }
    pendingHttpServiceRefresh = false;
}

void serviceRemoteWs(bool staConnected) {
    const bool apClientConnected = apRunning && WiFi.softAPgetStationNum() > 0;
    const bool shouldRun = (staConnected || apClientConnected) &&
                           ESP.getFreeHeap() >= REMOTE_WS_MIN_FREE_HEAP;
    if(!shouldRun) {
        if(remote_ws_is_active()) {
            remote_ws_stop();
        }
        return;
    }
    if(!remote_ws_is_active()) {
        (void)remote_ws_start();
    }
    remote_ws_poll();
}

void stopAccessPoint() {
    if(dnsRunning) {
        dnsServer.stop();
        dnsRunning = false;
    }
    if(apRunning) {
        espnow_control_set_wifi_client_active(false);
        WiFi.softAPdisconnect(true);
        apRunning = false;
        apClientLeaseSeen = false;
        strncpy(lastApEvent, "stop", sizeof(lastApEvent) - 1);
        lastApEvent[sizeof(lastApEvent) - 1] = '\0';
        Serial.println("[wifi] ap_off");
    }
}

void restartAccessPointForDhcpStall() {
    if(!apRunning || (millis() - lastApRestartAt) < AP_RESTART_COOLDOWN_MS) {
        return;
    }
    ++apDhcpWatchdogRestarts;
    lastApRestartAt = millis();
    Serial.printf("[wifi] dhcp_watchdog_observe count=%lu stations=%u last_mac=%s\n",
                  static_cast<unsigned long>(apDhcpWatchdogRestarts),
                  static_cast<unsigned>(WiFi.softAPgetStationNum()),
                  lastApClientMac);
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

    gbw_set_ble_paused_for_wifi(true);
    configureRoutes();
    captiveSetupComplete = false;
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(!kDisableWifiModemSleepForTesting);
    ensureApEventLogging();
    homeStaFirstActive = savedHomeConfigured();
    if(homeStaFirstActive) {
        homeStaFirstUntil = millis() + HOME_STA_FIRST_MS;
        WiFi.mode(WIFI_STA);
        Serial.printf("[wifi] sta_first ssid=%s window_ms=%lu\n",
                      staSsid.c_str(),
                      static_cast<unsigned long>(HOME_STA_FIRST_MS));
    } else {
        WiFi.mode(WIFI_AP_STA);
        startAccessPoint();
    }
    if(staEnabled) {
        beginStaConnect(true);
    }
    startHttpServices();
    wifiUiActive = true;

    Serial.printf("[wifi] ap_ssid=%s ap_ip=%s\n", apSsid.c_str(), apIpString().c_str());
    Serial.printf("[wifi] modem_sleep=%s\n", kDisableWifiModemSleepForTesting ? "off(test)" : "on");
    if(staEnabled && !staSsid.isEmpty()) {
        Serial.printf("[wifi] sta_saved=%s status=%s\n", staSsid.c_str(), wifiStatusText().c_str());
    } else {
        Serial.println("[wifi] sta_saved=<none>");
    }

    return true;
}

void wifi_ui_stop() {
    if(!wifiUiActive) {
        gbw_set_ble_paused_for_wifi(false);
        return;
    }

    stopHttpServices();
    stopAccessPoint();
    WiFi.scanDelete();
    wifiScanStartedAt = 0;
    stopSta();
    WiFi.mode(WIFI_MODE_NULL);
    lastStaAttemptAt = 0;
    staConnectActive = false;
    staDisconnectCleanupPending = false;
    homeStaFirstActive = false;
    pendingStaConnect = false;
    pendingStaConfigBroadcast = false;
    pendingHttpServiceRefresh = false;
    captiveSetupComplete = false;
    wifiUiActive = false;
    gbw_set_ble_paused_for_wifi(false);
}

void wifi_ui_poll() {
    if(!wifiUiActive) {
        return;
    }

    const bool staConnected = staEnabled && WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
    if(!staConnected) {
        captiveSetupComplete = false;
    }
    if(homeStaFirstActive && !staConnected && !homeStaFirstWindowActive()) {
        homeStaFirstActive = false;
        Serial.println("[wifi] sta_first_timeout ap_fallback=on");
    }
    const bool homeConfigured = savedHomeConfigured();
    const bool setupApNeeded = !homeConfigured || (!staConnected && !homeStaFirstWindowActive());
    if(setupApNeeded && !apRunning) {
        startAccessPoint();
    }
    if(homeConfigured &&
       staConnected &&
       apRunning &&
       WiFi.softAPgetStationNum() == 0 &&
       !wifi_ui_web_activity_recent()) {
        stopAccessPoint();
    }

    const uint8_t apStations = apRunning ? WiFi.softAPgetStationNum() : 0;
    espnow_control_set_wifi_client_active(apRunning && apStations > 0);
    const bool allowDhcpWatchdog = !staEnabled && !pendingStaConnect;
    const bool clientReachedHttp = lastWebActivityAt != 0 && lastWebActivityAt >= lastApClientConnectAt;
    if(allowDhcpWatchdog &&
       !apClientLeaseSeen &&
       !clientReachedHttp &&
       apStations > 0 &&
       (millis() - lastApClientConnectAt) >= AP_DHCP_LEASE_WATCHDOG_MS) {
        restartAccessPointForDhcpStall();
        return;
    }
    if(dnsRunning) {
        dnsServer.processNextRequest();
    }

    if(serverStarted) {
        server.handleClient();
    }
    if(staDisconnectCleanupPending && timeReached(staDisconnectCleanupAt)) {
        staDisconnectCleanupPending = false;
        if(staEnabled && WiFi.status() != WL_CONNECTED) {
            WiFi.disconnect(false, false);
            Serial.println("[wifi] sta_cleanup_after_disconnect");
        }
    }
    serviceRemoteWs(staConnected);
    if(pendingHttpServiceRefresh && apStations == 0 && timeReached(pendingHttpServiceRefreshAt)) {
        pendingHttpServiceRefresh = false;
        stopHttpServices();
        delay(20);
        startHttpServices();
        Serial.println("[wifi] http_services_refreshed");
        return;
    }
    if(pendingStaConnect) {
        if(!timeReached(pendingStaConnectAt)) {
            return;
        }
        pendingStaConnect = false;
        beginStaConnect(true);
        if(pendingStaConfigBroadcast) {
            espnow_control_broadcast_wifi_config(staSsid, staPassword);
            pendingStaConfigBroadcast = false;
        }
        return;
    }
    if(staEnabled && !staConnected) {
        if(WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
            return;
        }
        const bool apClientActive = apRunning && WiFi.softAPgetStationNum() > 0;
        const bool holdSetupClient = apClientActive && lastStaAttemptAt != 0 && !setupDone;
        if(holdSetupClient) {
            return;
        }
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

bool wifi_ui_ap_running() {
    return wifiUiActive && apRunning;
}

bool wifi_ui_setup_complete() {
    return setupDone;
}

bool wifi_ui_has_saved_network() {
    return staEnabled && !staSsid.isEmpty();
}

bool wifi_ui_home_setup_online() {
    return setupDone && staEnabled && !staSsid.isEmpty() && wifi_ui_sta_connected();
}

void wifi_ui_set_captive_setup_complete(bool complete) {
    captiveSetupComplete = complete && wifi_ui_home_setup_online();
}

uint8_t wifi_ui_ap_station_count() {
    return static_cast<uint8_t>(WiFi.softAPgetStationNum());
}

bool wifi_ui_web_activity_recent() {
    if(!wifiUiActive) {
        return false;
    }
    if(WiFi.softAPgetStationNum() > 0) {
        return true;
    }
    return lastWebActivityAt != 0 && (millis() - lastWebActivityAt) < WEB_ACTIVITY_HOLD_MS;
}

bool wifi_ui_web_idle_for(unsigned long quietMs) {
    if(!wifiUiActive || lastWebActivityAt == 0) {
        return false;
    }
    return (millis() - lastWebActivityAt) >= quietMs;
}

bool wifi_ui_save_network_settings(const String& ssid, const String& password) {
    staSsid = ssid;
    staSsid.trim();
    staPassword = password;
    staEnabled = !staSsid.isEmpty();
    setupDone = staEnabled;
    captiveSetupComplete = false;
    pendingHttpServiceRefresh = false;
    saveStaConfig();
    if(!staEnabled) {
        forgetStaConnection();
        return false;
    }
    if(wifiUiActive) {
        scheduleStaConnectAfterAck(true);
    }
    return true;
}

String wifi_ui_sta_ip() {
    return wifi_ui_sta_connected() ? staIpString() : "";
}

String wifi_ui_sta_ssid() {
    if(wifi_ui_sta_connected()) {
        const String connectedSsid = WiFi.SSID();
        if(!connectedSsid.isEmpty()) {
            return connectedSsid;
        }
    }
    return staSsid;
}

int32_t wifi_ui_sta_rssi() {
    return wifi_ui_sta_connected() ? WiFi.RSSI() : 0;
}

String wifi_ui_sta_status() {
    if(!wifiUiActive) {
        return "inactive";
    }
    if(!staEnabled || staSsid.isEmpty()) {
        return "not configured";
    }
    if(staConnectInFlight()) {
        return "connecting";
    }
    if(staDisconnectReason != 0) {
        return wifiStatusText() + String(" r") + String(static_cast<unsigned>(staDisconnectReason));
    }
    return wifiStatusText();
}

String wifi_ui_lcd_status() {
    if(!wifiUiActive) {
        return "Web off";
    }
    if(wifi_ui_sta_connected()) {
        return String("LAN ") + staIpString();
    }
    if(apRunning) {
        return String("AP ") + apIpString();
    }
    return "Joining Wi-Fi";
}

String wifi_ui_status_json() {
    return wifiJson();
}

String wifi_ui_scan_json(bool force) {
    return wifiScanJson(force);
}

void wifi_ui_reset_network_settings() {
    staEnabled = false;
    setupDone = false;
    captiveSetupComplete = false;
    captiveProbeHits = 0;
    captiveApiHits = 0;
    captiveSetupHits = 0;
    captiveRedirectHits = 0;
    rootHits = 0;
    controlHits = 0;
    wifiApiHits = 0;
    wifiScanDeferredHits = 0;
    apConnectEvents = 0;
    apDisconnectEvents = 0;
    apIpAssignEvents = 0;
    apDhcpWatchdogRestarts = 0;
    lastApClientConnectAt = 0;
    lastApClientIpAt = 0;
    lastApRestartAt = 0;
    apClientLeaseSeen = false;
    lastCaptivePath = "";
    lastApClientMac[0] = '\0';
    lastApLeaseIp[0] = '\0';
    lastApEvent[0] = '\0';
    pendingHttpServiceRefresh = false;
    staSsid = "";
    staPassword = "";
    saveStaConfig();
    forgetStaConnection();
    if(wifiUiActive) {
        stopHttpServices();
        stopAccessPoint();
        startAccessPoint();
        startHttpServices();
    }
    Serial.println("[wifi] network settings reset to onboarding defaults");
}
