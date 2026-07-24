#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <time.h>

#include "config.h"
#include "dashboard_html.h"

// =====================================================================
// SECADORA - reescrita a partir de engenharia reversa do firmware original
// (ver docs/README.md no repositorio para detalhes do que foi recuperado
// byte-a-byte vs. reconstruido).
//
// O QUE E FIEL AO ORIGINAL: dashboard HTML/CSS/JS (byte-a-byte), rotas
// HTTP (/, /data, /set, /manual), contrato JSON, sensores SHT31 (0x44/0x45,
// GPIO21/22), pinos de saida (GPIO27/33/32, confirmados pelo usuario),
// URL do webhook do Google Sheets, parametros da query enviada a ele,
// NTP (pool.ntp.org), faixa do alvo (30-75C).
//
// O QUE FOI RECONSTRUIDO (nao recuperavel do binario compilado): a logica
// exata de controle do aquecedor/ventoinhas (histerese, curva de duty,
// tempos de exaustao). Abaixo esta uma implementacao razoavel de controle
// proporcional simples com SSR em time-proportioning. AJUSTE os parametros
// em config.h e a funcao controlar() conforme o comportamento real desejado.
// =====================================================================

WebServer server(80);
Adafruit_SHT31 sht31Topo = Adafruit_SHT31();
Adafruit_SHT31 sht31Copa = Adafruit_SHT31();
Adafruit_ST7735 tft = Adafruit_ST7735(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

#define LEDC_CH_FAN 0
#define LEDC_CH_EXT 1

// ----- Estado global (espelha os campos do JSON /data do dashboard) -----
float t1 = NAN, h1 = NAN;   // TOPO
float t2 = NAN, h2 = NAN;   // COPA
float uaVal = 0;            // umidade absoluta media (g/m3)
int alvo = ALVO_DEFAULT;
uint8_t duty = 0;           // duty do aquecedor (0-255)
uint8_t fDuty = 0;          // duty fan topo (0-255)
uint8_t eDuty = 0;          // duty fan copa (0-255)
bool heaterOn = false;
bool fanOn = false;
bool exaustOn = false;
bool modoManual = false;

unsigned long bootMillis = 0;
unsigned long lastSensorRead = 0;
unsigned long lastControlLoop = 0;
unsigned long lastSheetsSend = 0;
unsigned long lastDisplayUpdate = 0;
bool sheetsTaskRunning = false;
bool wifiConectado = false;  // rastreia queda de WiFi pra reconectar sozinho

// Media suavizada (exponencial) dos sensores - usada no controle (PI) em vez
// da leitura crua, pra reduzir chacoalho de duty por ruido de sensor. Valores
// individuais (t1/h1/t2/h2) continuam crus, exibidos como estao.
float filtTemp = NAN, filtHum = NAN, filtUA = 0;

// Trava de seguranca: se um sensor de temperatura ficar sem leitura valida
// por SENSOR_TIMEOUT_MS, sensorFalha vira true e controlar() forca o
// aquecedor desligado (ver config.h). Sem isso, um sensor travado faz o
// controle continuar usando o ultimo valor bom pra sempre, silenciosamente.
unsigned long lastValidT1Millis = 0, lastValidT2Millis = 0;
unsigned long lastSensorRecoveryTry = 0;
bool sensorFalha = false;

// ---------------------------------------------------------------------
// Umidade absoluta (g/m3) - formula padrao (Magnus)
// ---------------------------------------------------------------------
float calcAbsHumidity(float tempC, float rh) {
  if (isnan(tempC) || isnan(rh)) return 0;
  float es = 6.112f * expf((17.62f * tempC) / (243.12f + tempC));
  return 216.7f * ((rh / 100.0f) * es) / (273.15f + tempC);
}

// Media movel exponencial - suaviza ruido de leitura entre ciclos.
float smooth(float prev, float next) {
  if (isnan(prev)) return next; // primeira leitura, sem historico ainda
  return prev + (next - prev) * SENSOR_SMOOTH_FACTOR;
}

// ---------------------------------------------------------------------
// Leitura dos sensores
// ---------------------------------------------------------------------
void lerSensores() {
  float novoT1 = sht31Topo.readTemperature();
  float novoH1 = sht31Topo.readHumidity();
  float novoT2 = sht31Copa.readTemperature();
  float novoH2 = sht31Copa.readHumidity();

  unsigned long agora = millis();
  if (!isnan(novoT1)) { t1 = novoT1; lastValidT1Millis = agora; }
  if (!isnan(novoH1)) h1 = novoH1;
  if (!isnan(novoT2)) { t2 = novoT2; lastValidT2Millis = agora; }
  if (!isnan(novoH2)) h2 = novoH2;

  bool falhaAntes = sensorFalha;
  sensorFalha = (agora - lastValidT1Millis > SENSOR_TIMEOUT_MS) ||
                (agora - lastValidT2Millis > SENSOR_TIMEOUT_MS);

  if (sensorFalha) {
    if (!falhaAntes) {
      Serial.println("[SHT31] FALHA - sem leitura valida ha muito tempo. Aquecedor bloqueado por seguranca.");
    }
    // Autorrecuperacao: tenta reinicializar o barramento I2C periodicamente,
    // sem precisar de reboot manual.
    if (agora - lastSensorRecoveryTry >= SENSOR_RECOVERY_INTERVAL_MS) {
      lastSensorRecoveryTry = agora;
      Serial.println("[SHT31] Tentando reinicializar I2C...");
      Wire.begin(PIN_SDA, PIN_SCL);
      sht31Topo.begin(SHT31_ADDR_TOPO);
      sht31Copa.begin(SHT31_ADDR_COPA);
    }
  } else if (falhaAntes) {
    Serial.println("[SHT31] Sensores voltaram a responder normalmente.");
  }

  float ua1 = calcAbsHumidity(t1, h1);
  float ua2 = calcAbsHumidity(t2, h2);
  uaVal = (ua1 + ua2) / 2.0f;

  // Medias suavizadas - usadas no controle (PI) e na secao "media" do
  // display, pra reduzir chacoalho de duty por ruido de leitura.
  filtTemp = smooth(filtTemp, (t1 + t2) / 2.0f);
  filtHum  = smooth(filtHum,  (h1 + h2) / 2.0f);
  filtUA   = smooth(filtUA,   uaVal);
}

// ---------------------------------------------------------------------
// Controle (RECONSTRUIDO - nao e o algoritmo original, ver comentario acima)
// Aquecedor: PI sobre o erro (alvo - tMed) - proporcional + integral (este
// ultimo pra fechar o erro residual observado na pratica com so-proporcional,
// ver comentario de HEATER_KI em config.h), com time-proportioning no SSR
// (janela HEATER_WINDOW_MS).
// Fans: ligam junto com o aquecedor para circular/exaurir o ar.
// ---------------------------------------------------------------------
void controlar() {
  if (modoManual) return; // em modo manual, duty/estado sao definidos via /manual e nao mexidos aqui

  static float integralErro = 0;

  // Trava de seguranca: sensor de temperatura sem leitura valida ha muito
  // tempo (ver lerSensores()/config.h) - desliga tudo, nao confia no PI
  // enquanto nao soubermos a temperatura real.
  if (sensorFalha) {
    duty = 0; fDuty = 0; eDuty = 0;
    heaterOn = fanOn = exaustOn = false;
    integralErro = 0;
    ledcWrite(LEDC_CH_FAN, 0);
    ledcWrite(LEDC_CH_EXT, 0);
    return;
  }

  float tMed = filtTemp; // media suavizada, nao a crua - reduz ruido no duty
  float erro = alvo - tMed;

  if (erro <= HEATER_DEADBAND_C) {
    duty = 0;
    integralErro = 0; // reset ao atingir o alvo - evita windup enquanto ocioso
  } else {
    // proporcional: erro de 0 a 10C mapeado para duty 0-255
    float dP = (erro / 10.0f) * 255.0f;

    integralErro += erro * (CONTROL_LOOP_MS / 1000.0f);
    float integralMax = HEATER_INTEGRAL_MAX / HEATER_KI;
    if (integralErro > integralMax) integralErro = integralMax;
    float dI = integralErro * HEATER_KI;

    float d = dP + dI;
    if (d > 255) d = 255;
    if (d < 0) d = 0;
    duty = (uint8_t)d;
  }
  heaterOn = duty > 0;

  fDuty = heaterOn ? FAN_DUTY_AUTO : 0;
  eDuty = heaterOn ? EXT_DUTY_AUTO : 0;
  fanOn = fDuty > 0;
  exaustOn = eDuty > 0;

  ledcWrite(LEDC_CH_FAN, fDuty);
  ledcWrite(LEDC_CH_EXT, eDuty);
}

// SSR via time-proportioning (chamado com frequencia alta no loop())
void aplicarHeaterSSR() {
  unsigned long fase = millis() % HEATER_WINDOW_MS;
  unsigned long onTime = (unsigned long)duty * HEATER_WINDOW_MS / 255UL;
  digitalWrite(PIN_HEATER_SSR, fase < onTime ? HIGH : LOW);
}

// ---------------------------------------------------------------------
// JSON de /data - contrato identico ao original (nomes de campo)
// ---------------------------------------------------------------------
String montarJson() {
  float tMed = filtTemp; // suavizada - log menos ruidoso
  float hMed = filtHum;
  float dT = t1 - t2;
  unsigned long up = (millis() - bootMillis) / 1000UL;

  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"wf\":%s,\"ip\":\"%s\",\"t1\":%.2f,\"h1\":%.2f,\"t2\":%.2f,\"h2\":%.2f,"
    "\"ua\":%.2f,\"alvo\":%d,\"duty\":%d,\"f_duty\":%d,\"e_duty\":%d,"
    "\"h_on\":%s,\"f_on\":%s,\"e_on\":%s,\"m_man\":%s,\"sensor_falha\":%s,\"up\":%lu}",
    WiFi.status() == WL_CONNECTED ? "true" : "false",
    WiFi.localIP().toString().c_str(),
    t1, h1, t2, h2,
    uaVal, alvo, duty, fDuty, eDuty,
    heaterOn ? "true" : "false",
    fanOn ? "true" : "false",
    exaustOn ? "true" : "false",
    modoManual ? "true" : "false",
    sensorFalha ? "true" : "false",
    up
  );
  return String(buf);
}

