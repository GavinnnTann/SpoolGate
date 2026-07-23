#include "portal.h"

#include <Arduino.h>
#include <WebServer.h>
#include <esp_random.h>

#include "../config.h"

namespace portal {

namespace {

WebServer server(80);

// Single-session model: one operator at a time. A fresh login invalidates any prior
// session. Token is a 128-bit hex string from the hardware RNG.
String g_session;
uint32_t g_sessionExpiryMs = 0;
constexpr uint32_t kSessionTtlMs = 15UL * 60UL * 1000UL;  // 15 min inactivity

uint32_t g_rebootAtMs = 0;  // non-zero once a save has scheduled a reboot

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

void redirect(const char *path) {
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
  appendConfigForm(p, msgHtml);
  p += kPageFoot;
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
  }

  if (!err.isEmpty()) {
    String m = F("<div class='msg err'>");
    m += err;
    m += F("</div>");
    sendConfig(m.c_str());
    return;
  }

  n.uplinkSsid = uSsid;
  n.uplinkEnterprise = (server.arg("uAuth") == "ent");
  if (server.arg("uPass").length() > 0) {
    n.uplinkPass = server.arg("uPass");
  }
  n.eapIdentity = server.arg("eapId");
  n.eapUsername = server.arg("eapUser");
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
  server.send(200, "text/html", p);

  g_rebootAtMs = millis() + 1500;  // let the response flush first
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
