# Projeto SECADORA (secadora de filamento) — coleta a partir do ESP32

Este documento reúne tudo que foi possível recuperar diretamente da placa ESP32
(porta COM6) já que **não havia código-fonte local** — o firmware só existia
gravado na placa. O processo foi: dump completo da flash via `esptool`,
identificação da tabela de partições, e extração de strings legíveis dos
binários (bootloader/app não vêm com o `.ino` original, mas texto embutido,
nomes de rotas HTTP, HTML/JS do dashboard e chaves salvas na NVS permitiram
reconstruir o comportamento do firmware com boa fidelidade).

## Hardware

- **Chip**: ESP32-D0WD-V3 (revisão v3.1), dual core 240MHz, Wi-Fi + BT
- **MAC**: `38:18:2b:8a:ee:b4`
- **Flash**: 4MB (Winbond/GD, manufacturer id 0x5e, device 0x4016)
- **Bridge USB-serial**: Silicon Labs CP210x (aparece como COM6 no Windows)
- **Sensores**: 2x SHT31 (temperatura + umidade) via I2C
  - `0x44` → **SHT31 COPA**
  - `0x45` → **SHT31 TOPO**
- Controle de **aquecedor** (PWM/duty), **ventoinha interna (FAN)** e
  **exaustor (EXT)** — todos por PWM (duty 0-255)

## Toolchain original

Encontrado no binário um caminho de build residual:
`C:\Users\Gustavo Raposo\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.8\libraries\WebServer\src\Parsing.cpp`

→ Firmware compilado com **Arduino ESP32 core v3.3.8**, aparentemente em outra
máquina/por outra pessoa ("Gustavo Raposo"). Vale usar a mesma versão do core
ao recompilar para minimizar diferenças de comportamento.

## Lógica de controle (deduzida do dashboard embutido)

O firmware sobe um **web server** (`WebServer.h`) com uma página HTML/JS
embutida (dashboard "SECADORA") e os seguintes endpoints:

| Rota | Método | Função |
|---|---|---|
| `/data` | GET | Retorna JSON com o estado atual (polling do dashboard) |
| `/set?alvo=<N>` | GET | Define a temperatura alvo (30–75 °C, step configurável 1 ou 5) |
| `/manual?toggle=1` | GET | Alterna entre modo AUTO e MANUAL |

### Campos do JSON de `/data` (nomes deduzidos do JS do dashboard)

| Campo | Significado |
|---|---|
| `wf` | WiFi conectado (bool) |
| `ip` | IP local |
| `t1`, `t2` | Temperatura TOPO / COPA (°C) |
| `h1`, `h2` | Umidade relativa TOPO / COPA (%UR) |
| `ua` | Umidade absoluta calculada (g/m³) — faixa usada no gauge: 12–25 |
| `alvo` | Temperatura alvo (°C), limitada entre 30 e 75 |
| `duty` | Duty do aquecedor (0-255) → % exibido |
| `h_on` | Aquecedor ligado (bool) |
| `f_duty`, `f_on` | Duty / estado da ventoinha (FAN) |
| `e_duty`, `e_on` | Duty / estado do exaustor (EXT) |
| `m_man` | Modo manual ativo (bool) |
| `up` | Uptime em segundos |

O dashboard também calcula `dT = t1 - t2` (delta entre os dois sensores) e
exibe um badge "dT --" quando não há leitura.

## Integração externa

- **NTP**: `pool.ntp.org`
- **Logging em nuvem**: webhook do Google Apps Script (provavelmente grava
  linhas numa Google Sheet a cada leitura):
  ```
  https://script.google.com/macros/s/AKfycbxeqNioAF2gh3w7e_A9GlTeXXlPp1Wgl_3yPYjmTDH2mwYSjKOKhkk7K52pFW_ZJEM0NA/exec
  ```
  ⚠️ Trate esse link como sensível — quem tiver a URL pode enviar dados
  para a sua planilha. Não é uma credencial de acesso à planilha em si, mas
  evite deixá-la em repositório público sem necessidade.