// ---------------------------------------------------------------------
// Rotas HTTP (identicas ao original)
// ---------------------------------------------------------------------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
  server.send(200, "application/json", montarJson());
}

void handleSet() {
  if (server.hasArg("alvo")) {
    int novoAlvo = server.arg("alvo").toInt();
    if (novoAlvo < ALVO_MIN) novoAlvo = ALVO_MIN;
    if (novoAlvo > ALVO_MAX) novoAlvo = ALVO_MAX;
    alvo = novoAlvo;
  }
  server.send(200, "application/json", montarJson());
}

void handleManual() {
  if (server.hasArg("toggle")) {
    modoManual = !modoManual;
    if (modoManual) {
      // entra em modo manual com tudo desligado por seguranca
      duty = 0; fDuty = 0; eDuty = 0;
      heaterOn = fanOn = exaustOn = false;
      ledcWrite(LEDC_CH_FAN, 0);
      ledcWrite(LEDC_CH_EXT, 0);
    }
  }
  server.send(200, "application/json", montarJson());
}

// ---------------------------------------------------------------------
// Envio para Google Sheets (assincrono via task, como no original -
// "Task anterior ainda rodando. Pulando" foi visto nas strings do firmware)
// ---------------------------------------------------------------------
void sheetsTask(void *param) {
  sheetsTaskRunning = true;

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%d/%m/%Y %H:%M:%S", &timeinfo);

  float tMed = (t1 + t2) / 2.0f;
  float hMed = (h1 + h2) / 2.0f;
  float dT = t1 - t2;

  String url = String(SHEETS_WEBHOOK_URL) +
    "?timestamp=" + timestamp +
    "&t_topo=" + String(t1, 1) +
    "&h_topo=" + String(h1, 1) +
    "&t_copa=" + String(t2, 1) +
    "&h_copa=" + String(h2, 1) +
    "&t_media=" + String(tMed, 1) +
    "&h_media=" + String(hMed, 1) +
    "&ua=" + String(uaVal, 1) +
    "&delta=" + String(dT, 1) +
    "&heater=" + (heaterOn ? "1" : "0") +
    "&duty=" + String(duty) +
    "&fan=" + String(fDuty) +
    "&exaust=" + String(eDuty) +
    "&t_alvo=" + String(alvo);
  url.replace(" ", "%20"); // timestamp tem espaco (ex "22/07/2026 23:10:00"),
                           // sem isso o Apps Script recebe URL malformada (HTTP 400)

  // WiFiClientSecure + retry: HTTP -1 e um erro comum e intermitente em
  // requisicoes HTTPS no ESP32 core v3 (visto na pratica). Recria o client
  // a cada tentativa pra evitar estado corrompido de uma tentativa anterior.
  int httpCode = -1;
  for (int tentativa = 1; tentativa <= 3 && httpCode < 0; tentativa++) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);

    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Connection", "close");
    http.setTimeout(20000);

    Serial.printf("[Sheets] Enviando (tentativa %d)...\n", tentativa);
    httpCode = http.GET();
    if (httpCode == 200 || httpCode == 302) {
      Serial.printf("[Sheets] OK - HTTP %d\n", httpCode);
    } else if (httpCode > 0) {
      Serial.printf("[Sheets] Resposta inesperada - HTTP %d\n", httpCode);
    } else {
      Serial.printf("[Sheets] Falhou - %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();

    if (httpCode < 0 && tentativa < 3) delay(2000);
  }

  sheetsTaskRunning = false;
  vTaskDelete(nullptr);
}

