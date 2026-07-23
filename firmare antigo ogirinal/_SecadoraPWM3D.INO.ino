/*******************************************************
 * SECADORA — PID + WebServer de controle
 * v3.1 — IP Fixo: http://192.168.1.100
 *
 * Acesse http://192.168.1.100 no navegador (mesma rede WiFi)
 *
 * Saídas:
 *   PIN_HEATER  (32) — PTC aquecedor PWM (SSR/MOSFET)
 *   PIN_FAN_INT (33) — Ventoinha interna (relay)
 *   Fan interno liga/desliga por delta T (topo - copa)
 *
 * Colunas planilha:
 * TIMESTAMP | T_TOPO | H_TOPO | T_COPA | H_COPA |
 * T_MEDIA | H_MEDIA | UA | DELTA | HEATER | DUTY | FAN | EXAUST | T_ALVO
 *******************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_SHT31.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <time.h>

// ─── WiFi ───────────────────────────────────────────────
#define WIFI_SSID  "APT_07"
#define WIFI_PASS  "REDIGIDO"   // senha real removida antes de versionar (era de outra rede/local)

// ─── IP Fixo ────────────────────────────────────────────
// Acesse sempre: http://192.168.1.100
// Ajuste os 3 valores conforme sua rede
IPAddress staticIP (192, 168, 0, 250);  // IP desejado para o ESP32
IPAddress gateway  (192, 168, 0,   1);  // IP do seu roteador
IPAddress subnet   (255, 255, 255, 0);  // Máscara (quase sempre essa)

// ─── Google Sheets ──────────────────────────────────────
#define APPS_SCRIPT_URL  "https://script.google.com/macros/s/AKfycbxeqNioAF2gh3w7e_A9GlTeXXlPp1Wgl_3yPYjmTDH2mwYSjKOKhkk7K52pFW_ZJEM0NA/exec"
#define INTERVALO_LOG_MS  (5UL * 60UL * 1000UL)
#define NTP_SERVER        "pool.ntp.org"
#define NTP_OFFSET        -10800

// ─── PID ────────────────────────────────────────────────
// PID ajustado: KP alto para forcar subida, KI acumula em regime, KD suave
#define KP               25.0   // era 10 — sobe mais rapido ao alvo
#define KI                0.30   // era 0.08 — corrige erro em regime
#define KD                1.0   // era 3.0 — menos freio na subida
#define PID_INTEGRAL_MAX  200.0
#define PID_INTEGRAL_MIN    0.0  // nao deixa integral negativo (so aquece)

// ─── PWM ────────────────────────────────────────────────
// ESP32 core v3.x: ledcAttach(pin, freq, bits) — sem canal explícito
#define PWM_FREQ_HEAT     1000   // Hz
#define PWM_BITS_HEAT     8      // resolução 0–255

// ─── Temperatura alvo ───────────────────────────────────
#define TEMP_ALVO_PADRAO  48.0
#define TEMP_ALVO_MIN     30.0
#define TEMP_ALVO_MAX     75.0

// ─── Exaustão ───────────────────────────────────────────
// Fan interno controlado pelo delta topo-copa
// Fan PWM — velocidade varia com o delta topo-copa
#define FAN_PWM_FREQ    25000  // Hz — 25kHz para nao ouvir chiado
#define FAN_PWM_BITS    8      // 0-255
#define FAN_DELTA_MIN   0.5   // graus C — abaixo disso fan em velocidade minima
#define FAN_DELTA_MAX   8.0   // graus C — acima disso fan em 100%
#define FAN_DUTY_MIN    80    // duty minimo (fans sempre girando levemente)
#define FAN_DUTY_MAX    255   // duty maximo
#define SMOOTH_FACTOR  0.15

// ─── Pinos ──────────────────────────────────────────────
#define PIN_HEATER   32
#define PIN_FAN_INT  33
// PIN_EXAUST removido - apenas 1 MOSFET disponivel
#define I2C_SDA      21
#define I2C_SCL      22

// ─── Sensores ───────────────────────────────────────────
#define SHT31_TOPO_ADDR  0x45
#define SHT31_COPA_ADDR  0x44

// ─── Display ────────────────────────────────────────────
#define TFT_CS   5
#define TFT_RST  4
#define TFT_DC   2

#define C_BG       ST77XX_BLACK
#define C_BORDER   0x0480
#define C_WHITE    ST77XX_WHITE
#define C_GREEN    0x07E0
#define C_YELLOW   0xFFE0
#define C_CYAN     0x07FF
#define C_MAGENTA  0xF81F
#define C_RED      0xF800
#define C_GRAY     0x8410
#define C_DARKGRAY 0x2104
#define C_ORANGE   0xFD20

#define Y_LBL_SNS    3
#define Y_HL0       13
#define Y_TEMP      16
#define Y_HUM       36
#define Y_HL1       46
#define Y_AVG_LBL   49
#define Y_AVG_VAL   59
#define Y_HL2       69
#define Y_DELTA     72
#define Y_HL3       82
#define Y_CTRL      85
#define Y_BAR       96
#define Y_HL4      105
#define Y_FOOTER   109
#define ICON_W       9

// ════════════════════════════════════════════════════════
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
Adafruit_SHT31  shtTop;
Adafruit_SHT31  shtBot;
WebServer       server(80);

// ─── Sensores / filtros ─────────────────────────────────
float filtTemp = 0, filtHum = 0, filtUA = 0;
float rawT1 = 0, rawH1 = 0, rawT2 = 0, rawH2 = 0;

// ─── Display state ──────────────────────────────────────
float lastTopT=-999, lastTopH=-999, lastBotT=-999, lastBotH=-999;
float lastAvgT=-999, lastAvgH=-999, lastAvgUA=-999;
float lastDelta=-999, lastPct=-1, lastAlvo=-999;
bool  lastHeater=false, lastFan=false, lastExaust=false, lastWifiIcon=false;
uint32_t uptimeSec = 0;

// ─── Controle ───────────────────────────────────────────
float tempAlvo  = TEMP_ALVO_PADRAO;
bool  heaterON  = false;
bool  fanON     = false;
int   fanDuty   = 0;     // PWM do fan 0-255
bool  modoManual = false;   // true = usuário desligou manualmente pelo web
int   dutyAtual = 0;

// ─── PID ────────────────────────────────────────────────
float pidIntegral  = 0;
float pidLastError = 0;
unsigned long pidLastTime = 0;

// ─── WiFi / log ─────────────────────────────────────────
bool wifiOK = false, ntpOK = false;
unsigned long tUltimoLog = 0;

// ════════════════════════════════════════════════════════
// Auxiliares
// ════════════════════════════════════════════════════════
float calcUA(float t, float h) {
  return (6.112 * exp((17.67 * t) / (t + 243.5)) * h * 2.1674) / (273.15 + t);
}
float smooth(float prev, float next) {
  return prev + (next - prev) * SMOOTH_FACTOR;
}
void hline(int y, uint16_t c = C_BORDER) {
  tft.drawFastHLine(0, y, 160, c);
}
void iconThermo(int x, int y, uint16_t color) {
  tft.drawFastVLine(x+3,y,5,color); tft.drawFastVLine(x+4,y,5,color);
  tft.drawPixel(x+2,y+1,color); tft.drawPixel(x+5,y+1,color);
  tft.drawPixel(x+2,y+3,color); tft.drawPixel(x+5,y+3,color);
  tft.fillCircle(x+3,y+6,2,color);
}
void iconDrop(int x, int y, uint16_t color) {
  tft.drawPixel(x+3,y,color); tft.drawPixel(x+4,y,color);
  tft.drawFastHLine(x+2,y+1,4,color); tft.drawFastHLine(x+1,y+2,6,color);
  tft.drawFastHLine(x+1,y+3,6,color); tft.drawFastHLine(x+1,y+4,6,color);
  tft.drawFastHLine(x+2,y+5,4,color); tft.drawFastHLine(x+3,y+6,2,color);
}

// ════════════════════════════════════════════════════════
// Display
// ════════════════════════════════════════════════════════
void drawTemp(int x, int y, float t, float &last) {
  if (abs(t-last)<0.09) return;
  tft.fillRect(x,y,76,17,C_BG);
  iconThermo(x,y+4,C_RED);
  tft.setTextSize(2); tft.setTextColor(C_WHITE);
  tft.setCursor(x+ICON_W,y);
  char buf[7]; dtostrf(t,4,1,buf); tft.print(buf);
  tft.setTextSize(1); tft.setTextColor(C_GRAY); tft.print("c");
  last=t;
}
void drawHum(int x, int y, float h, float &last) {
  if (abs(h-last)<0.4) return;
  tft.fillRect(x,y,76,8,C_BG);
  iconDrop(x,y,C_CYAN);
  tft.setTextSize(1); tft.setTextColor(C_CYAN);
  tft.setCursor(x+ICON_W,y); tft.print(h,0); tft.print("%");
  last=h;
}
void drawAverages() {
  if (abs(filtTemp-lastAvgT)>0.09) {
    tft.fillRect(3,Y_AVG_VAL,52,8,C_BG);
    iconThermo(3,Y_AVG_VAL,C_RED);
    tft.setTextSize(1); tft.setTextColor(C_YELLOW);
    tft.setCursor(3+ICON_W,Y_AVG_VAL); tft.print(filtTemp,1); tft.print("c");
    lastAvgT=filtTemp;
  }
  if (abs(filtHum-lastAvgH)>0.4) {
    tft.fillRect(58,Y_AVG_VAL,52,8,C_BG);
    iconDrop(58,Y_AVG_VAL,C_CYAN);
    tft.setTextSize(1); tft.setTextColor(C_CYAN);
    tft.setCursor(58+ICON_W,Y_AVG_VAL); tft.print(filtHum,0); tft.print("%");
    lastAvgH=filtHum;
  }
  if (abs(filtUA-lastAvgUA)>0.09) {
    tft.fillRect(113,Y_AVG_VAL,44,8,C_BG);
    tft.setTextSize(1); tft.setTextColor(C_MAGENTA);
    tft.setCursor(113,Y_AVG_VAL); tft.print(filtUA,1);
    lastAvgUA=filtUA;
  }
}
uint16_t deltaColor(float d) {
  float a=abs(d);
  if(a<2.0)  return C_GREEN;
  if(a<5.0)  return C_YELLOW;
  if(a<10.0) return C_ORANGE;
  return C_RED;
}
void drawDelta(float t1, float t2) {
  float delta=t1-t2;
  if(abs(delta-lastDelta)<0.09) return;
  tft.fillRect(0,Y_DELTA,160,9,C_BG);
  uint16_t col=deltaColor(delta);
  tft.setTextSize(1); tft.setTextColor(C_GRAY);
  tft.setCursor(0,Y_DELTA); tft.print("dT");
  tft.setTextColor(col); tft.setCursor(14,Y_DELTA);
  if(delta>=0) tft.print("+");
  tft.print(delta,1); tft.print("c");
  int cx=120, bw=38;
  int fill=constrain((int)(abs(delta)/20.0*bw),0,bw);
  tft.fillRect(cx-bw,Y_DELTA,bw*2,7,C_DARKGRAY);
  tft.drawFastVLine(cx,Y_DELTA,7,C_GRAY);
  if(delta>0)      tft.fillRect(cx,       Y_DELTA,fill,7,col);
  else if(delta<0) tft.fillRect(cx-fill,  Y_DELTA,fill,7,col);
  lastDelta=delta;
}
void drawCtrlLine() {
  if(abs(tempAlvo-lastAlvo)<0.05 &&
     heaterON==lastHeater && fanON==lastFan) return;
  tft.fillRect(0,Y_CTRL,160,9,C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY); tft.setCursor(6,Y_CTRL); tft.print("A:");
  tft.setTextColor(C_RED);  tft.print(tempAlvo,0);   tft.print("c");
  tft.setCursor(60,Y_CTRL);
  tft.setTextColor(fanON     ? C_GREEN    : C_DARKGRAY); tft.print("FAN");
  tft.setCursor(112,Y_CTRL);
  int pct=map(dutyAtual,0,255,0,100);
  uint16_t dc=(pct<40)?C_DARKGRAY:(pct<75)?C_YELLOW:C_RED;
  tft.setTextColor(dc);
  char dbuf[6]; snprintf(dbuf,sizeof(dbuf),"%3d%%",pct); tft.print(dbuf);
  tft.setCursor(142,Y_CTRL);
  if(heaterON){ tft.setTextColor(C_RED);      tft.print("*ON"); }
  else        { tft.setTextColor(C_DARKGRAY); tft.print("OFF"); }
  lastAlvo=tempAlvo; lastHeater=heaterON; lastFan=fanON;
}
void drawBar() {
  float pct=constrain((filtTemp-20.0)/(tempAlvo-20.0),0.0,1.0);
  if(abs(pct-lastPct)<0.005) return;
  tft.fillRoundRect(3,Y_BAR,148,6,2,C_DARKGRAY);
  uint16_t col=(pct<0.6)?C_CYAN:(pct<0.85)?C_YELLOW:C_GREEN;
  int barW=(int)(pct*147);
  if(barW>0) tft.fillRoundRect(3,Y_BAR,barW,6,2,col);
  tft.drawFastVLine(151,Y_BAR-2,10,C_RED);
  lastPct=pct;
}
void drawWifiIcon() {
  bool con=(WiFi.status()==WL_CONNECTED);
  if(con==lastWifiIcon) return;
  tft.fillRect(140,Y_FOOTER,20,8,C_BG);
  tft.setTextSize(1);
  if(con){ tft.setTextColor(C_GREEN);  tft.setCursor(140,Y_FOOTER); tft.print("WiFi"); }
  else   { tft.setTextColor(C_GRAY);   tft.setCursor(140,Y_FOOTER); tft.print("----"); }
  lastWifiIcon=con;
}
void drawUptime() {
  uint32_t s=millis()/1000;
  if(s==uptimeSec) return;
  uptimeSec=s;
  tft.fillRect(18,Y_FOOTER,62,8,C_BG);
  tft.setTextSize(1); tft.setTextColor(C_DARKGRAY);
  tft.setCursor(18,Y_FOOTER);
  char buf[10];
  snprintf(buf,sizeof(buf),"%02lu:%02lu:%02lu",s/3600,(s%3600)/60,s%60);
  tft.print(buf);
}
void drawStatic() {
  tft.fillScreen(C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_GREEN);
  tft.setCursor(3,Y_LBL_SNS);  tft.print("TOPO");
  tft.setCursor(83,Y_LBL_SNS); tft.print("COPA");
  hline(Y_HL0); hline(Y_HL1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(3,Y_AVG_LBL);   tft.print("MED");
  tft.setCursor(58,Y_AVG_LBL);  tft.print("UM");
  tft.setCursor(113,Y_AVG_LBL); tft.print("UA");
  hline(Y_HL2); hline(Y_HL3);
  tft.fillRoundRect(3,Y_BAR,148,6,2,C_DARKGRAY);
  tft.drawFastVLine(151,Y_BAR-2,10,C_RED);
  hline(Y_HL4);
  tft.setTextColor(C_GRAY);
  tft.setCursor(3,Y_FOOTER); tft.print("UP");
}

// ════════════════════════════════════════════════════════
// PID
// ════════════════════════════════════════════════════════
void computePID(float t1) {
  if(modoManual) {
    dutyAtual=0; ledcWrite(PIN_HEATER,0); heaterON=false; return;
  }
  unsigned long agora=millis();
  float dt=constrain((agora-pidLastTime)/1000.0f,0.01f,2.0f);
  pidLastTime=agora;
  float erro=tempAlvo-t1;
  pidIntegral+=erro*dt;
  pidIntegral=constrain(pidIntegral,PID_INTEGRAL_MIN,PID_INTEGRAL_MAX);
  float derivada=(erro-pidLastError)/dt;
  pidLastError=erro;
  float saida=KP*erro+KI*pidIntegral+KD*derivada;
  dutyAtual=(int)constrain(saida,0.0f,255.0f);
  ledcWrite(PIN_HEATER,dutyAtual);
  heaterON=(dutyAtual>8);
}

// ════════════════════════════════════════════════════════
// Fan / Exaust
// ════════════════════════════════════════════════════════
void controleFanExaust() {
  // Fan com PWM proporcional ao delta topo-copa
  // Quanto maior o desequilibrio, mais rapido o fan gira
  // Sempre girando no minimo para manter circulacao base
  float delta = abs(rawT1 - rawT2);

  // Mapeia delta para duty: FAN_DELTA_MIN->FAN_DUTY_MIN, FAN_DELTA_MAX->FAN_DUTY_MAX
  float ratio = (delta - FAN_DELTA_MIN) / (FAN_DELTA_MAX - FAN_DELTA_MIN);
  ratio = constrain(ratio, 0.0f, 1.0f);
  fanDuty = (int)(FAN_DUTY_MIN + ratio * (FAN_DUTY_MAX - FAN_DUTY_MIN));
  fanON   = true;  // fan sempre ligado (velocidade minima ou mais)

  ledcWrite(PIN_FAN_INT, fanDuty);
}

// ════════════════════════════════════════════════════════
// Web Server — página + API JSON
// ════════════════════════════════════════════════════════
void handleRoot() {
  // Página HTML completa embutida — responsiva, tema escuro industrial
  String html = R"rawhtml(<!DOCTYPE html>
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
  .on {background:#ff3b3b22;color:var(--red);  border:1px solid var(--red)}
  .off{background:#37474f22;color:var(--gray); border:1px solid var(--dim)}
  .act{background:#00e67622;color:var(--green);border:1px solid var(--green)}

  /* barra de progresso */
  .bar-wrap{background:var(--border);border-radius:4px;height:8px;
            margin:10px 0 4px;overflow:hidden}
  .bar-fill{height:100%;border-radius:4px;transition:width .6s ease}
  .bar-label{display:flex;justify-content:space-between;
             font-size:.65rem;color:var(--gray)}

  /* duty ring */
  .duty-ring{width:80px;height:80px;margin:0 auto}
  svg.ring circle{fill:none;stroke-width:7}
  .ring-bg{stroke:var(--border)}
  .ring-val{stroke-dasharray:220;stroke-dashoffset:220;
            stroke-linecap:round;transition:stroke-dashoffset .6s ease}

  /* controles */
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
  .btn-up  {background:#ffd60022;color:var(--yellow);border:1px solid var(--yellow)}
  .btn-dn  {background:#ffd60022;color:var(--yellow);border:1px solid var(--yellow)}
  .btn-set {background:#00e67622;color:var(--green); border:1px solid var(--green);
            width:100%;margin-top:4px;padding:12px}
  .btn-stop{background:#ff3b3b22;color:var(--red);   border:1px solid var(--red);
            width:100%;margin-top:8px;padding:12px}
  .btn-step{display:flex;gap:8px;justify-content:center;margin-bottom:8px}
  .step-btn{background:var(--border);color:var(--gray);border:1px solid var(--dim);
            border-radius:4px;padding:4px 10px;font-size:.75rem;cursor:pointer}
  .step-btn.sel{background:#ffd60033;color:var(--yellow);border-color:var(--yellow)}

  /* status row */
  .status-row{display:flex;gap:8px;flex-wrap:wrap;justify-content:center;
              margin-bottom:10px}
  /* footer */
  .footer{font-family:'Share Tech Mono',monospace;font-size:.6rem;
          color:var(--dim);text-align:center;padding-top:6px}
  .dot{display:inline-block;width:6px;height:6px;border-radius:50%;
       margin-right:4px;vertical-align:middle}
  .dot-green{background:var(--green)}
  .dot-red  {background:var(--red)}
  .dot-gray {background:var(--dim)}
</style>
</head>
<body>
<h1>&#x25C6; SECADORA &#x25C6;</h1>
<div class="ip" id="ip-label">Carregando...</div>

<!-- Status badges -->
<div class="status-row">
  <span class="badge" id="badge-heat">HEAT</span>
  <span class="badge" id="badge-fan">FAN</span>
  <span class="badge off" id="badge-delta">dT --</span>
  <span class="badge" id="badge-wifi">WiFi</span>
  <span class="badge" id="badge-mode">AUTO</span>
</div>

<!-- Sensores -->
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

<!-- Média + UA -->
<div class="grid">
  <div class="card">
    <div class="card-label">Média</div>
    <div class="card-val" id="tmed">--<span>°C</span></div>
    <div class="card-sub" id="hmed">-- %UR</div>
  </div>
  <div class="card">
    <div class="card-label">UA / Delta</div>
    <div class="card-val" id="ua" style="font-size:1.2rem">--<span>g/m³</span></div>
    <div class="card-sub" id="delta">dT: --</div>
  </div>
</div>

<!-- Duty cycle ring + barra -->
<div class="card" style="margin-bottom:10px">
  <div class="card-label">PID — Potência do aquecedor</div>
  <div class="duty-ring">
    <svg class="ring" viewBox="0 0 80 80">
      <circle class="ring-bg"  cx="40" cy="40" r="35"/>
      <circle class="ring-val" id="ring-arc" cx="40" cy="40" r="35"
              stroke="#ff3b3b" transform="rotate(-90 40 40)"/>
    </svg>
  </div>
  <div style="text-align:center;font-family:'Share Tech Mono',monospace;
              font-size:1.4rem;color:var(--red);margin-top:-56px" id="duty-pct">0%</div>
  <div style="margin-top:56px">
    <div class="bar-label">
      <span>20°C</span><span id="bar-cur">--</span><span id="bar-alvo">--°C</span>
    </div>
    <div class="bar-wrap">
      <div class="bar-fill" id="bar-fill" style="width:0%;background:var(--cyan)"></div>
    </div>
  </div>
</div>

<!-- Controles -->
<div class="ctrl-wrap">
  <div class="ctrl-title">&#x25CE; Temperatura Alvo</div>
  <div class="btn-step" id="step-btns">
    <button class="step-btn" onclick="setStep(0.5)">0.5°</button>
    <button class="step-btn sel" onclick="setStep(1)">1°</button>
    <button class="step-btn" onclick="setStep(5)">5°</button>
    <button class="step-btn" onclick="setStep(10)">10°</button>
  </div>
  <div class="alvo-row">
    <button class="btn btn-dn" onclick="ajustar(-step)">&#x25BC;</button>
    <div class="alvo-val" id="alvo-disp">--°C</div>
    <button class="btn btn-up" onclick="ajustar(+step)">&#x25B2;</button>
  </div>
  <button class="btn btn-set" onclick="enviarAlvo()">&#x2713; CONFIRMAR ALVO</button>
  <button class="btn btn-stop" id="btn-emerg" onclick="toggleEmerg()">&#x25A0; PARAR AQUECIMENTO</button>
</div>

<div class="footer" id="footer-up">UP: --  |  atualiza a cada 3s</div>

<script>
let step = 1;
let alvoLocal = )rawhtml";

  html += String(tempAlvo, 1);

  html += R"rawhtml(;
let emergencia = false;

function setStep(v){
  step=v;
  document.querySelectorAll('.step-btn').forEach(b=>{
    b.classList.toggle('sel', parseFloat(b.textContent)===v);
  });
}

function ajustar(delta){
  alvoLocal = Math.min(75, Math.max(30, Math.round((alvoLocal+delta)*10)/10));
  document.getElementById('alvo-disp').textContent = alvoLocal.toFixed(1)+'°C';
}

function enviarAlvo(){
  fetch('/set?alvo='+alvoLocal)
    .then(r=>r.json())
    .then(d=>{ if(d.ok) pulse('alvo-disp'); });
}

function toggleEmerg(){
  emergencia=!emergencia;
  fetch('/emerg?v='+(emergencia?1:0))
    .then(r=>r.json())
    .then(d=>{
      const b=document.getElementById('btn-emerg');
      if(emergencia){
        b.textContent='▶ RETOMAR AQUECIMENTO';
        b.style.background='#00e67622'; b.style.color='var(--green)';
        b.style.borderColor='var(--green)';
      } else {
        b.textContent='■ PARAR AQUECIMENTO';
        b.style.background='#ff3b3b22'; b.style.color='var(--red)';
        b.style.borderColor='var(--red)';
      }
    });
}

function pulse(id){
  const el=document.getElementById(id);
  el.style.transition='color .1s'; el.style.color='#fff';
  setTimeout(()=>{ el.style.color=''; },300);
}

function badgeSet(id, on, labelOn, labelOff, classOn){
  const el=document.getElementById(id);
  el.textContent=on?labelOn:labelOff;
  el.className='badge '+(on?classOn:'off');
}

function atualizar(){
  fetch('/status')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('t1').innerHTML    = d.t1.toFixed(1)+'<span>°C</span>';
      document.getElementById('h1').textContent  = d.h1.toFixed(0)+' %UR';
      document.getElementById('t2').innerHTML    = d.t2.toFixed(1)+'<span>°C</span>';
      document.getElementById('h2').textContent  = d.h2.toFixed(0)+' %UR';
      document.getElementById('tmed').innerHTML  = d.tmed.toFixed(1)+'<span>°C</span>';
      document.getElementById('hmed').textContent= d.hmed.toFixed(0)+' %UR';
      document.getElementById('ua').innerHTML    = d.ua.toFixed(1)+'<span>g/m³</span>';
      document.getElementById('delta').textContent='dT: '+(d.delta>=0?'+':'')+d.delta.toFixed(1)+'°C';

      // Duty
      const pct=d.duty;
      document.getElementById('duty-pct').textContent=pct+'%';
      const offset=220-(220*pct/100);
      document.getElementById('ring-arc').style.strokeDashoffset=offset;

      // Barra progresso
      const prog=Math.min(100,Math.max(0,(d.tmed-20)/(d.alvo-20)*100));
      const fill=document.getElementById('bar-fill');
      fill.style.width=prog+'%';
      fill.style.background=prog<60?'var(--cyan)':prog<85?'var(--yellow)':'var(--green)';
      document.getElementById('bar-cur').textContent=d.tmed.toFixed(1)+'°C';
      document.getElementById('bar-alvo').textContent=d.alvo.toFixed(0)+'°C';

      // Alvo (sincroniza se não estiver editando)
      alvoLocal=d.alvo;
      document.getElementById('alvo-disp').textContent=d.alvo.toFixed(1)+'°C';

      // Badges
      badgeSet('badge-heat', d.heater, 'HEAT ON',  'HEAT OFF', 'on');
      const fp=d.fanduty||0; badgeSet('badge-fan', fp>0, 'FAN '+fp+'%', 'FAN OFF', 'act');
      const dabs=Math.abs(d.delta); const dbadge=document.getElementById('badge-delta');
      dbadge.textContent='dT '+(d.delta>=0?'+':'')+d.delta.toFixed(1)+'°C';
      dbadge.className='badge '+(dabs>4?'on':dabs>2?'badge' +' ' + 'off':'act');
      badgeSet('badge-wifi', d.wifi,   'WiFi OK',  'Sem WiFi', 'act');
      badgeSet('badge-mode', d.manual, 'MANUAL',   'AUTO',
               d.manual?'on':'act');

      document.getElementById('ip-label').textContent='http://'+d.ip;

      const s=d.uptime;
      const up=String(Math.floor(s/3600)).padStart(2,'0')+':'+
               String(Math.floor((s%3600)/60)).padStart(2,'0')+':'+
               String(s%60).padStart(2,'0');
      document.getElementById('footer-up').textContent='UP: '+up+'  |  atualiza a cada 3s';
    })
    .catch(()=>{});
}

