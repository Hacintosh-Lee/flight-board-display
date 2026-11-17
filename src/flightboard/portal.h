// 手机设置页: 不用电脑也能配 WiFi、换机场、换语言、填数据源密钥。
//
// 没配过 WiFi, 或配了但 45 秒连不上时, 板子自己开一个热点 "FlightBoard-XXXX"(没有密码),
// 屏幕上会提示。手机连上后一般会自动弹出设置页; 没弹就浏览器打开 192.168.4.1。
// 连上家里 WiFi 以后热点再开 2 分钟就关; 之后在同一个 WiFi 下用板子的 IP 也能打开这个页面。
//
// 本文件由 main.cpp 在定义完全局变量之后 include, 直接用那边的变量和函数。
#pragma once

static WebServer web(80);
static DNSServer dnsSrv;
static uint32_t apStopAt = 0, staLostMs = 0, staRetryMs = 0, applyWifiAt = 0;

static void apStart() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName);
  dnsSrv.start(53, "*", WiFi.softAPIP());  // 所有域名都指向自己, 手机才会弹出设置页
  // 热点开着时, 后台自动重连会不停换信道扫描, 手机那边会掉线; 改成没人连热点时才定时试一次
  WiFi.setAutoReconnect(false);
  apOn = true;
  apStopAt = 0;
  dirty = true;
}

static void apStop() {
  if (!apOn) return;
  dnsSrv.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  apOn = false;
  dirty = true;
}