## WiFi

As credenciais de WiFi ficam salvas pela biblioteca `WiFi`/`Preferences` na
partição **NVS** (não estão hardcoded no `.ino`). Isso é o comportamento
padrão do ESP32 — ele guarda a última rede conectada automaticamente.
As credenciais antigas foram recuperadas e estão em
`../wifi_credentials_OLD.txt` (fora do controle de versão) só para
referência histórica — **não servem mais**, já que a rede foi trocada.

## Estrutura de arquivos deste backup

```
SECADORA/
├── docs/
│   ├── README.md          (este arquivo)
│   └── boot_log.txt        captura do boot serial original
├── esp32_backup/
│   ├── flash_backup_4MB.bin        dump bruto completo da flash (4MB)
│   ├── partition_table.json        tabela de partições parseada
│   ├── chunks/                     blocos de 256KB usados no dump (redundante, pode apagar)
│   └── partitions/
│       ├── nvs_nvs.bin (+ _strings.txt)         config/WiFi
│       ├── otadata_ota.bin                       ponteiro OTA ativo
│       ├── app0_ota_0.bin (+ _strings.txt)       firmware ativo (1.25MB)
│       ├── app1_ota_1.bin (+ _strings.txt)       slot OTA alternativo
│       ├── spiffs_spiffs.bin                     VAZIA (0xFF) - não usada
│       └── coredump_coredump.bin (+ _strings.txt) dump de crash (se houver)
└── wifi_credentials_OLD.txt (não versionar)
```

## Partições (offset / tamanho)

| Nome | Tipo | Offset | Tamanho |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB |
| otadata | data/ota | 0xe000 | 8 KB |
| app0 | app/ota_0 | 0x10000 | 1.25 MB |
| app1 | app/ota_1 | 0x150000 | 1.25 MB |
| spiffs | data/spiffs | 0x290000 | 1.375 MB (vazia) |
| coredump | data/coredump | 0x3f0000 | 64 KB |

## O que NÃO foi recuperado

O `.ino` original **não pode ser reconstruído literalmente** a partir do
binário compilado — o firmware compilado não contém o código-fonte C++,
apenas texto embutido (strings, HTML, nomes de símbolos não removidos pelo
linker). O que este backup garante:

- Um **backup de recuperação** (`flash_backup_4MB.bin`) que pode ser
  regravado no ESP32 a qualquer momento com
  `esptool write-flash 0x0 flash_backup_4MB.bin` — restaura o dispositivo
  exatamente como estava, incluindo WiFi salvo (rede antiga).
- Documentação funcional suficiente (rotas HTTP, campos JSON, sensores,
  faixas de operação, integrações externas) para **reescrever um `.ino`
  equivalente do zero**, ajustando o que for necessário (nova rede WiFi,
  melhorias, etc.).

## Parte 2 — Firmware reescrito (`secadora_esp32/`)

Depois da coleta, foi escrito um firmware novo em `secadora_esp32/` (projeto
PlatformIO), com o objetivo de manter o comportamento o mais fiel possível ao
original, trocando apenas a rede WiFi.

### O que é fiel ao original (byte-a-byte ou confirmado)

- **Dashboard HTML/CSS/JS** (`src/dashboard_html.h`): extraído byte-a-byte
  direto do binário do firmware original (região entre `<!DOCTYPE html>` e
  `</html>` no `app0_ota_0.bin`), não reescrito à mão — visual e
  comportamento do JS idênticos ao original.
- **Rotas HTTP e contrato JSON**: `/`, `/data`, `/set?alvo=`,
  `/manual?toggle=1`, com os mesmos nomes de campo (`t1`, `h1`, `f_duty`,
  `e_on`, `m_man`, etc.).
