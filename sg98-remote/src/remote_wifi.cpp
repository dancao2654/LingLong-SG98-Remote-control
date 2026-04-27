#include "remote_wifi.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "remote_config.h"

namespace {

constexpr uint32_t kStaRetryMs = 15000;
constexpr uint16_t kDnsPort = 53;

WebServer server(80);
DNSServer dnsServer;
Preferences wifiPrefs;

String apSsid;
String staSsid;
String staPassword;
bool staEnabled = false;
bool setupDone = false;
bool wifiActive = false;
bool routesConfigured = false;
bool apRunning = false;
bool dnsRunning = false;
bool serverStarted = false;
uint32_t lastStaAttemptAt = 0;

const char kPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SG98 Remote WiFi Setup</title>
  <style>
    :root{
      --bg:#09111a;--panel:#102033;--panel2:#0d1828;--line:rgba(107,185,255,.22);
      --text:#eef6ff;--muted:#97afc5;--blue:#5bc3ff;--green:#55de95;--amber:#ffd466;
      --font-body:"Avenir Next","Segoe UI",sans-serif;--font-display:"DIN Condensed","Avenir Next Condensed","Arial Narrow",sans-serif;
    }
    *{box-sizing:border-box} body{margin:0;color:var(--text);font-family:var(--font-body);background:
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
    .value{font-size:1rem}
    .field{display:grid;gap:8px;margin-bottom:14px}
    input,button{font:inherit}
    input{width:100%;min-height:46px;padding:0 14px;border-radius:14px;border:1px solid rgba(134,186,234,.22);background:rgba(255,255,255,.05);color:var(--text)}
    .actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:16px 0}
    button{border:0;border-radius:14px;min-height:46px;padding:0 14px;background:var(--blue);color:#04121d;font-weight:800;cursor:pointer}
    button.secondary{background:rgba(255,255,255,.08);color:var(--text);border:1px solid rgba(134,186,234,.18)}
    .net-head{display:flex;justify-content:space-between;gap:12px;align-items:baseline}
    .net-title{font-size:1rem}
    .net-meta{color:var(--muted);font-size:.86rem}
    .hint{padding:12px 14px;border-radius:16px;background:rgba(91,195,255,.08);border:1px solid rgba(91,195,255,.16);color:var(--text)}
    @media (max-width:620px){.shell{width:min(100vw - 12px,720px);padding:10px 0 24px}.panel{padding:14px;border-radius:16px}.actions{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <div class="shell">
    <section class="panel">
      <div class="eyebrow">Remote Setup</div>
      <h1>SG98 Remote WiFi</h1>
      <p>Join the handheld remote to the same home Wi-Fi as the controller. Once both are on the same network, the remote will use WebSocket control automatically.</p>
      <div class="hint">If you leave the remote in AP-only mode, it will fall back to ESP-NOW instead of WebSocket.</div>
      <div class="status-grid">
        <div class="card"><span class="label">Remote AP</span><div id="apInfo" class="value">--</div></div>
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
      </div>
      <div id="scanState" class="hint">No scan yet.</div>
      <div id="scanResults" class="scan-list"></div>
    </section>
  </div>
  <script>
    const el = (id) => document.getElementById(id);
    let formEditUntil = 0;

    function markFormEditing(ms = 6000) {
      formEditUntil = Date.now() + ms;
    }

    function formEditingActive() {
      const active = document.activeElement;
      return Date.now() < formEditUntil || active === el('ssid') || active === el('password');
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

    function renderStatus(status) {
      el('apInfo').textContent = `${status.apSsid} @ ${status.apIp}`;
      el('staInfo').textContent = status.staConnected
        ? `${status.staSsid} @ ${status.staIp}`
        : (status.staEnabled ? `${status.staSsid} (${status.staStatus})` : 'Not configured');
      el('statusInfo').textContent = status.staConnected
        ? 'Connected and ready for WebSocket'
        : (status.setupDone ? 'Saved, waiting for LAN link' : 'Onboarding');
      if (!formEditingActive()) {
        el('ssid').value = status.staSsid || '';
      }
    }

    function renderScan(scan) {
      el('scanState').textContent = scan.scanning
        ? 'Scanning nearby networks...'
        : `${scan.count || 0} network${scan.count === 1 ? '' : 's'} found`;
      const results = el('scanResults');
      const networks = scan.networks || [];
      results.innerHTML = networks.length ? networks.map((net) => `
        <div class="net">
          <div class="net-head">
            <strong class="net-title">${net.ssid || '(hidden)'}</strong>
            <span class="net-meta">RSSI ${net.rssi} dBm</span>
          </div>
          <div class="net-meta">CH ${net.channel} · ${net.auth}</div>
          <div class="actions">
            <button data-ssid="${net.ssid || ''}">Use This SSID</button>
          </div>
        </div>`).join('') : '<div class="card">No scan results yet.</div>';
      results.querySelectorAll('[data-ssid]').forEach((button) => {
        button.onclick = () => {
          el('ssid').value = button.dataset.ssid || '';
          el('password').focus();
        };
      });
      if (scan.scanning) {
        setTimeout(() => refreshScan(false).catch(() => {}), 1400);
      }
    }

    async function refreshStatus() {
      renderStatus(await getJson('/api/status'));
    }

    async function refreshScan(force) {
      renderScan(await getJson(`/api/wifi_scan${force ? '?force=1' : ''}`));
    }

    async function saveWifi() {
      markFormEditing(0);
      await postForm('/api/wifi', {
        action: 'save',
        ssid: el('ssid').value || '',
        password: el('password').value || '',
      });
      el('password').value = '';
      await refreshStatus();
    }

    async function useApOnly() {
      markFormEditing(0);
      await postForm('/api/wifi', { action: 'ap_only' });
      el('password').value = '';
      await refreshStatus();
    }

    async function forgetWifi() {
      markFormEditing(0);
      await postForm('/api/wifi', { action: 'forget' });
      el('ssid').value = '';
      el('password').value = '';
      await refreshStatus();
    }

    ['ssid', 'password'].forEach((id) => {
      el(id).addEventListener('focus', () => markFormEditing());
      el(id).addEventListener('input', () => markFormEditing());
      el(id).addEventListener('keydown', () => markFormEditing());
    });

    el('saveButton').onclick = () => saveWifi().catch(() => {});
    el('apOnlyButton').onclick = () => useApOnly().catch(() => {});
    el('forgetButton').onclick = () => forgetWifi().catch(() => {});
    el('scanButton').onclick = () => refreshScan(true).catch(() => {});

    refreshStatus().catch(() => {});
    refreshScan(true).catch(() => {});
    setInterval(() => refreshStatus().catch(() => {}), 1500);
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
    wifiPrefs.begin("rmtwifi", false);
    staEnabled = wifiPrefs.getBool("staEnabled", false);
    setupDone = wifiPrefs.getBool("setupDone", false);
    staSsid = wifiPrefs.getString("ssid", "");
    staPassword = wifiPrefs.getString("password", "");
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
    if(!forceRetry && (lastStaAttemptAt + kStaRetryMs) > millis()) {
        return;
    }

    lastStaAttemptAt = millis();
    WiFi.begin(staSsid.c_str(), staPassword.c_str());
}

void stopSta() {
    WiFi.disconnect(false, false);
}

String statusJson() {
    String out;
    out.reserve(320);
    out += '{';
    out += "\"apSsid\":\"";
    out += jsonEscape(apSsid);
    out += "\",\"apIp\":\"";
    out += jsonEscape(apIpString());
    out += "\",\"staEnabled\":";
    out += staEnabled ? "true" : "false";
    out += ",\"setupDone\":";
    out += setupDone ? "true" : "false";
    out += ",\"staSsid\":\"";
    out += jsonEscape(staSsid);
    out += "\",\"staConnected\":";
    out += remote_wifi_sta_connected() ? "true" : "false";
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

String wifiScanJson(bool force) {
    int scanState = startWifiScan(force);
    if(scanState != WIFI_SCAN_RUNNING) {
        scanState = WiFi.scanComplete();
    }

    String out;
    out.reserve(1536);
    out += '{';
    out += "\"scanning\":";
    out += scanState == WIFI_SCAN_RUNNING ? "true" : "false";
    out += ",\"count\":";
    out += scanState > 0 ? String(scanState) : "0";
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

void sendJson(const String& body, int code = 200) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

void sendAck(bool ok, const String& message) {
    String out;
    out.reserve(96);
    out += "{\"ok\":";
    out += ok ? "true" : "false";
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
        serveAppleSuccess();
        return;
    }
    servePortalPage();
}

void handleWifi() {
    const String action = server.arg("action");
    if(server.method() == HTTP_GET || action.isEmpty()) {
        sendJson(statusJson());
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
    if(routesConfigured) {
        return;
    }

    server.on("/", HTTP_GET, servePortalPage);
    server.on("/hotspot-detect.html", HTTP_GET, serveCaptiveAwarePage);
    server.on("/library/test/success.html", HTTP_GET, serveCaptiveAwarePage);
    server.on("/generate_204", HTTP_GET, []() { if(setupDone) serveAndroidSuccess(); else servePortalPage(); });
    server.on("/gen_204", HTTP_GET, []() { if(setupDone) serveAndroidSuccess(); else servePortalPage(); });
    server.on("/connecttest.txt", HTTP_GET, []() { if(setupDone) serveAppleSuccess(); else servePortalPage(); });
    server.on("/redirect", HTTP_GET, serveCaptiveAwarePage);
    server.on("/canonical.html", HTTP_GET, serveCaptiveAwarePage);
    server.on("/ncsi.txt", HTTP_GET, []() { if(setupDone) serveNcsiSuccess(); else servePortalPage(); });
    server.on("/success.txt", HTTP_GET, []() { if(setupDone) serveAppleSuccess(); else servePortalPage(); });
    server.on("/api/status", HTTP_GET, []() { sendJson(statusJson()); });
    server.on("/api/wifi", HTTP_ANY, handleWifi);
    server.on("/api/wifi_scan", HTTP_GET, []() { sendJson(wifiScanJson(server.hasArg("force"))); });
    server.onNotFound([]() { servePortalPage(); });
    routesConfigured = true;
}

bool startAccessPoint() {
    if(apRunning) {
        return true;
    }

    WiFi.mode(WIFI_AP_STA);
    if(!WiFi.softAP(apSsid.c_str())) {
        return false;
    }
    dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
    dnsRunning = true;
    apRunning = true;
    Serial.printf("[remote_wifi] ap_on ssid=%s ip=%s\n", apSsid.c_str(), apIpString().c_str());
    return true;
}

void stopAccessPoint() {
    if(dnsRunning) {
        dnsServer.stop();
        dnsRunning = false;
    }
    if(apRunning) {
        WiFi.softAPdisconnect(true);
        apRunning = false;
        Serial.println("[remote_wifi] ap_off");
    }
}

void startPortalServer() {
    if(serverStarted) {
        return;
    }
    server.begin();
    serverStarted = true;
    Serial.println("[remote_wifi] portal_on");
}

void stopPortalServer() {
    if(!serverStarted) {
        return;
    }
    server.stop();
    serverStarted = false;
    Serial.println("[remote_wifi] portal_off");
}

}  // namespace

void remote_wifi_init() {
    const uint64_t chipId = ESP.getEfuseMac();
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06llX", static_cast<unsigned long long>(chipId & 0xFFFFFFULL));
    apSsid = String("SG98-REMOTE-") + suffix;

    loadStaConfig();
    configureRoutes();
}

bool remote_wifi_start() {
    if(wifiActive) {
        return true;
    }

    configureRoutes();
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(staEnabled && !remote_config::kDisableWifiModemSleepForTesting);
    startAccessPoint();
    beginStaConnect(true);
    if(!staEnabled || staSsid.isEmpty()) {
        startWifiScan(true);
    }
    startPortalServer();
    wifiActive = true;

    Serial.printf("[remote_wifi] ap_ssid=%s ap_ip=%s\n", apSsid.c_str(), apIpString().c_str());
    if(remote_wifi_has_saved_network()) {
        Serial.printf("[remote_wifi] sta_saved=%s\n", staSsid.c_str());
    } else {
        Serial.println("[remote_wifi] sta_saved=<none>");
    }
    Serial.printf("[remote_wifi] modem_sleep=%s\n",
                  remote_config::kDisableWifiModemSleepForTesting ? "off(test)" : "on");

    return true;
}

void remote_wifi_stop() {
    if(!wifiActive) {
        return;
    }

    stopAccessPoint();
    stopPortalServer();
    WiFi.scanDelete();
    stopSta();
    WiFi.mode(WIFI_MODE_NULL);
    lastStaAttemptAt = 0;
    wifiActive = false;
    Serial.println("[remote_wifi] stopped");
}

void remote_wifi_poll() {
    if(!wifiActive) {
        return;
    }

    const bool staConnected = staEnabled && WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
    if(staConnected) {
        if(apRunning) {
            stopAccessPoint();
        }
        if(serverStarted) {
            stopPortalServer();
        }
    } else {
        if(!apRunning) {
            startAccessPoint();
        }
        if(!serverStarted) {
            startPortalServer();
        }
        if(dnsRunning) {
            dnsServer.processNextRequest();
        }
    }

    if(serverStarted) {
        server.handleClient();
    }
    if(staEnabled && !staConnected) {
        beginStaConnect(false);
    }
}

bool remote_wifi_is_active() {
    return wifiActive;
}

bool remote_wifi_sta_connected() {
    if(!wifiActive) {
        return false;
    }
    return staEnabled && WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
}

bool remote_wifi_has_saved_network() {
    return staEnabled && !staSsid.isEmpty();
}

void remote_wifi_apply_network_settings(const String& ssid, const String& password, bool connectNow) {
    if(ssid.isEmpty()) {
        return;
    }

    staSsid = ssid;
    staPassword = password;
    staEnabled = true;
    setupDone = true;
    saveStaConfig();
    lastStaAttemptAt = 0;

    if(connectNow && wifiActive) {
        beginStaConnect(true);
    }
    Serial.printf("[remote_wifi] synced ssid=%s connect=%s\n",
                  staSsid.c_str(),
                  connectNow && wifiActive ? "now" : "deferred");
}

String remote_wifi_sta_ssid() {
    return staSsid;
}

String remote_wifi_sta_ip() {
    return remote_wifi_sta_connected() ? staIpString() : "";
}

String remote_wifi_ap_ssid() {
    return apSsid;
}

String remote_wifi_ap_ip() {
    return apIpString();
}

String remote_wifi_status_text() {
    if(!wifiActive) {
        return "WiFi off";
    }
    return remote_wifi_sta_connected() ? staIpString() : String("AP ") + apIpString();
}

void remote_wifi_reset_network_settings() {
    staEnabled = false;
    setupDone = false;
    staSsid = "";
    staPassword = "";
    saveStaConfig();
    stopSta();
    lastStaAttemptAt = 0;
    if(wifiActive) {
        startWifiScan(true);
    }
}
