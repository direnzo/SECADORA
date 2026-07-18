#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
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

#define LEDC_CH_FAN_TOPO 0
#define LEDC_CH_FAN_COPA 1

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
bool sheetsTaskRunning = false;

// ---------------------------------------------------------------------
// Umidade absoluta (g/m3) - formula padrao (Magnus)
// ---------------------------------------------------------------------
float calcAbsHumidity(float tempC, float rh) {
  if (isnan(tempC) || isnan(rh)) return 0;
  float es = 6.112f * expf((17.62f * tempC) / (243.12f + tempC));
  return 216.7f * ((rh / 100.0f) * es) / (273.15f + tempC);
}

// ---------------------------------------------------------------------
// Leitura dos sensores
// ---------------------------------------------------------------------
void lerSensores() {
  float novoT1 = sht31Topo.readTemperature();
  float novoH1 = sht31Topo.readHumidity();
  float novoT2 = sht31Copa.readTemperature();
  float novoH2 = sht31Copa.readHumidity();

  if (!isnan(novoT1)) t1 = novoT1;
  if (!isnan(novoH1)) h1 = novoH1;
  if (!isnan(novoT2)) t2 = novoT2;
  if (!isnan(novoH2)) h2 = novoH2;

  float ua1 = calcAbsHumidity(t1, h1);
  float ua2 = calcAbsHumidity(t2, h2);
  uaVal = (ua1 + ua2) / 2.0f;
}

// ---------------------------------------------------------------------
// Controle (RECONSTRUIDO - nao e o algoritmo original, ver comentario acima)
// Aquecedor: proporcional simples sobre o erro (alvo - tMed), com
// time-proportioning no SSR (janela HEATER_WINDOW_MS).
// Fans: ligam junto com o aquecedor para circular/exaurir o ar.
// ---------------------------------------------------------------------
void controlar() {
  if (modoManual) return; // em modo manual, duty/estado sao definidos via /manual e nao mexidos aqui

  float tMed = (t1 + t2) / 2.0f;
  float erro = alvo - tMed;

  if (erro <= HEATER_DEADBAND_C) {
    duty = 0;
  } else {
    // proporcional simples: erro de 0 a 10C mapeado para duty 0-255
    float d = (erro / 10.0f) * 255.0f;
    if (d > 255) d = 255;
    if (d < 0) d = 0;
    duty = (uint8_t)d;
  }
  heaterOn = duty > 0;

  fDuty = heaterOn ? FAN_TOPO_DUTY_AUTO : 0;
  eDuty = heaterOn ? FAN_COPA_DUTY_AUTO : 0;
  fanOn = fDuty > 0;
  exaustOn = eDuty > 0;

  ledcWrite(LEDC_CH_FAN_TOPO, fDuty);
  ledcWrite(LEDC_CH_FAN_COPA, eDuty);
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
  float tMed = (t1 + t2) / 2.0f;
  float hMed = (h1 + h2) / 2.0f;
  float dT = t1 - t2;
  unsigned long up = (millis() - bootMillis) / 1000UL;

  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"wf\":%s,\"ip\":\"%s\",\"t1\":%.2f,\"h1\":%.2f,\"t2\":%.2f,\"h2\":%.2f,"
    "\"ua\":%.2f,\"alvo\":%d,\"duty\":%d,\"f_duty\":%d,\"e_duty\":%d,"
    "\"h_on\":%s,\"f_on\":%s,\"e_on\":%s,\"m_man\":%s,\"up\":%lu}",
    WiFi.status() == WL_CONNECTED ? "true" : "false",
    WiFi.localIP().toString().c_str(),
    t1, h1, t2, h2,
    uaVal, alvo, duty, fDuty, eDuty,
    heaterOn ? "true" : "false",
    fanOn ? "true" : "false",
    exaustOn ? "true" : "false",
    modoManual ? "true" : "false",
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
      ledcWrite(PIN_FAN_TOPO, 0);
      ledcWrite(PIN_FAN_COPA, 0);
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

  HTTPClient http;
  http.begin(url);
  http.addHeader("Connection", "close");
  Serial.println("[Sheets] Enviando...");
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.printf("[Sheets] OK - HTTP %d\n", httpCode);
  } else {
    Serial.printf("[Sheets] Falhou - %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();

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
  ledcSetup(LEDC_CH_FAN_TOPO, 25000, 8);
  ledcSetup(LEDC_CH_FAN_COPA, 25000, 8);
  ledcAttachPin(PIN_FAN_TOPO, LEDC_CH_FAN_TOPO);
  ledcAttachPin(PIN_FAN_COPA, LEDC_CH_FAN_COPA);
  ledcWrite(LEDC_CH_FAN_TOPO, 0);
  ledcWrite(LEDC_CH_FAN_COPA, 0);

  // I2C / sensores
  Wire.begin(PIN_SDA, PIN_SCL);
  bool okTopo = sht31Topo.begin(SHT31_ADDR_TOPO);
  bool okCopa = sht31Copa.begin(SHT31_ADDR_COPA);
  Serial.printf("[SHT31] TOPO (0x%02X): %s\n", SHT31_ADDR_TOPO, okTopo ? "OK" : "FALHA");
  Serial.printf("[SHT31] COPA (0x%02X): %s\n", SHT31_ADDR_COPA, okCopa ? "OK" : "FALHA");

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
}