// 泰文 TIS-620 -> UTF-8(网页要 UTF-8)
static String tisToUtf8(const char *s) {
  String o;
  for (; s && *s; s++) {
    uint8_t b = (uint8_t)*s;
    if (b < 0x80) {
      o += (char)b;
    } else if (b >= 0xA1 && b <= 0xFB) {
      uint16_t u = 0x0E00 + (b - 0xA0);
      o += (char)(0xE0 | (u >> 12));
      o += (char)(0x80 | ((u >> 6) & 0x3F));
      o += (char)(0x80 | (u & 0x3F));
    }
  }
  return o;
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Flight Board</title><style>
:root{--bg:#f7f5f2;--fg:#1d1b19;--mute:#8a847c;--line:#e4dfd8;--card:#fff;--acc:#e0892a}
@media(prefers-color-scheme:dark){:root{--bg:#161514;--fg:#efebe6;--mute:#8d877f;--line:#2e2b28;--card:#1f1e1c}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px -apple-system,system-ui,sans-serif}
main{max-width:420px;margin:0 auto;padding:32px 16px 48px}
h1{font-size:28px;margin:0 0 6px;letter-spacing:-.02em}
.st{color:var(--mute);font-size:14px;display:flex;align-items:center;gap:8px;margin-bottom:12px}
.dot{width:8px;height:8px;border-radius:50%;background:#c9c2b8}.ok .dot{background:#3aa55d}
label{display:block;font-size:13px;color:var(--mute);margin:20px 0 6px}
input,select{width:100%;font:inherit;padding:12px;border:1px solid var(--line);border-radius:10px;background:var(--card);color:var(--fg);-webkit-appearance:none}
.seg{display:flex;border:1px solid var(--line);border-radius:10px;overflow:hidden;background:var(--card)}
.seg button{flex:1;border:0;background:none;padding:12px;font:inherit;color:var(--fg)}
.seg button.on{background:var(--fg);color:var(--bg)}
.nm{font-size:13px;color:var(--mute);margin-top:6px;min-height:1.2em}
details{margin-top:20px}summary{color:var(--mute);font-size:13px;cursor:pointer}
.go{margin-top:28px;width:100%;padding:14px;border:0;border-radius:12px;background:var(--acc);color:#fff;font:600 16px -apple-system,system-ui,sans-serif}
.go:disabled{opacity:.5}.msg{text-align:center;color:var(--mute);font-size:14px;margin-top:14px;min-height:1.2em}
</style></head><body><main>
<h1 id=t></h1><div class=st id=st><span class=dot></span><span id=sx></span></div>
<label id=lw></label><select id=ssid></select>
<label id=lp></label><input id=pass type=password autocomplete=off>
<label id=la></label><input id=apt maxlength=3 autocapitalize=characters autocomplete=off><div class=nm id=nm></div>
<label id=ll></label><div class=seg id=lang><button data-v=0>中文</button><button data-v=1>EN</button><button data-v=2>ไทย</button></div>
<details id=kd><summary id=lk></summary><label>AirLabs API key</label><input id=key type=password autocomplete=off></details>
<button class=go id=go></button><div class=msg id=msg></div>
</main><script>
const TX=[
{t:'航班牌',w:'WiFi',p:'密码',a:'机场三字码',l:'屏幕语言',k:'数据源密钥',go:'保存',ok:'已连接 ',no:'未连接 WiFi',sc:'正在扫描…',bad:'找不到这个机场',sv:'已保存，正在连接…',done:'已连接，屏幕马上会更新',fail:'没连上，检查一下密码',kh:'已填写，留空不改',kn:'到 airlabs.co 免费申请'},
{t:'Flight Board',w:'WiFi',p:'Password',a:'Airport code',l:'Screen language',k:'Data source key',go:'Save',ok:'Connected ',no:'WiFi not connected',sc:'Scanning…',bad:'Unknown airport',sv:'Saved, connecting…',done:'Connected, the screen will update shortly',fail:'Could not connect, check the password',kh:'Set, leave blank to keep',kn:'Free at airlabs.co'},
{t:'ป้ายเที่ยวบิน',w:'WiFi',p:'รหัสผ่าน',a:'รหัสสนามบิน',l:'ภาษาหน้าจอ',k:'คีย์แหล่งข้อมูล',go:'บันทึก',ok:'เชื่อมต่อแล้ว ',no:'ยังไม่ได้เชื่อมต่อ WiFi',sc:'กำลังสแกน…',bad:'ไม่พบสนามบินนี้',sv:'บันทึกแล้ว กำลังเชื่อมต่อ…',done:'เชื่อมต่อแล้ว หน้าจอจะอัปเดตเร็วๆ นี้',fail:'เชื่อมต่อไม่ได้ ตรวจสอบรหัสผ่าน',kh:'ตั้งไว้แล้ว เว้นว่างเพื่อใช้ค่าเดิม',kn:'สมัครฟรีที่ airlabs.co'}];
const $=i=>document.getElementById(i);let S={},L=0,X=TX[0];
async function j(u,o){return (await fetch(u,o)).json()}
function paint(){X=TX[L];$('t').textContent=X.t;$('lw').textContent=X.w;$('lp').textContent=X.p;$('la').textContent=X.a;
$('ll').textContent=X.l;$('lk').textContent=X.k;$('go').textContent=X.go;$('key').placeholder=S.key?X.kh:X.kn;
document.querySelectorAll('#lang button').forEach(b=>b.classList.toggle('on',+b.dataset.v==L));status();name()}
function status(){$('st').className='st'+(S.conn?' ok':'');$('sx').textContent=S.conn?X.ok+S.ssid+'  '+S.ip:X.no}
async function name(){const c=$('apt').value.toUpperCase();if(c.length<3){$('nm').textContent='';return}
const r=await j('/api/name?c='+c+'&l='+L);$('nm').textContent=r.name||X.bad}
async function scan(){const s=$('ssid');s.innerHTML='<option>'+X.sc+'</option>';const r=await j('/api/scan');s.innerHTML='';
const list=r.nets.slice();if(S.ssid&&!list.includes(S.ssid))list.unshift(S.ssid);
list.forEach(n=>{const o=document.createElement('option');o.textContent=n;o.selected=n==S.ssid;s.appendChild(o)})}
document.querySelectorAll('#lang button').forEach(b=>b.onclick=()=>{L=+b.dataset.v;paint()});
$('apt').oninput=()=>{$('apt').value=$('apt').value.toUpperCase();name()};
$('go').onclick=async()=>{$('go').disabled=true;$('msg').textContent=X.sv;
const f=new URLSearchParams({ssid:$('ssid').value,pass:$('pass').value,apt:$('apt').value.toUpperCase(),lang:L,key:$('key').value});
const chg=$('ssid').value!=S.ssid||$('pass').value;
try{await fetch('/api/save',{method:'POST',headers:{'X-Flightboard':'1'},body:f})}catch(e){}
$('pass').value='';$('key').value='';
for(let i=0;i<20;i++){await new Promise(r=>setTimeout(r,1500));try{S=await j('/api/state')}catch(e){continue}
status();if(!chg||S.conn&&S.ssid==$('ssid').value){$('msg').textContent=X.done;break}if(i==19)$('msg').textContent=X.fail}
$('go').disabled=false};
(async()=>{S=await j('/api/state');L=S.lang;$('apt').value=S.apt;$('kd').open=!S.key;paint();scan()})();
</script></body></html>)HTML";

static void sendJson(JsonDocument &d) {
  String out;
  serializeJson(d, out);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", out);
}

static void webState() {
  JsonDocument d;
  bool conn = WiFi.status() == WL_CONNECTED;
  d["conn"] = conn;
  d["ssid"] = wifiSsid;
  d["ip"] = conn ? WiFi.localIP().toString() : String();
  d["apt"] = airport;
  d["lang"] = nameLang;
  d["key"] = alKey.length() > 0;  // 只说有没有, 不把密钥本身发出去
  d["dep"] = boards[0].n;
  d["arr"] = boards[1].n;
  sendJson(d);
}

static void webScan() {
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray a = d["nets"].to<JsonArray>();
  // 按信号强弱排好(scanNetworks 已经排过), 去掉重名和隐藏的
  for (int i = 0; i < n && a.size() < 20; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (JsonVariant v : a) dup = dup || s == v.as<const char *>();
    if (!dup) a.add(s);
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void webName() {
  String c = web.arg("c");
  c.toUpperCase();
  int l = web.arg("l").toInt();
  JsonDocument d;
  const char *p = c.length() == 3 ? fbPlace(c.c_str(), l == 2 ? 2 : 1) : nullptr;
  if (p) d["name"] = l == 2 ? tisToUtf8(p) : String(p);
  sendJson(d);
}

static String pendSsid, pendPass;

static void webSave() {
  // 要求自定义请求头: 别的网页没法跨站偷偷提交(浏览器会先预检, 这里不答应)
  if (web.header("X-Flightboard") != "1") {
    web.send(403, "text/plain", "forbidden");
    return;
  }
  String lang = web.arg("lang");
  if (lang.length()) {
    uint8_t v = (uint8_t)constrain(lang.toInt(), 0, 2);
    if (v != nameLang) {
      nameLang = v;
      prefs.putUChar("lang", v);
      needCls = true;
      dirty = true;
    }
  }
  String apt = web.arg("apt");
  apt.trim();
  apt.toUpperCase();
  if (apt.length() == 3 && apt != airport) {
    setAirport(apt.c_str());
    Serial.printf("$AP,%s\n", airport);  // 插着电脑的话, 让电脑跟着换, 由它马上取新机场的数据
  }
  String key = web.arg("key");
  key.trim();
  if (key.length() && key != alKey) {
    alKey = key;
    prefs.putString("alkey", alKey);
    nextFetchMs = millis();
    fetchState = waitState = -1;
    dirty = true;
  }
  String ssid = web.arg("ssid"), pass = web.arg("pass");
  if (ssid.length() && (ssid != wifiSsid || pass.length() || WiFi.status() != WL_CONNECTED)) {
    pendSsid = ssid;
    pendPass = pass.length() || ssid != wifiSsid ? pass : wifiPass;  // 同一个 WiFi 不填密码 = 沿用
    applyWifiAt = millis() + 400;  // 先把回应发出去再换 WiFi
  }
  web.send(200, "application/json", "{\"ok\":true}");
}

static void webOther() {
  if (apOn) {
    // 手机连上热点后会探测外网(captive.apple.com 之类), 一律引到设置页, 手机就会自动弹出来
    web.sendHeader("Location", "http://192.168.4.1/");
    web.send(302, "text/plain", "");
  } else {
    web.send(404, "text/plain", "not found");
  }
}

static void portalBegin() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(apName, sizeof(apName), "FlightBoard-%02X%02X", (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 40) & 0xFF));
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/api/state", HTTP_GET, webState);
  web.on("/api/scan", HTTP_GET, webScan);
  web.on("/api/name", HTTP_GET, webName);
  web.on("/api/save", HTTP_POST, webSave);
  web.onNotFound(webOther);
  const char *hdrs[] = {"X-Flightboard"};
  web.collectHeaders(hdrs, 1);
  if (!wifiSsid.length()) apStart();
  web.begin();
}

static void portalTask() {
  web.handleClient();
  if (apOn) dnsSrv.processNextRequest();

  if (applyWifiAt && (int32_t)(millis() - applyWifiAt) >= 0) {
    applyWifiAt = 0;
    wifiSsid = pendSsid;
    wifiPass = pendPass;
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    WiFi.disconnect();
    wifiStart();
    staLostMs = millis() ? millis() : 1;
    staRetryMs = millis();
    dirty = true;
  }

  if (WiFi.status() == WL_CONNECTED) {
    staLostMs = 0;
    if (apOn && !apStopAt) apStopAt = millis() + 120000UL;  // 给手机留时间看到"已连接"
    if (apOn && (int32_t)(millis() - apStopAt) >= 0 && WiFi.softAPgetStationNum() == 0) apStop();
    return;
  }
  apStopAt = 0;
  // 注意不能写 millis() | 1: 偶数时会比 millis() 大 1, 一减就下溢成很大的数, 开机立刻开热点
  if (!staLostMs) staLostMs = millis() ? millis() : 1;
  if (!apOn && (!wifiSsid.length() || (int32_t)(millis() - staLostMs) > 45000)) apStart();
  // 热点开着、又没人连的时候, 每 2 分钟再试一次家里的 WiFi(路由器可能只是比板子晚开机)
  if (apOn && wifiSsid.length() && WiFi.softAPgetStationNum() == 0 && millis() - staRetryMs > 120000UL) {
    staRetryMs = millis();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}
