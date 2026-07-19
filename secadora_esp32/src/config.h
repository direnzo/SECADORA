#pragma once

// ===== WiFi =====
// Credenciais reais ficam em config_secrets.h (fora do git, ver .gitignore).
// Crie o arquivo com:
//   #define WIFI_SSID     "sua_rede"
//   #define WIFI_PASSWORD "sua_senha"
#include "config_secrets.h"

// ===== Pinos (confirmados na fiacao real, nao recuperados do firmware) =====
#define PIN_SDA        21   // SHT31 (I2C)
#define PIN_SCL        22   // SHT31 (I2C)
// Nomeados por FUNCAO (o dashboard rotula por funcao, nao por posicao fisica):
// confirmado com o usuario que o par de coolers do TOPO exausta (joga ar pra
// fora) e o par da COPA circula (faz o ar interno da camara circular).
#define PIN_FAN 33   // MOSFET -> 2x cooler DeepCool COPA (circulacao, badge "FAN")
#define PIN_EXT 27   // MOSFET -> 2x cooler DeepCool TOPO (exaustao, badge "EXT")
#define PIN_HEATER_SSR 32   // SSR 40A -> 2x aquecedor 400W (110V)

// ===== TFT display 1.8" SPI 128x160 V1.1 (chip ST7735), confirmado na fiacao
// real - reescrito do zero, nao recuperado do firmware original (ver docs) =====
#define PIN_TFT_CS   5    // SCK=GPIO18 e MOSI=GPIO23 sao VSPI hardware, fixos
#define PIN_TFT_DC   2    // A0
#define PIN_TFT_RST  4
#define DISPLAY_UPDATE_MS 1000

// ===== Enderecos I2C dos sensores (confirmados via boot log original) =====
#define SHT31_ADDR_TOPO 0x45
#define SHT31_ADDR_COPA 0x44

// ===== Google Sheets (recuperado do firmware original) =====
#define SHEETS_WEBHOOK_URL "https://script.google.com/macros/s/AKfycbxeqNioAF2gh3w7e_A9GlTeXXlPp1Wgl_3yPYjmTDH2mwYSjKOKhkk7K52pFW_ZJEM0NA/exec"
#define SHEETS_SEND_INTERVAL_MS 60000UL   // intervalo entre envios (nao recuperado, ajustar se necessario)

// ===== NTP =====
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC -3*3600   // Brasil (ajustar se necessario)
#define DST_OFFSET_SEC 0

// ===== Faixa de temperatura alvo (confirmado no JS do dashboard original) =====
#define ALVO_MIN 30
#define ALVO_MAX 75
#define ALVO_DEFAULT 48

// ===== Controle (NAO recuperado do firmware original - valores de partida,
// ajustar apos testar na pratica) =====
#define CONTROL_LOOP_MS      1000    // periodo do laco de controle
#define HEATER_WINDOW_MS      4000   // janela de controle proporcional do SSR (time-proportioning)
#define HEATER_DEADBAND_C     0.5f   // histerese em torno do alvo
#define FAN_DUTY_AUTO          180   // duty (0-255) do cooler de circulacao (COPA) quando aquecendo
#define EXT_DUTY_AUTO          140   // duty (0-255) do cooler de exaustao (TOPO) em modo automatico
#define SENSOR_READ_INTERVAL_MS 2000