atualizar();
setInterval(atualizar, 3000);
</script>
</body>
</html>)rawhtml";

  server.send(200, "text/html", html);
}

void handleStatus() {
  float avgT = (rawT1 + rawT2) / 2.0;
  float avgH = (rawH1 + rawH2) / 2.0;
  float ua   = calcUA(filtTemp, filtHum);
  int   pct  = map(dutyAtual, 0, 255, 0, 100);

  String json = "{";
  json += "\"t1\":"     + String(rawT1,   2) + ",";
  json += "\"h1\":"     + String(rawH1,   1) + ",";
  json += "\"t2\":"     + String(rawT2,   2) + ",";
  json += "\"h2\":"     + String(rawH2,   1) + ",";
  json += "\"tmed\":"   + String(filtTemp,2) + ",";
  json += "\"hmed\":"   + String(filtHum, 1) + ",";
  json += "\"ua\":"     + String(ua,      2) + ",";
  json += "\"delta\":"  + String(rawT1-rawT2,2) + ",";
  json += "\"alvo\":"   + String(tempAlvo,1) + ",";
  json += "\"duty\":"   + String(pct)        + ",";
  json += "\"heater\":" + String(heaterON  ? "true":"false") + ",";
  json += "\"fan\":"    + String(fanON     ? "true":"false") + ",";
  json += "\"manual\":" + String(modoManual ? "true":"false") + ",";
  json += "\"wifi\":"   + String(WiFi.status()==WL_CONNECTED ? "true":"false") + ",";
  json += "\"ip\":\""   + WiFi.localIP().toString() + "\",";
  json += "\"uptime\":" + String(millis()/1000);
  json += "}";

  server.send(200, "application/json", json);
}

