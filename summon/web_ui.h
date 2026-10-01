#pragma once

// Single-page status + settings UI served from the ESP32.
const char WEB_UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Breadboard AI Summon</title>
<style>
:root{color-scheme:dark}
body{margin:0 auto;max-width:640px;padding:16px;font:15px/1.6 system-ui,sans-serif;background:#111;color:#eee}
h1{font-size:20px;margin:4px 0 12px}h2{font-size:15px;margin:0 0 8px;color:#aaa}
section{background:#1c1c1c;border:1px solid #333;border-radius:10px;padding:14px;margin:12px 0}
.big{font-size:26px;font-weight:700;margin-bottom:8px}
dl{display:grid;grid-template-columns:7em 1fr;gap:4px 10px;margin:8px 0 0}dt{color:#999}dd{margin:0;overflow-wrap:anywhere}
pre{white-space:pre-wrap;word-break:break-word;max-height:320px;overflow:auto;margin:0;font-size:13px}
label{display:block;color:#aaa;font-size:13px}
input,button{width:100%;box-sizing:border-box;padding:9px;margin:4px 0 10px;border-radius:8px;border:1px solid #444;background:#0c0c0c;color:#eee;font:inherit}
button{background:#2b3f66;border-color:#3d5a8f;cursor:pointer}
.led{display:inline-block;width:14px;height:14px;border-radius:50%;background:#333;vertical-align:middle}
.led.on{background:#ffcc33;box-shadow:0 0 10px #ffcc33}
.meter{height:8px;background:#000;border-radius:4px;overflow:hidden;margin-top:4px}.meter>div{height:100%;width:0;background:#6ea8ff}
</style></head><body>
<h1>Breadboard AI Summon</h1>
<section>
<div class="big" id="state">-</div>
<button onclick="fetch('/api/summon',{method:'POST'})">開始 / 終了</button>
<dl>
<dt>LED</dt><dd><span id="led" class="led"></span></dd>
<dt>マイク</dt><dd><span id="mic">-</span><div class="meter"><div id="micBar"></div></div></dd>
<dt>スピーカー</dt><dd id="spk">-</dd>
<dt>セッション</dt><dd id="session">-</dd>
<dt>Wi-Fi</dt><dd id="wifi">-</dd>
<dt>メモリ</dt><dd id="mem">-</dd>
</dl>
</section>
<section><h2>ログ</h2><pre id="log"></pre></section>
<section><h2>設定</h2>
<form id="settings">
<label>Wi-Fi SSID<input name="ssid" id="ssid" autocomplete="off"></label>
<label>Wi-Fi パスワード<input name="password" type="password" placeholder="空欄なら変更しない"></label>
<label>OpenAI API key<input name="api_key" id="key" type="password" autocomplete="new-password"></label>
<button>保存して再起動</button>
<div id="msg"></div>
</form>
</section>
<script>
const $=id=>document.getElementById(id);
const LABEL={IDLE:'待機中',CONNECTING:'接続しています…',LIVE:'会話中（話しかけてください）',CLOSING:'終了しています…',SETUP:'セットアップモード'};
let first=true;
async function tick(){
  try{
    const s=await (await fetch('/api/status')).json();
    $('state').textContent=LABEL[s.state]||s.state;
    $('led').className='led'+(s.led?' on':'');
    $('mic').textContent=`L ${s.mic_l} / R ${s.mic_r} dBFS ・ 送信 ${s.mic_sent} dBFS${s.mic_muted?'（再生中ミュート）':''} ・ ${s.mic_chunks} 回送信`;
    $('micBar').style.width=Math.max(0,Math.min(100,(s.mic_sent+70)/60*100))+'%';
    $('spk').textContent=`出力 ${s.spk} dBFS ・ バッファ ${s.play_ms} ms`;
    $('session').textContent=s.session_s?`${s.session_s} 秒経過 / 課金 ${s.billed_s} 秒`:'-';
    $('wifi').textContent=s.state==='SETUP'?`AP ${s.ssid} / ${s.ip}`:`${s.ssid} / ${s.ip} / ${s.rssi} dBm`;
    $('mem').textContent=`heap ${s.heap} / psram ${s.psram} / 最大受信 ${s.largest_msg} B`;
    const log=$('log');const atBottom=log.scrollTop+log.clientHeight>=log.scrollHeight-4;
    log.textContent=s.log;if(atBottom)log.scrollTop=log.scrollHeight;
    if(first){first=false;$('ssid').value=s.wifi_ssid;$('key').placeholder=s.api_key_set?'設定済み（空欄なら変更しない）':'未設定';}
  }catch(e){$('state').textContent='接続できません';}
}
$('settings').onsubmit=async e=>{
  e.preventDefault();
  const r=await fetch('/api/settings',{method:'POST',body:new URLSearchParams(new FormData(e.target))});
  $('msg').textContent=await r.text();
};
tick();setInterval(tick,1000);
</script>
</body></html>
)HTML";