- **Webhook do Google Sheets**: mesma URL e mesmos parâmetros de query
  (`timestamp`, `t_topo`, `h_topo`, `t_copa`, `h_copa`, `t_media`, `h_media`,
  `ua`, `delta`, `heater`, `duty`, `fan`, `exaust`, `t_alvo`), recuperados
  também byte-a-byte do binário.
- **Sensores**: SHT31 em `0x44` (COPA) e `0x45` (TOPO), I2C em GPIO21
  (SDA) / GPIO22 (SCL) — confirmado tanto pelo log de boot original quanto
  pelas strings do firmware.
- **NTP**: `pool.ntp.org`.
- **Faixa do alvo**: 30–75 °C, passo 1 ou 5 °C (do JS original).

### Pinagem de saída (aquecedor/ventoinhas)

Esses valores **não estavam em nenhum texto recuperável** do binário (são
constantes numéricas compiladas, não strings) — foram informados pelo
usuário a partir da documentação/fiação real do hardware:

| Pino | Função |
|---|---|
| GPIO27 | MOSFET → 2x cooler DeepCool **TOPO** (mapeado para o badge "FAN" do dashboard) |
| GPIO33 | MOSFET → 2x cooler DeepCool **COPA** (mapeado para o badge "EXT" do dashboard) |
| GPIO32 | SSR 40A → 2x aquecedor 400W / 110V (controle por time-proportioning) |

⚠️ O mapeamento FAN=topo / EXT=copa foi uma escolha razoável (a fiação real
é por localização — topo/copa — enquanto o dashboard rotula por função —
fan/exaustor). Confirme na prática e inverta em `config.h` se necessário.

Uma tentativa inicial de identificar os pinos por teste físico (gravando um
sketch que pulsava candidatos um a um) foi abandonada por ser lenta demais de
acompanhar — o usuário tinha a documentação da fiação e passou os pinos
diretamente. O firmware de teste ficou em `pin_test/` (pode ser apagado).

### TFT display

O binário original não tinha nenhuma string de biblioteca de display
(TFT_eSPI/ILI9341/ST7735/Adafruit_GFX) na varredura — mas o usuário confirmou
que o display **funcionava fisicamente** no hardware antigo, fiado direto no
ESP32 (3V3/GPIO2/GPIO4/GPIO5/GPIO18/GND/GPIO23). Conclusão: o código original
provavelmente desenhava na tela via um driver enxuto/manual sem strings de
debug (por isso invisível numa varredura de texto), então **não dava pra
recuperar o código exato** — foi reescrito do zero.

Modelo real do display, confirmado pelo usuário: **"1.8 TFT SPI 128x160
V1.1"**, chip **ST7735**, sem touchscreen (só 5 sinais wireados: CS/DC/RESET/
SCK/MOSI — touch precisaria de mais pinos tipo T_CLK/T_CS/T_DIN/T_DO/T_IRQ).

Pinagem confirmada pelo usuário:

| Sinal | Pino ESP32 |
|---|---|
| CS | GPIO5 |
| DC (A0) | GPIO2 |
| RESET | GPIO4 |
| SCK | GPIO18 (VSPI hardware) |
| MOSI (SDA) | GPIO23 (VSPI hardware) |
| VCC / LED (backlight) | 3.3V direto |

Implementado em `src/main.cpp` (`atualizarDisplay()`) com a lib `Adafruit
ST7735 and ST7789 Library`. Layout em retrato (tela montada em pé,
`setRotation(0)`): números grandes de TOPO/COPA em destaque, alvo, barra de
potência do aquecedor, e indicadores de status (aquecedor/fan/exaustor) com
bolinha colorida. Atualiza a cada `DISPLAY_UPDATE_MS` (1s), redesenhando a
tela inteira.

⚠️ **Problema conhecido**: o display mostra os dados e depois apaga/pisca —
sintoma que muda conforme o aparelho é movido fisicamente, apontando para
solda fria/mau contato (provavelmente no GND ou no RESET), não bug de
software. Mitigação em código: reinicialização automática do controlador a
cada poucos ciclos (`ciclosDesdeReinit` em `atualizarDisplay()`) para
recuperar sozinho quando trava. Correção definitiva pendente: inspecionar/
resoldar as juntas do display, começando por GND e RESET.

