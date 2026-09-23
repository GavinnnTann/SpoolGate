#include "portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include "../config.h"
#include "../logbuf.h"
#include "../net/health.h"
#include "../net/napt.h"
#include "../watchdog.h"

namespace portal {

namespace {

WebServer server(80);

// Single-session model: one operator at a time. A fresh login invalidates any prior
// session. Token is a 128-bit hex string from the hardware RNG.
String g_session;
uint32_t g_sessionExpiryMs = 0;
constexpr uint32_t kSessionTtlMs = 15UL * 60UL * 1000UL;  // 15 min inactivity

uint32_t g_rebootAtMs = 0;  // non-zero once a save has scheduled a reboot

// ---- Throughput test constants ------------------------------------------------

// Bytes per write in /speed/down. 4 KiB is large enough that the loop spends its
// time in the radio rather than in per-call overhead, and small enough that a
// client which walks away is noticed within one write.
constexpr size_t kSpeedBlockBytes = 4096;

// Size of one run, and the hard ceiling on both size and duration. The caps exist
// because the run blocks loop() from start to finish: NAPT reconciliation, the
// health probes, the channel-change watch and the status LED all stop until it
// returns. That is harmless for a few seconds — the loop watchdog only acts after
// five minutes — but it must not be open-ended, and a caller supplying ?mb= must
// not be able to make it so.
constexpr uint32_t kSpeedDefaultMiB = 8;
constexpr uint32_t kSpeedMaxMiB = 64;
constexpr uint32_t kSpeedMaxMs = 20000;

// Last run as the router measured it, surfaced in /stat.json. The client's own
// figure is the one to trust — it includes the air time of the final packets, which
// the router counts as sent the moment lwIP accepts them — but a wide disagreement
// between the two is itself the diagnosis: the router reporting 20 Mbit/s while the
// browser sees 3 means the bytes left here and died on the air.
uint32_t g_speedBytes = 0;
uint32_t g_speedMs = 0;

// ---- HTML ---------------------------------------------------------------------

const char kCss[] =
  "<style>"
  "*{box-sizing:border-box}"
  "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;"
  "background:#0f1115;color:#e6e6e6;line-height:1.5}"
  ".wrap{max-width:560px;margin:0 auto;padding:24px 18px 60px}"
  "h1{font-size:20px;margin:8px 0 2px}.sub{color:#8b93a1;font-size:13px;margin:0 0 22px}"
  "h2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:#8b93a1;"
  "border-bottom:1px solid #2a2f3a;padding-bottom:6px;margin:26px 0 12px}"
  "label{display:block;font-size:13px;margin:12px 0 4px;color:#c7ccd6}"
  "input,select{width:100%;padding:10px 12px;border:1px solid #2a2f3a;border-radius:8px;"
  "background:#171a21;color:#e6e6e6;font-size:15px}"
  "input:focus,select:focus{outline:none;border-color:#3b82f6}"
  ".hint{font-size:12px;color:#8b93a1;margin:4px 0 0}"
  "button{width:100%;margin-top:22px;padding:12px;border:0;border-radius:8px;"
  "background:#3b82f6;color:#fff;font-size:15px;font-weight:600;cursor:pointer}"
  "button.alt{background:#2a2f3a;margin-top:10px}"
  ".msg{padding:10px 12px;border-radius:8px;margin:0 0 16px;font-size:14px}"
  ".err{background:#3b1d1d;border:1px solid #7f1d1d;color:#fca5a5}"
  ".ok{background:#14321f;border:1px solid #166534;color:#86efac}"
  ".row{display:flex;gap:10px}.row>div{flex:1}"
  "a{color:#60a5fa}"
  ".tabs{display:flex;gap:6px;margin:18px 0 8px}"
  ".tab{flex:1;width:auto;margin-top:0;padding:9px;border:1px solid #2a2f3a;border-radius:8px;"
  "background:#171a21;color:#c7ccd6;font-size:14px;font-weight:500;cursor:pointer}"
  ".tab.active{background:#3b82f6;border-color:#3b82f6;color:#fff}"
  ".pane{display:none}"
  "input[type=range]{padding:0;height:28px}"
  ".big{font-size:22px;font-weight:600}"
  ".kv{display:flex;justify-content:space-between;gap:12px;padding:6px 0;"
  "border-bottom:1px solid #1c212b;font-size:13px}"
  ".kv span{color:#8b93a1}.kv b{font-weight:600;text-align:right;word-break:break-all}"
  ".up{color:#86efac}.down{color:#fca5a5}"
  "pre.log{background:#0a0c10;border:1px solid #2a2f3a;border-radius:8px;padding:12px;"
  "font:12px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace;color:#c7ccd6;"
  "white-space:pre-wrap;word-break:break-word;max-height:62vh;overflow-y:auto;margin:0}"
  "</style>";

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String pageHead(const char *title) {
  String h = F("<!doctype html><html><head><meta charset=utf-8>"
               "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>");
  h += title;
  h += F("</title>");
  h += kCss;
  h += F("</head><body><div class=wrap>");
  return h;
}

const char kPageFoot[] = "</div></body></html>";

// ---- Access control -----------------------------------------------------------

bool fromLan() {
  IPAddress r = server.client().remoteIP();
  const IPAddress &ap = config::settings.apIp;
  return r[0] == ap[0] && r[1] == ap[1] && r[2] == ap[2];
}

String makeToken() {
  static const char hex[] = "0123456789abcdef";
  char buf[33];
  for (int i = 0; i < 32; ++i) {
    buf[i] = hex[esp_random() & 0x0F];
  }
  buf[32] = '\0';
  return String(buf);
}

bool isAuthed() {
  if (g_session.isEmpty() || millis() > g_sessionExpiryMs) {
    return false;
  }
  if (!server.hasHeader("Cookie")) {
    return false;
  }
  String cookie = server.header("Cookie");
  int i = cookie.indexOf("SESSION=");
  if (i < 0) {
    return false;
  }
  String tok = cookie.substring(i + 8);
  int semi = tok.indexOf(';');
  if (semi >= 0) {
    tok = tok.substring(0, semi);
  }
  tok.trim();
  if (tok != g_session) {
    return false;
  }
  g_sessionExpiryMs = millis() + kSessionTtlMs;  // sliding expiry on activity
  return true;
}

// Every portal response carries no-store. Without it a browser is free to cache
// the settings page heuristically (it is a 200 text/html from a bare IP with no
// validators), and a cached page is not merely stale to look at: every field is
// rendered with value= and posted back on save, so submitting a stale form
// silently writes the old SSID and EAP credentials back into NVS even when the
// operator only touched the LED slider.
void noStore() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void redirect(const char *path) {
  noStore();
  server.sendHeader("Location", path);
  server.send(302, "text/plain", "");
}

// Returns true if the request may proceed. Rejects anything off-LAN (403) and, when
// requireAuth, bounces unauthenticated requests to the login page.
bool guard(bool requireAuth) {
  if (!fromLan()) {
    server.send(403, "text/plain", "Forbidden: admin portal is only reachable from the private network.");
    return false;
  }
  if (requireAuth && !isAuthed()) {
    redirect("/login");
    return false;
  }
  return true;
}

// ---- Pages --------------------------------------------------------------------

void sendLogin(const char *errMsg) {
  String p = pageHead("SpoolGate — Sign in");
  p += F("<h1>SpoolGate</h1><p class=sub>Admin sign-in</p>");
  if (errMsg) {
    p += F("<div class='msg err'>");
    p += errMsg;
    p += F("</div>");
  }
  p += F("<form method=post action=/login>"
         "<label>Username</label><input name=u autocomplete=username autofocus>"
         "<label>Password</label><input name=p type=password autocomplete=current-password>"
         "<button type=submit>Sign in</button></form>");
  p += kPageFoot;
  noStore();
  server.send(200, "text/html", p);
}

void handleLoginGet() {
  if (!guard(/*requireAuth=*/false)) {
    return;
  }
  if (isAuthed()) {
    redirect("/");
    return;
  }
  sendLogin(nullptr);
}

void handleLoginPost() {
  if (!guard(/*requireAuth=*/false)) {
    return;
  }
  const config::Settings &s = config::settings;
  if (server.arg("u") == s.adminUser && server.arg("p") == s.adminPass) {
    g_session = makeToken();
    g_sessionExpiryMs = millis() + kSessionTtlMs;
    server.sendHeader("Set-Cookie", "SESSION=" + g_session + "; Path=/; HttpOnly; SameSite=Strict");
    redirect("/");
  } else {
    sendLogin("Incorrect username or password.");
  }
}

void handleLogout() {
  if (!guard(/*requireAuth=*/false)) {
    return;
  }
  g_session = "";
  server.sendHeader("Set-Cookie", "SESSION=; Path=/; Max-Age=0");
  redirect("/login");
}

const char *staStatusName(wl_status_t st) {
  switch (st) {
    case WL_CONNECTED:     return "connected";
    case WL_NO_SSID_AVAIL: return "SSID not found in scan";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:  return "disconnected";
    case WL_IDLE_STATUS:   return "idle";
    case WL_SCAN_COMPLETED: return "scan completed";
    default:               return "unknown";
  }
}

void kv(String &p, const char *k, const String &v, const char *cls = nullptr) {
  p += F("<div class=kv><span>");
  p += k;
  p += F("</span><b");
  if (cls != nullptr) {
    p += F(" class=");
    p += cls;
  }
  p += F(">");
  p += htmlEscape(v);
  p += F("</b></div>");
}

// Renders a stored secret as a length rather than a value: enough to tell "set" from
// "silently blanked" without putting the credential on screen.
String secretSummary(const String &secret) {
  return secret.isEmpty() ? String(F("(not set)")) : String(secret.length()) + F(" chars stored");
}

String uptimeString() {
  uint32_t t = millis() / 1000;
  char buf[32];
  snprintf(
    buf, sizeof(buf), "%ud %02u:%02u:%02u", (unsigned)(t / 86400), (unsigned)((t % 86400) / 3600), (unsigned)((t % 3600) / 60),
    (unsigned)(t % 60)
  );
  return String(buf);
}

// What the firmware is using right now, as opposed to what the form below will set.
// Read straight from the running WiFi stack and config::settings on every request,
// so it cannot be served stale. The credential rows matter most: a blanked EAP
// identity is otherwise invisible until the uplink quietly stops associating, and
// that is exactly the failure a resubmitted stale form produces.
void appendStatus(String &p) {
  const config::Settings &s = config::settings;
  bool up = WiFi.status() == WL_CONNECTED;

  p += F("<h2>Current state</h2>");
  kv(p, "Uplink", up ? F("Up") : F("Down"), up ? "up" : "down");
  kv(p, "STA status", staStatusName(WiFi.status()));
  kv(p, "SSID in use", s.uplinkSsid);
  kv(p, "Security", s.uplinkEnterprise ? F("WPA2-Enterprise (802.1X)") : F("WPA2-Personal"));

  if (s.uplinkEnterprise) {
    kv(p, "EAP identity", s.eapIdentity.isEmpty() ? String(F("(empty)")) : s.eapIdentity, s.eapIdentity.isEmpty() ? "down" : nullptr);
    kv(p, "EAP username", s.eapUsername.isEmpty() ? String(F("(empty)")) : s.eapUsername, s.eapUsername.isEmpty() ? "down" : nullptr);
    kv(p, "EAP password", secretSummary(s.eapPassword), s.eapPassword.isEmpty() ? "down" : nullptr);
  } else {
    kv(p, "Password", secretSummary(s.uplinkPass), s.uplinkPass.isEmpty() ? "down" : nullptr);
  }

  if (up) {
    kv(p, "IP address", WiFi.localIP().toString());
    kv(p, "Gateway", WiFi.gatewayIP().toString());
    kv(p, "Upstream DNS", WiFi.dnsIP(0).toString());
    kv(p, "Signal", String(WiFi.RSSI()) + F(" dBm"));
  }

  kv(p, "SoftAP", s.apSsid + F(" on ") + s.apIp.toString());
  kv(p, "Clients joined", String(WiFi.softAPgetStationNum()));
  kv(p, "NAPT", napt::isEnabled() ? F("on") : F("off"), napt::isEnabled() ? "up" : "down");
  kv(p, "Uptime", uptimeString());
  kv(p, "Last reset", watchdog::lastResetReason());
}

void appendConfigForm(String &p, const char *msgHtml) {
  const config::Settings &s = config::settings;
  if (msgHtml) {
    p += msgHtml;
  }

  // Tab bar (type=button so it never submits the form).
  p += F("<div class=tabs>"
         "<button type=button class='tab active' data-t=net onclick=\"showTab('net')\">Network</button>"
         "<button type=button class=tab data-t=led onclick=\"showTab('led')\">LED</button>"
         "<button type=button class=tab data-t=adm onclick=\"showTab('adm')\">Admin</button>"
         "</div>");

  p += F("<form method=post action=/save>");

  // --- Network pane ---
  p += F("<div class=pane id=pane-net>");
  p += F("<h2>Upstream Wi-Fi (STA)</h2>");
  p += F("<label>SSID</label><input name=uSsid value='");
  p += htmlEscape(s.uplinkSsid);
  p += F("'>");
  p += F("<label>Security</label><select name=uAuth id=uAuth onchange=\"tog()\">");
  p += s.uplinkEnterprise ? F("<option value=personal>WPA2-Personal</option><option value=ent selected>WPA2-Enterprise (802.1X)</option>")
                          : F("<option value=personal selected>WPA2-Personal</option><option value=ent>WPA2-Enterprise (802.1X)</option>");
  p += F("</select>");
  p += F("<div id=personalBox><label>Password</label><input name=uPass type=password placeholder='(unchanged)'></div>");
  p += F("<div id=entBox><label>Identity</label><input name=eapId value='");
  p += htmlEscape(s.eapIdentity);
  p += F("'><label>Username</label><input name=eapUser value='");
  p += htmlEscape(s.eapUsername);
  p += F("'><label>Password</label><input name=eapPass type=password placeholder='(unchanged)'></div>");

  p += F("<h2>Downstream Wi-Fi (SoftAP)</h2>");
  p += F("<label>SSID</label><input name=apSsid value='");
  p += htmlEscape(s.apSsid);
  p += F("'><label>Password</label><input name=apPass type=password placeholder='(unchanged)'>"
         "<p class=hint>Minimum 8 characters. Leave blank to keep the current password.</p>");

  p += F("<h2>Network</h2>");
  p += F("<div class=row><div><label>AP IP address</label><input name=apIp value='");
  p += s.apIp.toString();
  p += F("'></div><div><label>DNS for clients</label><input name=dns value='");
  p += s.dns.toString();
  p += F("'></div></div><p class=hint>Clients get a /24 on the AP IP; DNS is handed out via DHCP.</p>");
  p += F("</div>");  // end net pane

  // --- LED pane ---
  p += F("<div class=pane id=pane-led>");
  p += F("<h2>Status LED</h2>");
  p += F("<label>Brightness</label>"
         "<input type=range name=ledBri min=0 max=255 value='");
  p += String(s.ledBrightness);
  p += F("' oninput=\"document.getElementById('briVal').textContent="
         "(this.value==0?'Off':Math.round(this.value/255*100)+'%')\">");
  p += F("<p class=hint>Level: <span id=briVal class=big></span> &nbsp; 0 turns the LED off entirely.</p>");
  p += F("<p class=hint>Colours: red = upstream down, amber = up/no client, blue = client connected.</p>");
  p += F("</div>");  // end led pane

  // --- Admin pane ---
  p += F("<div class=pane id=pane-adm>");
  p += F("<h2>Admin login</h2>");
  p += F("<label>Username</label><input name=admUser value='");
  p += htmlEscape(s.adminUser);
  p += F("'><label>New password</label><input name=admPass type=password placeholder='(unchanged)'>");
  p += F("</div>");  // end admin pane

  p += F("<button type=submit>Save &amp; reboot</button></form>");
  p += F("<a href=/log><button class=alt type=button>View log</button></a>");
  p += F("<a href=/speed><button class=alt type=button>Speed test</button></a>");
  p += F("<form method=post action=/logout><button class=alt type=submit>Sign out</button></form>");

  p += F("<script>"
         "function tog(){var e=document.getElementById('uAuth').value=='ent';"
         "document.getElementById('entBox').style.display=e?'block':'none';"
         "document.getElementById('personalBox').style.display=e?'none':'block';}"
         "function showTab(t){var ps=document.querySelectorAll('.pane');"
         "for(var i=0;i<ps.length;i++)ps[i].style.display='none';"
         "document.getElementById('pane-'+t).style.display='block';"
         "var bs=document.querySelectorAll('.tab');"
         "for(var j=0;j<bs.length;j++)bs[j].classList.toggle('active',bs[j].dataset.t==t);}"
         "tog();showTab('net');"
         "document.getElementById('briVal').textContent="
         "(document.getElementsByName('ledBri')[0].value==0?'Off':"
         "Math.round(document.getElementsByName('ledBri')[0].value/255*100)+'%');"
         "</script>");
}

void sendConfig(const char *msgHtml) {
  String p = pageHead("SpoolGate — Settings");
  p += F("<h1>SpoolGate</h1><p class=sub>Settings</p>");
  if (msgHtml != nullptr) {
    p += msgHtml;
  }
  appendStatus(p);
  appendConfigForm(p, nullptr);
  p += kPageFoot;
  noStore();
  server.send(200, "text/html", p);
}

void handleRoot() {
  if (!guard(/*requireAuth=*/true)) {
    return;
  }
  sendConfig(nullptr);
}

void handleSave() {
  if (!guard(/*requireAuth=*/true)) {
    return;
  }

  config::Settings n = config::settings;  // start from current, overlay changes

  String uSsid = server.arg("uSsid");
  uSsid.trim();
  String apSsid = server.arg("apSsid");
  apSsid.trim();
  String admUser = server.arg("admUser");
  admUser.trim();
  String apPass = server.arg("apPass");
  String admPass = server.arg("admPass");
  IPAddress apIp, dns;

  String err;
  if (uSsid.isEmpty()) {
    err = "Upstream SSID cannot be empty.";
  } else if (apSsid.isEmpty()) {
    err = "Downstream SSID cannot be empty.";
  } else if (admUser.isEmpty()) {
    err = "Admin username cannot be empty.";
  } else if (!apIp.fromString(server.arg("apIp"))) {
    err = "AP IP address is not a valid IPv4 address.";
  } else if (!dns.fromString(server.arg("dns"))) {
    err = "DNS address is not a valid IPv4 address.";
  } else if (apPass.length() > 0 && apPass.length() < 8) {
    err = "Downstream password must be at least 8 characters.";
  } else if (server.arg("uAuth") == "ent" && server.arg("eapUser").isEmpty() && config::settings.eapUsername.isEmpty()) {
    err = "802.1X username cannot be empty.";
  }

  if (!err.isEmpty()) {
    String m = F("<div class='msg err'>");
    m += err;
    m += F("</div>");
    sendConfig(m.c_str());
    return;
  }

  n.uplinkSsid = uSsid;

  // Every field below is applied only when the form actually carried a value for
  // it. Assigning unconditionally means a partial POST — a stale cached page, a
  // truncated submit, a hand-rolled request — silently blanks the EAP credentials
  // or drops the security mode back to WPA2-Personal, either of which strands the
  // router with no way upstream and no indication of why. Passwords already worked
  // this way; the identity and username did not, and they are just as fatal to
  // lose. The trade is that these fields can no longer be cleared from the portal,
  // only replaced, which is no loss: an empty 802.1X identity has no valid use.
  if (server.hasArg("uAuth")) {
    n.uplinkEnterprise = (server.arg("uAuth") == "ent");
  }
  if (server.arg("uPass").length() > 0) {
    n.uplinkPass = server.arg("uPass");
  }
  if (server.arg("eapId").length() > 0) {
    n.eapIdentity = server.arg("eapId");
  }
  if (server.arg("eapUser").length() > 0) {
    n.eapUsername = server.arg("eapUser");
  }
  if (server.arg("eapPass").length() > 0) {
    n.eapPassword = server.arg("eapPass");
  }
  n.apSsid = apSsid;
  if (apPass.length() >= 8) {
    n.apPass = apPass;
  }
  n.apIp = apIp;
  n.dns = dns;
  long bri = server.arg("ledBri").toInt();
  n.ledBrightness = static_cast<uint8_t>(bri < 0 ? 0 : (bri > 255 ? 255 : bri));
  n.adminUser = admUser;
  if (admPass.length() > 0) {
    n.adminPass = admPass;
  }

  config::settings = n;
  config::save();

  String p = pageHead("SpoolGate — Rebooting");
  p += F("<h1>Settings saved</h1>"
         "<div class='msg ok'>Rebooting to apply. If you changed the SoftAP name or "
         "password, reconnect to the new network, then browse to the AP IP address.</div>");
  p += kPageFoot;
  noStore();
  server.send(200, "text/html", p);

  g_rebootAtMs = millis() + 1500;  // let the response flush first
}

// The log as plain text, polled by the page below. Kept separate from the HTML so
// a refresh costs one small body rather than re-rendering the whole page.
void handleLogText() {
  if (!guard(/*requireAuth=*/true)) {
    return;
  }
  String body;
  body.reserve(logbuf::kLines * 64);
  logbuf::render(body);
  noStore();
  server.send(200, "text/plain", body);
}

void handleLog() {
  if (!guard(/*requireAuth=*/true)) {
    return;
  }
  String p = pageHead("SpoolGate — Log");
  p += F("<h1>SpoolGate</h1><p class=sub>Log</p>");
  p += F("<a href=/><button class=alt type=button>Back to settings</button></a>");
  p += F("<label><input type=checkbox id=follow checked style='width:auto;margin-right:6px'>"
         "Auto-refresh every 3s, pinned to the newest line</label>");
  p += F("<pre class=log id=log>loading...</pre>");
  p += F("<script>"
         "var el=document.getElementById('log'),fo=document.getElementById('follow');"
         "function load(){fetch('/log.txt',{cache:'no-store'}).then(function(r){"
         "if(!r.ok)throw 0;return r.text()}).then(function(t){"
         // Only touch the DOM when the text actually changed, so a reader scrolled
         // back through the buffer is not yanked to the bottom every three seconds.
         "if(t===el.textContent)return;el.textContent=t;"
         "if(fo.checked)el.scrollTop=el.scrollHeight;})"
         ".catch(function(){el.textContent+='\n[disconnected from router]';});}"
         "load();setInterval(function(){if(fo.checked)load();},3000);"
         "</script>");
  p += kPageFoot;
  noStore();
  server.send(200, "text/html", p);
}

// ---- Throughput test ----------------------------------------------------------

// RSSI of the associated client as this radio hears it: the printer's signal at
// *this* end of the link. No WiFi.softAP* call exposes it, and without it a stream
// that breaks up is indistinguishable between "the downstream radio link is
// marginal" and "NAT forwarding can't keep up" — which have nothing in common as
// fixes. Returns 0 when nothing is associated.
int8_t clientRssi() {
  wifi_sta_list_t list = {};
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num <= 0) {
    return 0;
  }
  int8_t best = -127;
  for (int i = 0; i < list.num; ++i) {
    if (list.sta[i].rssi > best) {
      best = list.sta[i].rssi;
    }
  }
  return best;
}

const char *healthJson(health::Result r) {
  switch (r) {
    case health::Result::Ok:     return "\"ok\"";
    case health::Result::Failed: return "\"fail\"";
    default:                     return "\"unknown\"";
  }
}

// Machine-readable snapshot of router-side state, for sampling while a throughput
// test runs on a laptop (tools/speedtest.ps1 polls it once a second). The heap
// figures are the reason it exists: the Wi-Fi driver's transmit buffers come out of
// the same heap, so a forwarding path that stalls under load shows up here as the
// free-heap low-water mark collapsing, which is invisible from the client end.
//
// Deliberately NOT behind the session login, unlike every other handler here. It
// carries no credentials, no SSIDs and nothing an attacker on the private /24 could
// not measure anyway, and gating it would mean every script that wants to correlate
// a throughput dip with router state has to carry the admin password. It is still
// refused off-LAN, which is the boundary that actually matters.
void handleStatJson() {
  if (!guard(/*requireAuth=*/false)) {
    return;
  }
  bool up = WiFi.status() == WL_CONNECTED;

  String j;
  j.reserve(400);
  j += F("{\"uptime_s\":");
  j += String(millis() / 1000);
  j += F(",\"uplink\":");
  j += up ? "true" : "false";
  j += F(",\"napt\":");
  j += napt::isEnabled() ? "true" : "false";
  j += F(",\"channel\":");
  j += String(WiFi.channel());
  j += F(",\"sta_rssi\":");
  j += String(up ? WiFi.RSSI() : 0);
  j += F(",\"clients\":");
  j += String(WiFi.softAPgetStationNum());
  j += F(",\"client_rssi\":");
  j += String(clientRssi());
  j += F(",\"health_up\":");
  j += healthJson(health::upstream());
  j += F(",\"health_down\":");
  j += healthJson(health::downstream());
  j += F(",\"heap_free\":");
  j += String(ESP.getFreeHeap());
  j += F(",\"heap_min\":");
  j += String(ESP.getMinFreeHeap());
  j += F(",\"heap_largest_block\":");
  j += String(ESP.getMaxAllocHeap());
  j += F(",\"aplink_bytes\":");
  j += String(g_speedBytes);
  j += F(",\"aplink_ms\":");
  j += String(g_speedMs);
  j += F("}");

  noStore();
  server.send(200, "application/json", j);
}

// Streams filler to the requesting client as fast as the SoftAP link will take it.
//
// This measures ONE hop — this radio to this client — and not the NAT path. That is
// the point of it: it separates "the downstream Wi-Fi link is slow" from
// "forwarding is slow", which look identical from the printer. A NAT'd flow can
// never beat about half of what this reports, because the single radio has to
// receive each packet on the STA link and re-send it on the AP link, on the same
// channel, out of the same air-time budget.
//
// Chunked rather than a declared Content-Length, so the deadline below can end a
// run early without the client seeing a truncated response. The client counts and
// times the bytes it actually received; the figure logged here is the router's own
// view of the same run.
void handleSpeedDown() {
  if (!guard(/*requireAuth=*/true)) {
    return;
  }

  long mb = server.arg("mb").toInt();
  if (mb <= 0) {
    mb = kSpeedDefaultMiB;
  } else if (mb > (long)kSpeedMaxMiB) {
    mb = kSpeedMaxMiB;
  }
  const uint32_t target = (uint32_t)mb * 1024UL * 1024UL;

  // Heap rather than a local. loopTask's stack is 8 KiB and a 4 KiB frame on it is
  // not worth the risk for a buffer that only lives for the length of the run.
  char *block = (char *)malloc(kSpeedBlockBytes);
  if (block == nullptr) {
    server.send(503, "text/plain", "Out of memory for the test buffer.");
    return;
  }
  memset(block, 'x', kSpeedBlockBytes);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  noStore();
  server.send(200, "application/octet-stream", "");

  uint32_t sent = 0;
  const uint32_t startMs = millis();
  while (sent < target && millis() - startMs < kSpeedMaxMs) {
    // Checked every block so a client that closes the tab mid-run ends the test
    // within one write rather than sitting out the whole deadline blocking loop().
    if (!server.client().connected()) {
      break;
    }
    server.sendContent(block, kSpeedBlockBytes);
    sent += kSpeedBlockBytes;
  }
  const uint32_t elapsedMs = millis() - startMs;
  server.sendContent("");  // zero-length chunk: terminates the chunked response
  free(block);

  g_speedBytes = sent;
  g_speedMs = elapsedMs;
  // bytes * 8 / milliseconds is bits per millisecond, which is kbit/s exactly.
  logbuf::printf(
    "[%10lu] speedtest: AP link sent %lu KiB in %lu ms = %lu kbit/s (client rssi %d dBm)\n", millis(), (unsigned long)(sent / 1024),
    (unsigned long)elapsedMs, (unsigned long)(elapsedMs > 0 ? (uint32_t)((uint64_t)sent * 8ULL / elapsedMs) : 0), clientRssi()
  );
}

void handleSpeedPage() {
  if (!guard(/*requireAuth=*/true)) {
    return;
  }
  String p = pageHead("SpoolGate — Speed test");
  p += F("<h1>SpoolGate</h1><p class=sub>Downstream link speed test</p>");
  p += F("<a href=/><button class=alt type=button>Back to settings</button></a>");

  p += F("<div class=tabs>"
         "<button type=button class=tab onclick=\"run(4)\">4 MiB</button>"
         "<button type=button class=tab onclick=\"run(16)\">16 MiB</button>"
         "<button type=button class=tab onclick=\"run(64)\">64 MiB</button>"
         "</div>");
  p += F("<p class=big id=res>idle</p>");
  p += F("<p class=hint>One hop only: this router's radio to the device you are "
         "reading this on. It is the ceiling for the downstream Wi-Fi link, not the "
         "speed of NAT-routed traffic &mdash; a forwarded packet crosses the same "
         "radio twice, so routed traffic gets at best about half of this. For the "
         "end-to-end figure run <code>tools/speedtest.ps1</code> from a laptop "
         "joined to this network.</p>");

  p += F("<h2>Router state</h2><div id=stat>loading&hellip;</div>");
  p += F("<p class=hint>Min free heap is a low-water mark since boot, so it only "
         "ever falls. If a run drives it down sharply, the Wi-Fi driver is running "
         "out of transmit buffers &mdash; that is a throughput ceiling no amount of "
         "signal will lift.</p>");

  p += F("<script>"
         "var busy=0;"
         "function row(k,v){return '<div class=kv><span>'+k+'</span><b>'+v+'</b></div>'}"
         "function stat(){"
         "fetch('/stat.json',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){"
         "var h='';"
         "h+=row('Uplink',d.uplink?'up':'down');"
         "h+=row('Channel',d.channel);"
         "h+=row('Uplink signal',d.sta_rssi+' dBm');"
         "h+=row('Clients joined',d.clients);"
         "h+=row('Client signal',d.client_rssi?d.client_rssi+' dBm':'n/a');"
         "h+=row('NAPT',d.napt?'on':'off');"
         "h+=row('Gateway reachable',d.health_up);"
         "h+=row('Client reachable',d.health_down);"
         "h+=row('Free heap',Math.round(d.heap_free/1024)+' KiB');"
         "h+=row('Min free heap since boot',Math.round(d.heap_min/1024)+' KiB');"
         "h+=row('Largest free block',Math.round(d.heap_largest_block/1024)+' KiB');"
         "h+=row('Last run, router side',d.aplink_ms?(d.aplink_bytes/125/d.aplink_ms).toFixed(2)+' Mbit/s':'none yet');"
         "document.getElementById('stat').innerHTML=h;"
         "}).catch(function(){document.getElementById('stat').textContent='unreachable'})}"
         // The clock starts on the first chunk, not on fetch(), so TCP setup and the
         // router's own malloc are excluded and what is left is transfer time.
         "function run(mb){"
         "if(busy){return}busy=1;"
         "var out=document.getElementById('res');"
         "out.textContent='running '+mb+' MiB\\u2026';"
         "var t0=0,got=0;"
         "fetch('/speed/down?mb='+mb+'&t='+Date.now(),{cache:'no-store'}).then(function(r){"
         "if(!r.ok){throw new Error('HTTP '+r.status)}"
         "var rd=r.body.getReader();"
         "function pump(){return rd.read().then(function(c){"
         "if(c.done){return}"
         "if(!t0){t0=performance.now()}"
         "got+=c.value.length;"
         "var s=(performance.now()-t0)/1000;"
         "if(s>0.3){out.textContent=(got/125000/s).toFixed(2)+' Mbit/s'}"
         "return pump()})}"
         "return pump()}).then(function(){"
         "var s=(performance.now()-t0)/1000;"
         "out.textContent=(got/125000/s).toFixed(2)+' Mbit/s \\u2014 '+(got/1048576).toFixed(1)"
         "+' MiB in '+s.toFixed(1)+' s';"
         "}).catch(function(e){out.textContent='failed: '+e.message})"
         ".then(function(){busy=0;stat()})}"
         "stat();"
         "</script>");
  p += kPageFoot;
  noStore();
  server.send(200, "text/html", p);
}

void handleNotFound() {
  if (!fromLan()) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  redirect(isAuthed() ? "/" : "/login");
}

}  // namespace

void begin() {
  static const char *headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLoginGet);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/log.txt", HTTP_GET, handleLogText);
  server.on("/speed", HTTP_GET, handleSpeedPage);
  server.on("/speed/down", HTTP_GET, handleSpeedDown);
  server.on("/stat.json", HTTP_GET, handleStatJson);
  server.onNotFound(handleNotFound);

  server.begin();
}

void loop() {
  server.handleClient();
  if (g_rebootAtMs != 0 && millis() > g_rebootAtMs) {
    ESP.restart();
  }
}

}  // namespace portal
