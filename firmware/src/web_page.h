/*
 * 手机控制页面（板子直接发给手机，不需要联网、不需要装 App）
 * 手机连上热点「灯带遥控」后，浏览器打开 http://192.168.4.1
 */
#ifndef WEB_PAGE_H
#define WEB_PAGE_H

const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>灯带遥控</title>
<style>
:root{--bg:#0b0f14;--card:#141a22;--line:#242c37;--line2:#2f3945;--txt:#e8eef6;--dim:#8593a5;--acc:#4f9cf9;--ok:#3fb950}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--txt);font:16px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Helvetica Neue",Arial,sans-serif}
.wrap{max-width:640px;margin:0 auto;padding:16px 14px 36px}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
h1{font-size:18px;margin:0;font-weight:600}
h1 small{color:var(--dim);font-size:12px;font-weight:400;margin-left:6px}
.stat{font-size:13px;color:var(--dim);display:flex;align-items:center}
.dot{width:9px;height:9px;border-radius:50%;background:#4a5563;margin-right:6px;transition:.3s}
.dot.on{background:var(--ok);box-shadow:0 0 8px rgba(63,185,80,.85)}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:12px}
h2{font-size:12px;color:var(--dim);font-weight:600;margin:0 0 11px;letter-spacing:.08em}
.row{display:flex;align-items:center;gap:9px;flex-wrap:wrap}
.row+.row{margin-top:9px}
input[type=number],input[type=time]{width:88px;padding:10px;font:600 16px/1.2 inherit;text-align:center;color:var(--txt);background:#0d1319;border:1px solid var(--line2);border-radius:9px}
input[type=time]{width:106px}
select{padding:10px;font:inherit;color:var(--txt);background:#0d1319;border:1px solid var(--line2);border-radius:9px}
input:focus,select:focus{outline:none;border-color:var(--acc)}
input[type=color]{width:58px;height:44px;padding:2px;border:1px solid var(--line2);border-radius:9px;background:#0d1319}
input[type=range]{-webkit-appearance:none;appearance:none;flex:1;min-width:140px;height:8px;background:linear-gradient(90deg,#1f6feb,#58a6ff,#a5d6ff);border-radius:99px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:28px;height:28px;border-radius:50%;background:#fff;border:3px solid var(--acc)}
button{font:inherit;color:var(--txt);background:#1b232d;border:1px solid var(--line2);border-radius:10px;padding:11px 14px;cursor:pointer;transition:.12s}
button:active{transform:scale(.97)}
button.pri{background:var(--acc);border-color:var(--acc);color:#04101f;font-weight:600}
button.mini{padding:8px 12px;font-size:14px;color:var(--dim)}
.eff{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.eff button{padding:14px 8px;font-size:15px}
.eff button.cur{border-color:var(--acc);background:#12263f;color:#9dcaff;font-weight:600}
.sw{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.sw button{width:40px;height:40px;padding:0;border-radius:50%;border:2px solid #2f3945}
.val{font-weight:700;font-variant-numeric:tabular-nums;min-width:40px;text-align:right}
.unit{color:var(--dim);font-size:14px}
.hint{font-size:12.5px;color:var(--dim);margin-top:9px;line-height:1.65}
.sub{font-size:13px;color:#9dcaff;margin:13px 0 7px;font-weight:600}
.sub:first-child{margin-top:0}
#log{background:#080c11;border:1px solid var(--line);border-radius:10px;padding:10px;font:12px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;color:#9fb3c8;white-space:pre-wrap;min-height:56px}
.foot{color:var(--dim);font-size:12.5px;text-align:center;line-height:1.9}
.chk{display:flex;align-items:center;gap:7px;font-size:15px}
.chk input{width:20px;height:20px}
</style>
</head>
<body>
<div class="wrap">

  <header>
    <h1>灯带遥控<small>WS2812B · ESP32-S3</small></h1>
    <div class="stat"><span class="dot" id="dot"></span><span id="stTxt">连接中…</span></div>
  </header>

  <div class="card">
    <h2>亮度</h2>
    <div class="row">
      <input type="range" id="bri" min="0" max="255" value="121">
      <span class="val" id="briVal">121</span>
    </div>
  </div>

  <div class="card">
    <h2>单色（整条一个颜色）</h2>
    <div class="row">
      <input type="color" id="col" value="#ff8000">
      <span class="unit">选颜色，或点下面的色块</span>
    </div>
    <div class="sw" id="sw">
      <button data-c="FF0000" style="background:#ff0000" title="红"></button>
      <button data-c="FF7A00" style="background:#ff7a00" title="橙"></button>
      <button data-c="FFE600" style="background:#ffe600" title="黄"></button>
      <button data-c="00E676" style="background:#00e676" title="绿"></button>
      <button data-c="00E5FF" style="background:#00e5ff" title="青"></button>
      <button data-c="0A84FF" style="background:#0a84ff" title="蓝"></button>
      <button data-c="8E7BFF" style="background:#8e7bff" title="紫"></button>
      <button data-c="FF64B4" style="background:#ff64b4" title="粉"></button>
      <button data-c="FFFFFF" style="background:#ffffff" title="白"></button>
    </div>
  </div>

  <div class="card">
    <h2>灯效</h2>
    <div class="eff" id="effs">
      <button data-fx="0">🌈 彩虹流动</button>
      <button data-fx="1">➡️ 逐颗扫描</button>
      <button data-fx="2">🫧 整条呼吸</button>
      <button data-fx="3">☄️ 彗星来回</button>
      <button data-fx="5">🎨 整体变色</button>
      <button data-fx="4">⚪️ 全白长亮</button>
      <button data-fx="7">🎯 纯色常亮</button>
      <button data-fx="8">💡 纯色呼吸</button>
      <button data-fx="9">🌬️ 彩虹呼吸</button>
      <button data-fx="10">🐎 跑马灯</button>
      <button data-fx="11">🎭 双色追逐</button>
      <button data-fx="12">🌠 彩虹流星</button>
      <button data-fx="13">✨ 星星闪烁</button>
      <button data-fx="14">🔥 篝火</button>
      <button data-fx="15">🚨 警灯</button>
      <button data-fx="16">🌀 彩虹波浪</button>
      <button data-fx="6">🌑 关灯</button>
    </div>
  </div>

  <div class="card">
    <h2>定时</h2>
    <div class="sub">倒计时关灯</div>
    <div class="row">
      <input type="number" id="offMin" value="30" min="1" max="720">
      <span class="unit">分钟后关灯</span>
      <button class="mini" id="offStart">开始</button>
      <button class="mini" id="offCancel">取消</button>
    </div>
    <div class="hint" id="offInfo"></div>

    <div class="sub">每天定时开关</div>
    <div class="row">
      <label class="chk"><input type="checkbox" id="dOn">启用</label>
      <input type="time" id="tOn" value="19:00">
      <span class="unit">开</span>
      <input type="time" id="tOff" value="23:00">
      <span class="unit">关</span>
      <button class="mini" id="dSave">保存</button>
    </div>
    <div class="hint" id="clockInfo">打开本页会自动对时。</div>
  </div>

  <div class="card">
    <h2>灯珠数量</h2>
    <div class="row">
      <input type="number" id="nLeds" value="11" min="1" max="300">
      <span class="unit">颗</span>
      <button class="pri" id="applyN">应用</button>
    </div>
    <div class="row">
      <input type="number" id="lenVal" value="18" min="0.1" step="0.1">
      <select id="lenUnit"><option value="cm">厘米</option><option value="m">米</option></select>
      <span class="unit">→</span><span class="val" id="lenCalc">11 颗</span>
      <button class="mini" id="lenUse">用这个</button>
    </div>
    <div class="hint">你这卷是每米 200 颗（1 厘米 2 颗）。算完按「用这个」，再看灯带尾巴：还有几颗不亮就加 1~2 颗，走到头还空跑就减 1~2 颗。</div>
  </div>

  <div class="card">
    <h2>板子现在</h2>
    <div id="log">正在读…</div>
  </div>

  <p class="foot">
    手机连 Wi-Fi：<b id="apName">灯带遥控</b>　密码 <b>12345678</b><br>
    网页地址 <b>http://192.168.4.1</b>
  </p>
</div>

<script>
var $ = function(id){ return document.getElementById(id); };
var editing = false;

function api(p){
  return fetch(p, {cache:"no-store"}).then(function(r){ return r.json(); })
          .catch(function(){ return {ok:false}; });
}

function pad2(v){ return (v<10?"0":"")+v; }
function hm(min){ min = (min||0); return pad2(Math.floor(min/60)) + ":" + pad2(min%60); }
function toMin(s){
  var p = (s||"00:00").split(":");
  return (parseInt(p[0],10)||0)*60 + (parseInt(p[1],10)||0);
}

/* ---------- 对时（每天定时要用到）---------- */
function syncTime(){
  var d = new Date();
  api("/api/time?epoch=" + Math.floor(d.getTime()/1000) +
      "&tz=" + (-d.getTimezoneOffset()));
}
syncTime();
setInterval(syncTime, 60000);

/* ---------- 读板子状态 ---------- */
function markFx(id){
  document.querySelectorAll("#effs button").forEach(function(b){
    if (parseInt(b.dataset.fx,10) === id) b.classList.add("cur");
    else                                   b.classList.remove("cur");
  });
}

function refresh(){
  api("/api/state").then(function(s){
    if (!s.ok){
      $("dot").classList.remove("on");
      $("stTxt").textContent = "连不上板子";
      return;
    }
    $("dot").classList.add("on");
    $("stTxt").textContent = "已连上";

    if (document.activeElement !== $("bri")) $("bri").value = s.bri;
    $("briVal").textContent = s.bri;
    if (document.activeElement !== $("nLeds")) $("nLeds").value = s.n;
    if (s.color && document.activeElement !== $("col")) $("col").value = "#" + s.color;
    markFx(s.fxid);

    $("dOn").checked = !!s.daily;
    if (document.activeElement !== $("tOn"))  $("tOn").value  = hm(s.onMin);
    if (document.activeElement !== $("tOff")) $("tOff").value = hm(s.offMin);

    $("offInfo").textContent = s.offLeft > 0
      ? ("⏳ 还有 " + s.offLeft + " 分钟自动关灯（取消就点上面的「取消」）")
      : "";
    $("clockInfo").textContent = s.synced
      ? "已经对过时了，每天定时会照常生效。"
      : "还没对时 —— 打开本页就自动对时（板子断电后要对一次）。";

    $("log").textContent = "现在： " + s.fx + "　亮度 " + s.bri + "　" + s.n + " 颗" +
      (s.offLeft > 0 ? "　（" + s.offLeft + " 分钟后关灯）" : "");
    if (s.ap) $("apName").textContent = s.ap;
  }).then(function(){ setTimeout(refresh, 2000); });
}

/* ---------- 操作 ---------- */
var briTimer = null;
$("bri").oninput = function(){
  $("briVal").textContent = this.value;
  clearTimeout(briTimer);
  briTimer = setTimeout(function(){ api("/api/set?bri=" + $("bri").value); }, 250);
};
$("bri").onchange = function(){ api("/api/set?bri=" + this.value); };

function useColor(hex){
  hex = (hex||"").replace("#","").toUpperCase();
  if (hex.length !== 6) return;
  $("col").value = "#" + hex;
  api("/api/set?color=" + hex);
}
$("col").onchange = function(){ useColor(this.value); };
document.querySelectorAll("#sw button").forEach(function(b){
  b.onclick = function(){ useColor(b.dataset.c); };
});

document.querySelectorAll("#effs button").forEach(function(b){
  b.onclick = function(){
    markFx(parseInt(b.dataset.fx,10));
    api("/api/set?fx=" + b.dataset.fx);
  };
});

$("applyN").onclick = function(){
  var n = parseInt($("nLeds").value, 10);
  if (!(n >= 1 && n <= 300)) { alert("灯珠数量要在 1~300 之间"); return; }
  api("/api/set?n=" + n);
};

function lenCount(){
  var v = parseFloat($("lenVal").value);
  if (!(v > 0)) return 1;
  var cm = ($("lenUnit").value === "m") ? v * 100 : v;
  return Math.min(300, Math.max(1, Math.round(cm * 2)));
}
function lenShow(){ $("lenCalc").textContent = lenCount() + " 颗"; }
$("lenVal").oninput = lenShow;
$("lenUnit").onchange = lenShow;
$("lenUse").onclick = function(){
  var n = lenCount();
  $("nLeds").value = n;
  api("/api/set?n=" + n);
};

$("offStart").onclick = function(){
  var m = parseInt($("offMin").value, 10);
  if (!(m >= 1 && m <= 720)) { alert("分钟数要在 1~720 之间"); return; }
  api("/api/timer?off=" + m).then(refresh);
};
$("offCancel").onclick = function(){ api("/api/timer?off=0").then(refresh); };

function saveDaily(){
  api("/api/daily?en=" + ($("dOn").checked ? 1 : 0) +
      "&on=" + toMin($("tOn").value) + "&off=" + toMin($("tOff").value)).then(refresh);
}
$("dSave").onclick = saveDaily;
$("dOn").onchange = saveDaily;

lenShow();
refresh();
</script>
</body></html>
)HTMLPAGE";

#endif
