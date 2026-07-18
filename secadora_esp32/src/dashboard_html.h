#pragma once

#include <Arduino.h>

// Recuperado byte-a-byte do firmware original (app0_ota_0.bin), extraido
// diretamente da regiao entre "<!DOCTYPE html>" e "</html>" no binario.
// NAO alterar sem necessidade - mantem o dashboard identico ao original.
const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SECADORA</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Rajdhani:wght@400;600;700&display=swap');
  :root{
    --bg:#0a0c0f;--panel:#111418;--border:#1e2530;
    --red:#ff3b3b;--green:#00e676;--cyan:#00e5ff;
    --yellow:#ffd600;--orange:#ff6d00;--gray:#546e7a;
    --text:#cfd8dc;--dim:#37474f;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font-family:'Rajdhani',sans-serif;
       min-height:100vh;padding:12px}
  h1{font-family:'Share Tech Mono',monospace;font-size:1.1rem;letter-spacing:.2em;
     color:var(--cyan);text-align:center;padding:10px 0 4px;text-transform:uppercase}
  .ip{font-family:'Share Tech Mono',monospace;font-size:.65rem;color:var(--dim);
      text-align:center;margin-bottom:14px}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
  .card{background:var(--panel);border:1px solid var(--border);border-radius:6px;
        padding:12px;position:relative}
  .card-label{font-size:.65rem;letter-spacing:.15em;color:var(--gray);
              text-transform:uppercase;margin-bottom:6px}
  .card-val{font-family:'Share Tech Mono',monospace;font-size:1.6rem;color:var(--text)}
  .card-val span{font-size:.8rem;color:var(--gray)}
  .card-sub{font-size:.75rem;color:var(--cyan);margin-top:4px}
  .badge{display:inline-block;padding:2px 8px;border-radius:3px;
         font-size:.7rem;letter-spacing:.1em;font-weight:700}
  .on {background:#ff3b3b22;color:var(--red);border:1px solid var(--red)}
  .off{background:#37474f22;color:var(--gray);border:1px solid var(--dim)}
  .act{background:#00e67622;color:var(--green);border:1px solid var(--green)}
  .ext{background:#00e5ff22;color:var(--cyan);border:1px solid var(--cyan)}
  .warn{background:#ffd60022;color:var(--yellow);border:1px solid var(--yellow)}
  .bar-wrap{background:var(--border);border-radius:4px;height:8px;
            margin:10px 0 4px;overflow:hidden}
  .bar-fill{height:100%;border-radius:4px;transition:width .6s ease}
  .bar-label{display:flex;justify-content:space-between;
             font-size:.65rem;color:var(--gray)}
  .ua-bar-wrap{background:var(--border);border-radius:4px;height:6px;
               margin-top:6px;overflow:hidden}
  .ua-bar-fill{height:100%;border-radius:4px;transition:width .6s ease;
               background:var(--cyan)}
  .ua-bar-label{display:flex;justify-content:space-between;
                font-size:.6rem;color:var(--dim);margin-top:2px}
  .duty-ring{width:80px;height:80px;margin:0 auto}
  svg.ring circle{fill:none;stroke-width:7}
  .ring-bg{stroke:var(--border)}
  .ring-val{stroke-dasharray:220;stroke-dashoffset:220;
            stroke-linecap:round;transition:stroke-dashoffset .6s ease}
  .ctrl-wrap{background:var(--panel);border:1px solid var(--border);
             border-radius:6px;padding:14px;margin-bottom:10px}
  .ctrl-title{font-size:.65rem;letter-spacing:.15em;color:var(--gray);
              text-transform:uppercase;margin-bottom:12px}
  .alvo-row{display:flex;align-items:center;gap:10px;justify-content:center;
            margin-bottom:14px}
  .alvo-val{font-family:'Share Tech Mono',monospace;font-size:2.2rem;
            color:var(--yellow);min-width:80px;text-align:center}
  .btn{border:none;border-radius:5px;padding:10px 20px;font-family:'Rajdhani',sans-serif;
       font-size:1rem;font-weight:700;cursor:pointer;letter-spacing:.05em;
       transition:transform .1s,opacity .2s;user-select:none}
  .btn:active{transform:scale(.94)}
  .btn-up {background:#ffd60022;color:var(--yellow);border:1px solid var(--yellow)}
  .btn-dn {background:#ffd60022;color:var(--yellow);border:1px solid var(--yellow)}
  .btn-set {background:#00e67622;color:var(--green);border:1px solid var(--green);
            width:100%;margin-top:4px;padding:12px}
  .btn-stop{background:#ff3b3b22;color:var(--red);border:1px solid var(--red);
            width:100%;margin-top:8px;padding:12px}
  .btn-step{display:flex;gap:8px;justify-content:center;margin-bottom:8px}
  .step-btn{background:var(--border);color:var(--gray);border:1px solid var(--dim);
            border-radius:4px;padding:4px 10px;font-size:.75rem;cursor:pointer}
  .step-btn.sel{background:#ffd60033;color:var(--yellow);border-color:var(--yellow)}
  .status-row{display:flex;gap:8px;flex-wrap:wrap;justify-content:center;
              margin-bottom:10px}
  .footer{font-family:'Share Tech Mono',monospace;font-size:.6rem;
          color:var(--dim);text-align:center;padding-top:6px}
</style>
</head>
<body>
  <h1>&#x25C6; SECADORA &#x25C6;</h1>
  <div class="ip" id="ip-label">Carregando...</div>

  <div class="status-row">
    <span class="badge" id="badge-heat">HEAT</span>
    <span class="badge" id="badge-fan">FAN</span>
    <span class="badge" id="badge-exaust">EXT</span>
    <span class="badge off" id="badge-delta">dT --</span>
    <span class="badge" id="badge-wifi">WiFi</span>
    <span class="badge" id="badge-mode">AUTO</span>
  </div>

  <div class="grid">
    <div class="card">
      <div class="card-label">&#x25B2; Topo</div>
      <div class="card-val" id="t1">--<span>°C</span></div>
      <div class="card-sub" id="h1">-- %UR</div>
    </div>
    <div class="card">
      <div class="card-label">&#x25BC; Copa</div>
      <div class="card-val" id="t2">--<span>°C</span></div>
      <div class="card-sub" id="h2">-- %UR</div>
    </div>
  </div>

  <div class="grid">
    <div class="card" style="grid-column: span 2">
      <div style="display:flex;justify-content:space-between;align-items:center">
        <div>
          <div class="card-label">Média Geral</div>
          <div class="card-val" id="t_med">--<span>°C</span></div>
          <div class="card-sub" id="h_med">-- %UR</div>
        </div>
        <div style="text-align:right">
          <div class="card-label">Umidade Absoluta</div>
          <div class="card-val" style="color:var(--magenta)" id="ua_val">--<span style="font-size:.65rem;color:var(--dim)"> g/m³</span></div>
          <div class="ua-bar-wrap" style="width:100px;display:inline-block">
            <div class="ua-bar-fill" id="ua-bar" style="width:0%"></div>
          </div>
        </div>
      </div>
      <div class="bar-wrap">
        <div class="bar-fill" id="temp-bar" style="width:0%;background:var(--cyan)"></div>
      </div>
      <div class="bar-label">
        <span>20°C</span>
        <span id="bar-pct">0%</span>
        <span id="lbl-alvo-bar">--°C</span>
      </div>
    </div>
  </div>

  <div class="grid">
    <div class="card" style="display:flex;align-items:center;justify-content:center">
      <div class="duty-ring">
        <svg class="ring" viewBox="0 0 80 80" width="80" height="80">
          <circle class="ring-bg" cx="40" cy="40" r="35"/>
          <circle class="ring-val" id="ring-heat" cx="40" cy="40" r="35" transform="rotate(-90 40 40)" stroke="var(--red)"/>
          <text x="40" y="44" font-family="'Share Tech Mono',monospace" font-size="14" fill="var(--text)" text-anchor="middle" id="txt-heat">0%</text>
        </svg>
      </div>
    </div>
    <div class="card">
      <div class="card-label">Fluxo de Ar</div>
      <div class="card-sub" style="color:var(--green)" id="txt-fan">FAN: 0%</div>
      <div class="card-sub" style="color:var(--cyan)" id="txt-ext">EXT: 0%</div>
    </div>
  </div>

  <div class="ctrl-wrap">
    <div class="ctrl-title">Configurar Temperatura</div>
    <div class="btn-step">
      <button class="step-btn" onclick="setStep(1)">±1°C</button>
      <button class="step-btn sel" onclick="setStep(5)">±5°C</button>
    </div>
    <div class="alvo-row">
      <button class="btn btn-dn" onclick="ajustar(-1)">-</button>
      <div class="alvo-val" id="alvo-val">--°C</div>
      <button class="btn btn-up" onclick="ajustar(1)">+</button>
    </div>
    <button class="btn btn-set" onclick="enviarAlvo()">APLICAR ALTERAÇÃO</button>
    <button class="btn btn-stop" id="btn-manual" onclick="toggleManual()">ENTRAR EM MODO MANUAL</button>
  </div>

  <div class="footer">
    UPTIME: <span id="uptime">00:00:00</span>
  </div>

<script>
let passo=5;let alvoAtual=48;let modoManual=false;
function setStep(s){passo=s;document.querySelectorAll('.step-btn').forEach(b=>b.classList.remove('sel'));event.target.classList.add('sel')}
function ajustar(dir){alvoAtual+=dir*passo;if(alvoAtual<30)alvoAtual=30;if(alvoAtual<30)alvoAtual=30;if(alvoAtual>75)alvoAtual=75;document.getElementById('alvo-val').innerText=alvoAtual+'°C'}
function enviarAlvo(){fetch('/set?alvo='+alvoAtual).then(r=>r.json()).then(atualizar)}
function toggleManual(){fetch('/manual?toggle=1').then(r=>r.json()).then(atualizar)}
function atualizar(j){
  if(!j)return;
  document.getElementById('t1').innerHTML=j.t1.toFixed(1)+'<span>°C</span>';
  document.getElementById('h1').innerText=j.h1.toFixed(0)+' %UR';
  document.getElementById('t2').innerHTML=j.t2.toFixed(1)+'<span>°C</span>';
  document.getElementById('h2').innerText=j.h2.toFixed(0)+' %UR';
  let tMed=(j.t1+j.t2)/2;let hMed=(j.h1+j.h2)/2;
  document.getElementById('t_med').innerHTML=tMed.toFixed(1)+'<span>°C</span>';
  document.getElementById('h_med').innerText=hMed.toFixed(0)+' %UR';
  document.getElementById('ua_val').innerHTML=j.ua.toFixed(1)+'<span style="font-size:.65rem;color:var(--dim)"> g/m³</span>';
  let uaPct=Math.min(Math.max((j.ua-12)/(25-12)*100,0),100);
  document.getElementById('ua-bar').style.width=uaPct+'%';
  if(!event||event.target.className!=='btn btn-up'&&event.target.className!=='btn btn-dn'){
    alvoAtual=j.alvo;document.getElementById('alvo-val').innerText=alvoAtual+'°C';
  }
  document.getElementById('lbl-alvo-bar').innerText=j.alvo+'°C';
  let pct=Math.min(Math.max((tMed-20)/(j.alvo-20)*100,0),100);
  let tBar=document.getElementById('temp-bar');tBar.style.width=pct+'%';
  tBar.style.background=pct<60?'var(--cyan)':pct<85?'var(--yellow)':'var(--green)';
  document.getElementById('bar-pct').innerText=pct.toFixed(0)+'%';
  let hPct=Math.min(Math.max(j.duty/255*100,0),100);
  document.getElementById('txt-heat').innerText=hPct.toFixed(0)+'%';
  document.getElementById('ring-heat').style.strokeDashoffset=220-(220*hPct/100);
  document.getElementById('txt-fan').innerText='FAN: '+Math.round(j.f_duty/255*100)+'%';
  document.getElementById('txt-ext').innerText='EXT: '+Math.round(j.e_duty/255*100)+'%';
  let bHeat=document.getElementById('badge-heat');bHeat.className='badge '+(j.h_on?'on':'off');
  let bFan=document.getElementById('badge-fan');bFan.className='badge '+(j.f_on?'act':'off');
  let bExt=document.getElementById('badge-exaust');bExt.className='badge '+(j.e_on?'ext':'off');
  let dT=j.t1-j.t2;let bDt=document.getElementById('badge-delta');
  bDt.innerText='dT '+(dT>=0?'+':'')+dT.toFixed(1)+'°C';
  let adT=Math.abs(dT);bDt.className='badge '+(adT<2?'act':adT<5?'warn':adT<10?'off':'on');
  document.getElementById('badge-wifi').className='badge '+(j.wf?'act':'off');
  modoManual=j.m_man;
  let bMan=document.getElementById('badge-mode');bMan.innerText=modoManual?'MANUAL':'AUTO';
  bMan.className='badge '+(modoManual?'on':'act');
  let btnM=document.getElementById('btn-manual');btnM.innerText=modoManual?'SAIR DO MODO MANUAL':'ENTRAR EM MODO MANUAL';
  btnM.className='btn '+ (modoManual?'btn-set':'btn-stop');
  let s=j.up;let h=Math.floor(s/3600);let m=Math.floor((s%3600)/60);let sec=s%60;
  document.getElementById('uptime').innerText=(h<10?'0':'')+h+':'+(m<10?'0':'')+m+':'+(sec<10?'0':'')+sec;
  document.getElementById('ip-label').innerText='IP LOCAL: '+j.ip;
}
function loadData(){fetch('/data').then(r=>r.json()).then(atualizar).catch(()=>{})}
setInterval(loadData,2000);window.onload=loadData;
</script>
</body>
</html>)HTMLPAGE";