### O que foi RECONSTRUÍDO (não é o algoritmo original)

A lógica exata de controle (histerese, curva de duty, tempos de exaustão)
**não é recuperável** de um binário compilado — não sobra nenhum texto
indicando os valores. Foi implementado um controle proporcional simples como
ponto de partida, em `src/main.cpp` (função `controlar()`) e
`src/config.h`:

- Erro = alvo − temperatura média; duty do aquecedor proporcional ao erro
  (mapeado de 0 a 10 °C de erro → 0 a 255 de duty), com histerese de
  `HEATER_DEADBAND_C`.
- Aquecedor (SSR) controlado por **time-proportioning**: dentro de uma
  janela de `HEATER_WINDOW_MS`, fica ligado por `duty/255` da janela.
- Ventoinhas ligam junto com o aquecedor, em duty fixo
  (`FAN_TOPO_DUTY_AUTO`, `FAN_COPA_DUTY_AUTO`).

**Ajustar esses parâmetros em `config.h` depois de observar o comportamento
real** — não há garantia de que batem com o comportamento original.

### Estrutura

```
secadora_esp32/
├── platformio.ini      ambiente esp32dev, lib Adafruit SHT31
└── src/
    ├── main.cpp          logica principal (wifi, sensores, controle, http, sheets)
    ├── config.h          credenciais wifi, pinos, urls, parametros de controle
    └── dashboard_html.h  dashboard recuperado byte-a-byte (nao mexer sem necessidade)
```

Compilar: `pio run` dentro de `secadora_esp32/`.
Gravar: `pio run --target upload` (porta configurada para COM6 no
`platformio.ini`).

### Status

Firmware **gravado e rodando no hardware** (COM6). Revisão de código feita
antes de gravar: encontrado e corrigido um bug em `handleManual()` que usava
o número do pino GPIO em vez do canal LEDC para desligar as ventoinhas (fix
em `main.cpp`), e as credenciais de WiFi foram movidas para
`secadora_esp32/src/config_secrets.h` (fora do git, ver `.gitignore`).

Credenciais reais de WiFi ficam em `config_secrets.h`, que **não é
versionado**. Para recriar esse arquivo:

```cpp
#pragma once
#define WIFI_SSID     "DIRENZO-2G"   // atencao: case-sensitive, com sufixo "-2G"
#define WIFI_PASSWORD "..."
```

⚠️ O SSID configurado inicialmente (`direnzo`, minusculo) nao existia na rede
— o nome real e `DIRENZO-2G` (maiusculo, com sufixo). Isso causava
`WL_NO_SSID_AVAIL` mesmo com a senha certa. Descoberto rodando
`WiFi.scanNetworks()` no boot.

Testado apos gravar: WiFi conecta, `/data` `/set` `/manual` e o dashboard
respondem corretamente, sensores SHT31 lendo nos dois pontos.

## Próximos passos

1. ~~Revisar `secadora_esp32/src/main.cpp` e `config.h`~~ — feito.
2. ~~Gravar no ESP32 e acompanhar o comportamento~~ — feito, ver Status acima.
3. Ajustar os parâmetros de controle (`HEATER_*`/`FAN_*_DUTY_AUTO` em
   `config.h`) conforme o resultado observado ao longo de um ciclo de secagem
   completo.
4. Confirmar se o mapeamento FAN=GPIO27(topo)/EXT=GPIO33(copa) está certo, ou
   inverter em `config.h`.
5. Se quiser manter a possibilidade de voltar ao firmware antigo: o backup
   completo (`esp32_backup/flash_backup_4MB.bin`) permite restaurar a
   qualquer momento com `esptool write-flash 0x0 flash_backup_4MB.bin`
   (a rede WiFi antiga salva nele não vai mais conectar, mas o resto volta
   como estava).
