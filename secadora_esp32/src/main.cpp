#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
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
// Display TFT ST7735 128x160 - reescrito do zero (nao recuperado do
// firmware original, ver docs/README.md). Redesenha a tela inteira a
// cada DISPLAY_UPDATE_MS para evitar sobra de digitos de valores antigos.
// ---------------------------------------------------------------------
#define COR_FUNDO_PAINEL 0x2104   // cinza-azulado escuro (header/rodape)
#define COR_APAGADO      0x39E7   // cinza escuro (indicador "desligado")
#define COR_LABEL        0x7BEF   // cinza claro (rotulos pequenos)

// Bolinha de status (ligado=verde, desligado=cinza) + rotulo ao lado.
void desenharStatusDot(int x, int y, bool ligado, const char *label) {
  tft.fillCircle(x, y, 4, ligado ? ST77XX_GREEN : COR_APAGADO);
  tft.setTextColor(ligado ? ST77XX_WHITE : COR_LABEL);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y - 4);
  tft.print(label);
}

void atualizarDisplay() {
  // Reinicializa o controlador periodicamente - autorrecuperacao caso o TFT
  // trave/apague por ruido eletrico ou glitch de sinal SPI (sintoma observado
  // na fiacao real: tela mostra dados e depois fica em branco).
  static uint8_t ciclosDesdeReinit = 0;
  if (ciclosDesdeReinit++ >= 5) {
    ciclosDesdeReinit = 0;
    tft.initR(INITR_BLACKTAB);
    tft.setSPISpeed(4000000);
    tft.setRotation(0);
  }

  float dT = t1 - t2;
  unsigned long up = (millis() - bootMillis) / 1000UL;
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  tft.fillScreen(ST77XX_BLACK);

  // ----- Cabecalho (titulo muda de cor em modo manual) -----
  tft.fillRect(0, 0, 128, 14, COR_FUNDO_PAINEL);
  tft.setTextSize(1);
  tft.setTextColor(modoManual ? ST77XX_YELLOW : ST77XX_WHITE);
  tft.setCursor(4, 4);
  tft.print(modoManual ? "MANUAL" : "SECADORA");
  tft.fillCircle(120, 7, 3, wifiOk ? ST77XX_GREEN : ST77XX_RED);

  // ----- TOPO (numero grande) -----
  tft.setTextColor(COR_LABEL);
  tft.setCursor(4, 18);
  tft.print("TOPO");
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 28);
  tft.printf("%2.0f", t1);
  tft.setTextSize(2);
  tft.print("C");
  tft.setTextSize(1);
  tft.setTextColor(COR_LABEL);
  tft.setCursor(90, 40);
  tft.printf("%2.0f%%", h1);

  // ----- COPA (numero grande) -----
  tft.setTextColor(COR_LABEL);
  tft.setCursor(4, 60);
  tft.print("COPA");
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 70);
  tft.printf("%2.0f", t2);
  tft.setTextSize(2);
  tft.print("C");
  tft.setTextSize(1);
  tft.setTextColor(COR_LABEL);
  tft.setCursor(90, 82);
  tft.printf("%2.0f%%", h2);

  tft.drawFastHLine(4, 104, 120, COR_FUNDO_PAINEL);

  // ----- Alvo / delta -----
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 110);
  tft.printf("ALVO %dC", alvo);
  tft.setTextSize(1);
  tft.setTextColor(COR_LABEL);
  tft.setCursor(4, 128);
  tft.printf("dT %4.1fC", dT);

  // ----- Barra de potencia do aquecedor -----
  int larguraBarra = 120;
  int fillBarra = (int)((long)duty * larguraBarra / 255);
  tft.drawRect(4, 138, larguraBarra, 8, COR_LABEL);
  tft.fillRect(5, 139, larguraBarra - 2, 6, COR_APAGADO);
  if (fillBarra > 2) {
    tft.fillRect(5, 139, fillBarra - 2, 6, heaterOn ? ST77XX_RED : ST77XX_GREEN);
  }

  // ----- Status (aquecedor / fan / exaustor / modo) -----
  desenharStatusDot(6, 152, heaterOn, "AQ");
  desenharStatusDot(46, 152, fanOn, "FAN");
  desenharStatusDot(90, 152, exaustOn, "EXA");
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
  tft.setRotation(0);   // retrato (tela montada em pe) - trocar p/ 2 se a imagem
                        // ficar de cabeca para baixo
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
}