void enviarParaSheets() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (sheetsTaskRunning) {
    Serial.println("[Sheets] Task anterior ainda rodando. Pulando.");
    return;
  }
  xTaskCreate(sheetsTask, "SheetsLog", 8192, nullptr, 1, nullptr);
}

// ---------------------------------------------------------------------
// Display TFT ST7735 128x160, paisagem (160x128) - reescrito do zero (nao
// recuperado do firmware original, ver docs/README.md). Redesenho PARCIAL:
// so limpa/redesenha a regiao de um campo quando o valor muda alem de um
// threshold pequeno (padrao visto no .ino original recuperado depois - ver
// docs/README.md), em vez de fillScreen() toda hora. Evita flicker e reduz
// trafego no barramento SPI por ciclo.
// ---------------------------------------------------------------------
#define COR_FUNDO_PAINEL 0x2104   // cinza-azulado escuro (header/rodape)
#define COR_APAGADO      0x39E7   // cinza escuro (indicador "desligado")
#define COR_LABEL        0x7BEF   // cinza claro (rotulos pequenos)

void atualizarDisplay() {
  // Reinicializacao periodica de autorrecuperacao - manter como rede de
  // seguranca de baixa frequencia mesmo com a fiacao ja corrigida (era a
  // cada 5 ciclos/~5s, o que causava o proprio pisca-pisca branco que
  // parecia um defeito; ~5min ainda protege contra um glitch raro sem
  // ficar visivel).
  static uint16_t ciclosDesdeReinit = 0;
  static bool forcarRedesenho = true; // true no boot e logo apos cada reinit
  if (ciclosDesdeReinit++ >= 300) {
    ciclosDesdeReinit = 0;
    tft.initR(INITR_BLACKTAB);
    tft.setSPISpeed(4000000);
    tft.setRotation(1); // paisagem (160x128) - montagem fisica e deitada
    forcarRedesenho = true; // reinit apaga a tela - forca redesenhar tudo
  }

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  float dT = t1 - t2;
  unsigned long up = (millis() - bootMillis) / 1000UL;
  int dutyPct = duty * 100 / 255;
  int fanPct = fDuty * 100 / 255;
  int extPct = eDuty * 100 / 255;

  static float lastT1 = NAN, lastH1 = NAN, lastT2 = NAN, lastH2 = NAN;
  static int lastAlvo = -999;
  static float lastDT = -999;
  static int lastDuty = -1;
  static bool lastWifiOk = false, lastModoManual = false, lastSensorFalha = false;

  if (forcarRedesenho) {
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 160, 14, COR_FUNDO_PAINEL);
    tft.drawFastHLine(4, 43, 152, COR_FUNDO_PAINEL);
    tft.drawFastHLine(4, 91, 152, COR_FUNDO_PAINEL);
  }

  // ----- Cabecalho (titulo muda de cor em modo manual, alerta se sensor falhar) -----
  if (forcarRedesenho || modoManual != lastModoManual || sensorFalha != lastSensorFalha) {
    tft.fillRect(4, 2, 100, 10, sensorFalha ? ST77XX_RED : COR_FUNDO_PAINEL);
    tft.setTextSize(1);
    tft.setTextColor(sensorFalha ? ST77XX_WHITE : (modoManual ? ST77XX_YELLOW : ST77XX_WHITE));
    tft.setCursor(4, 4);
    tft.print(sensorFalha ? "FALHA SENSOR" : (modoManual ? "MANUAL" : "SECADORA"));
    lastModoManual = modoManual;
    lastSensorFalha = sensorFalha;
  }
  if (forcarRedesenho || wifiOk != lastWifiOk) {
    tft.fillCircle(150, 7, 3, wifiOk ? ST77XX_GREEN : ST77XX_RED);
    lastWifiOk = wifiOk;
  }

  // ----- TOPO (esquerda) / COPA (direita), lado a lado -----
  if (forcarRedesenho || isnan(lastT1) || fabsf(t1 - lastT1) > 0.09f || fabsf(h1 - lastH1) > 0.4f) {
    tft.fillRect(4, 16, 74, 25, ST77XX_BLACK);
    tft.setTextColor(COR_LABEL);
    tft.setTextSize(1);
    tft.setCursor(4, 16);
    tft.print("TOPO");
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(4, 26);
    tft.printf("%2.0fC", t1);
    tft.setTextSize(1);
    tft.setTextColor(COR_LABEL);
    tft.setCursor(50, 33);
    tft.printf("%2.0f%%", h1);
    lastT1 = t1; lastH1 = h1;
  }
  if (forcarRedesenho || isnan(lastT2) || fabsf(t2 - lastT2) > 0.09f || fabsf(h2 - lastH2) > 0.4f) {
    tft.fillRect(84, 16, 72, 25, ST77XX_BLACK);
    tft.setTextColor(COR_LABEL);
    tft.setTextSize(1);
    tft.setCursor(84, 16);
    tft.print("COPA");
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(84, 26);
    tft.printf("%2.0fC", t2);
    tft.setTextSize(1);
    tft.setTextColor(COR_LABEL);
    tft.setCursor(130, 33);
    tft.printf("%2.0f%%", h2);
    lastT2 = t2; lastH2 = h2;
  }

  // ----- ALVO / dT -----
  if (forcarRedesenho || alvo != lastAlvo || fabsf(dT - lastDT) > 0.09f) {
    tft.fillRect(4, 46, 152, 9, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(4, 46);
    tft.printf("ALVO %dC", alvo);
    tft.setTextColor(COR_LABEL);
    tft.setCursor(70, 46);
    if (dT >= 0) tft.print("+");
    tft.printf("dT %.1fC", dT);
    lastAlvo = alvo; lastDT = dT;
  }

  // ----- AQ / FAN / EXA - duty% (texto colorido = ligado/desligado) -----
  tft.fillRect(4, 58, 152, 9, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(heaterOn ? ST77XX_RED : COR_APAGADO);
  tft.setCursor(4, 58);
  tft.printf("AQ %3d%%", dutyPct);
  tft.setTextColor(fanOn ? ST77XX_GREEN : COR_APAGADO);
  tft.setCursor(60, 58);
  tft.printf("FAN %3d%%", fanPct);
  tft.setTextColor(exaustOn ? ST77XX_CYAN : COR_APAGADO);
  tft.setCursor(116, 58);
  tft.printf("EXA %3d%%", extPct);

  // ----- Barra de potencia do aquecedor -----
  if (forcarRedesenho || duty != lastDuty) {
    int larguraBarra = 152;
    int fillBarra = (int)((long)duty * larguraBarra / 255);
    tft.fillRect(4, 70, larguraBarra, 10, COR_APAGADO);
    if (fillBarra > 2) {
      tft.fillRect(4, 70, fillBarra, 10, heaterOn ? ST77XX_RED : ST77XX_GREEN);
    }
    lastDuty = duty;
  }

  // ----- Rodape: modo + uptime (redesenha todo ciclo, custo baixo) -----
  tft.fillRect(4, 94, 152, 9, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(modoManual ? ST77XX_YELLOW : ST77XX_GREEN);
  tft.setCursor(4, 94);
  tft.print(modoManual ? "MANUAL" : "AUTO");
  tft.setTextColor(COR_LABEL);
  tft.setCursor(60, 94);
  tft.printf("up %02lu:%02lu:%02lu", up / 3600UL, (up % 3600UL) / 60UL, up % 60UL);

  forcarRedesenho = false;
}

// ---------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== INICIANDO SISTEMA SECADORA ===");

  // Saidas
  pinMode(PIN_HEATER_SSR, OUTPUT);
  digitalWrite(PIN_HEATER_SSR, LOW);
  ledcSetup(LEDC_CH_FAN, 25000, 8);
  ledcSetup(LEDC_CH_EXT, 25000, 8);
  ledcAttachPin(PIN_FAN, LEDC_CH_FAN);
  ledcAttachPin(PIN_EXT, LEDC_CH_EXT);
  ledcWrite(LEDC_CH_FAN, 0);
  ledcWrite(LEDC_CH_EXT, 0);

  // I2C / sensores
  Wire.begin(PIN_SDA, PIN_SCL);
  bool okTopo = sht31Topo.begin(SHT31_ADDR_TOPO);
  bool okCopa = sht31Copa.begin(SHT31_ADDR_COPA);
  Serial.printf("[SHT31] TOPO (0x%02X): %s\n", SHT31_ADDR_TOPO, okTopo ? "OK" : "FALHA");
  Serial.printf("[SHT31] COPA (0x%02X): %s\n", SHT31_ADDR_COPA, okCopa ? "OK" : "FALHA");

  // Display TFT
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(4000000);   // clock SPI baixo (default e bem mais alto) -
                               // reduz erro de sinal em fiacao breadboard/jumper
  tft.setRotation(1);   // paisagem (160x128) - montagem fisica e deitada -
                        // trocar p/ 3 se a imagem ficar de cabeca para baixo
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[TFT] Inicializado");

  // WiFi
  Serial.println("Conectando WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long inicioWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicioWifi < 20000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK - IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    Serial.println("NTP OK");
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS OK - acessivel em http://%s.local\n", MDNS_HOSTNAME);
    } else {
      Serial.println("mDNS FALHOU ao iniciar");
    }
    wifiConectado = true;
  } else {
    Serial.println("\nWiFi FALHOU - continuando offline (dashboard local indisponivel ate reconectar)");
  }

  // Web server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", handleSet);
  server.on("/manual", handleManual);
  server.begin();
  Serial.println("WebServer iniciado");

  bootMillis = millis();
  lerSensores();
}

// ---------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------
void loop() {
  server.handleClient();
  aplicarHeaterSSR();

  unsigned long agora = millis();

  if (agora - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = agora;
    lerSensores();
  }

  if (agora - lastControlLoop >= CONTROL_LOOP_MS) {
    lastControlLoop = agora;
    controlar();
  }

  if (agora - lastSheetsSend >= SHEETS_SEND_INTERVAL_MS) {
    lastSheetsSend = agora;
    enviarParaSheets();
  }

  if (agora - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
    lastDisplayUpdate = agora;
    atualizarDisplay();
  }

  // Reconexao automatica - so tentamos conectar uma vez no setup(), sem isso
  // uma queda de WiFi depois do boot deixava o dashboard offline pra sempre.
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConectado) {
      wifiConectado = false;
      Serial.println("[WiFi] Conexao perdida, tentando reconectar...");
      WiFi.reconnect();
    }
  } else {
    wifiConectado = true;
  }
}