void handleSetAlvo() {
  if (server.hasArg("alvo")) {
    float v = server.arg("alvo").toFloat();
    if (v >= TEMP_ALVO_MIN && v <= TEMP_ALVO_MAX) {
      tempAlvo    = v;
      pidIntegral = 0;   // zera integral ao mudar alvo
      lastAlvo    = -999;
      Serial.printf("Alvo atualizado via web: %.1f°C\n", tempAlvo);
    }
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleEmerg() {
  if (server.hasArg("v")) {
    modoManual = (server.arg("v") == "1");
    if (!modoManual) pidIntegral = 0;   // reset ao retomar
    Serial.printf("Modo manual: %s\n", modoManual ? "PARADO" : "AUTO");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// ════════════════════════════════════════════════════════
// WiFi / NTP / Log
// ════════════════════════════════════════════════════════
void conectarWiFi() {
  Serial.printf("Conectando WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.config(staticIP, gateway, subnet);  // IP fixo — sempre http://192.168.1.100
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

String getTimestamp() {
  if (!ntpOK) return "sem_horario";
  struct tm t;
  if (!getLocalTime(&t)) return "sem_horario";
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

void enviarLog() {
  if (WiFi.status() != WL_CONNECTED) return;

  float avgT = (rawT1+rawT2)/2.0;
  float avgH = (rawH1+rawH2)/2.0;
  float ua   = calcUA(avgT, avgH);
  int   pct  = map(dutyAtual,0,255,0,100);

  // Monta URL com todos os parametros
  String url = String(APPS_SCRIPT_URL);
  url += "?timestamp=" + getTimestamp();
  url += "&t_topo="  + String(rawT1,1);
  url += "&h_topo="  + String(rawH1,1);
  url += "&t_copa="  + String(rawT2,1);
  url += "&h_copa="  + String(rawH2,1);
  url += "&t_media=" + String(avgT,1);
  url += "&h_media=" + String(avgH,1);
  url += "&ua="      + String(ua,2);
  url += "&delta="   + String(rawT1-rawT2,1);
  url += "&heater="  + String(heaterON?1:0);
  url += "&duty="    + String(pct);
  url += "&fan="     + String(map(fanDuty,0,255,0,100));
  url += "&t_alvo="  + String(tempAlvo,1);
  url.replace(" ","%20");

  Serial.println("Log: "+url.substring(0,80));

  // Tentativa com retry — HTTP -1 e comum no ESP32 core v3 com HTTPS
  int code = -1;
  for (int tentativa = 1; tentativa <= 3 && code < 0; tentativa++) {
    // Recria client e http a cada tentativa para evitar estado corrompido
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);        // timeout SSL em segundos (core v3)

    HTTPClient http;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(20000);       // timeout HTTP em ms
    http.addHeader("Connection", "close");  // evita keep-alive que causa -1

    code = http.GET();
    Serial.printf("Tentativa %d — HTTP: %d\n", tentativa, code);

    http.end();

    if (code < 0) delay(2000);   // aguarda antes de tentar novamente
  }

  if (code == 200 || code == 302) {
    Serial.println("Log OK");
  } else {
    Serial.printf("Log falhou apos 3 tentativas. Ultimo codigo: %d\n", code);
  }
}

// ════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // ESP32 core v3.x — nova API LEDC (ledcSetup/ledcAttachPin foram removidos)
  ledcAttach(PIN_HEATER, PWM_FREQ_HEAT, PWM_BITS_HEAT);
  ledcWrite(PIN_HEATER, 0);

  // Fan PWM — 25kHz para nao causar ruido audivel nos motores
  ledcAttach(PIN_FAN_INT, FAN_PWM_FREQ, FAN_PWM_BITS);
  ledcWrite(PIN_FAN_INT, FAN_DUTY_MIN);  // inicia em velocidade minima

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(300);
  shtTop.begin(SHT31_TOPO_ADDR);
  delay(100);
  shtBot.begin(SHT31_COPA_ADDR);
  delay(500);

  float t1i=shtTop.readTemperature(), h1i=shtTop.readHumidity();
  float t2i=shtBot.readTemperature(), h2i=shtBot.readHumidity();
  if(!isnan(t1i)&&!isnan(t2i)){
    rawT1=t1i; rawH1=h1i; rawT2=t2i; rawH2=h2i;
    filtTemp=(t1i+t2i)/2.0;
    filtHum =(h1i+h2i)/2.0;
    filtUA  =calcUA(filtTemp,filtHum);
  }

  pidLastTime =millis();
  pidLastError=tempAlvo-filtTemp;

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  drawStatic();

  conectarWiFi();
  drawWifiIcon();

  // Rotas do web server
  server.on("/",        handleRoot);
  server.on("/status",  handleStatus);
  server.on("/set",     handleSetAlvo);
  server.on("/emerg",   handleEmerg);

  tUltimoLog = millis() - INTERVALO_LOG_MS;
}

// ════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════
void loop() {
  // Sensores
  float t1=shtTop.readTemperature(), h1=shtTop.readHumidity();
  float t2=shtBot.readTemperature(), h2=shtBot.readHumidity();

  if(!isnan(t1)&&!isnan(t2)&&!isnan(h1)&&!isnan(h2)){
    rawT1=t1; rawH1=h1; rawT2=t2; rawH2=h2;
    float avgT=(t1+t2)/2.0, avgH=(h1+h2)/2.0;
    filtTemp=smooth(filtTemp,avgT);
    filtHum =smooth(filtHum, avgH);
    filtUA  =smooth(filtUA,  calcUA(avgT,avgH));
  }

  // PID + atuadores
  computePID(filtTemp);  // PID sobre temperatura média filtrada (topo+copa)
  controleFanExaust();

  // Display
  drawTemp(3,  Y_TEMP, rawT1, lastTopT);
  drawHum (3,  Y_HUM,  rawH1, lastTopH);
  drawTemp(83, Y_TEMP, rawT2, lastBotT);
  drawHum (83, Y_HUM,  rawH2, lastBotH);
  drawAverages();
  drawDelta(rawT1, rawT2);
  drawCtrlLine();
  drawBar();
  drawUptime();
  drawWifiIcon();

  // WiFi / NTP
  if(WiFi.status()==WL_CONNECTED){
    if(!wifiOK){
      wifiOK=true;
      Serial.printf("WiFi OK — IP: %s\n", WiFi.localIP().toString().c_str());
      configTime(NTP_OFFSET,0,NTP_SERVER);
      server.begin();
      Serial.println("WebServer iniciado");
    }
    if(!ntpOK){
      struct tm t;
      if(getLocalTime(&t,100)){
        ntpOK=true; Serial.println("NTP OK");
        tUltimoLog=millis()-INTERVALO_LOG_MS;
      }
    }
  } else {
    if(wifiOK){ wifiOK=false; WiFi.reconnect(); }
  }

  // Web server
  server.handleClient();

  // Log Sheets — roda em task separada para nao bloquear o loop durante HTTPS
  if(millis()-tUltimoLog>=INTERVALO_LOG_MS){
    tUltimoLog = millis();  // atualiza antes para nao disparar de novo se demorar
    // Task temporaria com stack maior (8KB) — necessario para WiFiClientSecure
    xTaskCreate([](void*){
      enviarLog();
      vTaskDelete(NULL);
    }, "logTask", 8192, NULL, 1, NULL);
  }

  delay(80);
}