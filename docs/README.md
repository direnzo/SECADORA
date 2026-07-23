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
| GPIO27 (`PIN_EXT`) | MOSFET → 2x cooler DeepCool **TOPO** — exaustão, badge "EXT" |
| GPIO33 (`PIN_FAN`) | MOSFET → 2x cooler DeepCool **COPA** — circulação, badge "FAN" |
| GPIO32 | SSR 40A → 2x aquecedor 400W / 110V (controle por time-proportioning) |

✅ Confirmado com o usuário: TOPO faz **exaustão** (joga ar pra fora) e COPA
faz **circulação** (mistura o ar interno) — o oposto do que se assumia
inicialmente. Corrigido em `config.h`/`main.cpp` (constantes renomeadas por
função, `PIN_FAN`/`PIN_EXT`, em vez de por posição física).

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
ST7735 and ST7789 Library`. Layout em **paisagem** (tela montada deitada,
`setRotation(1)` — confirmado pelo usuário depois de recuperar o `.ino`
original, que também usava paisagem; a suposição inicial de retrato estava
errada): TOPO/COPA lado a lado, ALVO/dT, potência do aquecedor/fan/exaustor
em % (texto colorido conforme ligado/desligado), barra de potência do
aquecedor, e rodapé com modo (AUTO/MANUAL) + uptime. Atualiza a cada
`DISPLAY_UPDATE_MS` (1s) com **redesenho parcial**: só limpa/redesenha a
região de um campo quando o valor muda além de um threshold pequeno, em vez
de `fillScreen()` toda hora (técnica vista no `.ino` original recuperado,
ver seção abaixo).

**Problema do pisca-pisca branco — resolvido**: o sintoma ("mostra os dados
e depois apaga") persistia mesmo depois do usuário trocar todos os cabos do
display, descartando solda fria/mau contato como causa. A causa real era do
nosso próprio código: uma reinicialização periódica do controlador
(`ciclosDesdeReinit` em `atualizarDisplay()`) rodando a cada ~5s como
"autorrecuperação" — esse reinit por si só pisca a tela em branco por um
instante (reset de hardware do controlador). Reduzido para ~5min (ainda serve
de rede de segurança de baixa frequência, mas não é mais perceptível).

### O que foi RECONSTRUÍDO (não é o algoritmo original)

A lógica exata de controle (histerese, curva de duty, tempos de exaustão)
**não é recuperável** de um binário compilado — não sobra nenhum texto
indicando os valores. Foi implementado um controle **PI** (proporcional +
integral) em `src/main.cpp` (função `controlar()`) e `src/config.h`:

- Erro = alvo − temperatura média **suavizada** (`filtTemp`, média móvel
  exponencial, `SENSOR_SMOOTH_FACTOR`) — reduz chacoalho de duty por ruído
  de leitura.
- Proporcional: erro de 0 a 10°C mapeado para duty 0-255, com histerese de
  `HEATER_DEADBAND_C`.
- Integral: acumula o erro ao longo do tempo (anti-windup em
  `HEATER_INTEGRAL_MAX`) pra fechar o erro residual permanente visto na
  prática com só-proporcional (~3.5°C abaixo do alvo, ele "empatava" onde o
  calor gerado equilibrava o calor perdido, sem fechar o gap).
- Aquecedor (SSR) controlado por **time-proportioning**: dentro de uma
  janela de `HEATER_WINDOW_MS`, fica ligado por `duty/255` da janela. Mantido
  assim mesmo depois de achar o `.ino` original (que usava PWM direto de
  1kHz) porque o SSR é um **SSR-40DA**, tipicamente zero-cross — PWM rápido
  não é seguro nesse tipo de SSR.
- Ventoinhas ligam junto com o aquecedor, em duty fixo (`FAN_DUTY_AUTO` pro
  cooler de circulação/COPA, `EXT_DUTY_AUTO` pro de exaustão/TOPO).

**Ainda não validado até o fim** — o aquecedor físico parou de responder
durante o teste (ver pendência urgente mais abaixo), então não dá pra
confirmar se o PI fecha o gap sem overshoot na prática real ainda.

### `.ino` original recuperado depois (`_SecadoraPWM3D.INO.ino`)

O usuário resgatou um `.ino` (v3.1, não finalizado, de outra rede/local —
`APT_07`) que mostra uma versão mais madura do firmware do que o que foi
reconstruído por engenharia reversa do dump de flash. Não é a mesma versão
que rodava na placa quando o dump foi feito (esse `.ino` só tem **1**
MOSFET de ventoinha — comentário do autor: `// PIN_EXAUST removido - apenas
1 MOSFET disponivel` — enquanto o binário dumpado já tinha 2 campos de
ventoinha no JSON), então é uma iteração anterior/paralela, não o "firmware
final" — mas revelou boas ideias que foram adotadas nesta sessão:

- **PID completo** (o autor também percebeu o erro residual do
  só-proporcional e adicionou integral — mesma conclusão que chegamos
  independente; seu `KP=25` é quase idêntico ao nosso ganho proporcional
  efetivo de `25.5`). Adotamos o P+I mas não o termo D (derivativo) ainda —
  ver seção de controle acima.
- **Suavização exponencial dos sensores** (`smooth()`, fator 0.15) —
  adotada (`filtTemp`/`filtHum`/`filtUA` em `main.cpp`).
- **Reconexão automática de WiFi** (`WiFi.reconnect()` quando cai) — adotada.
- **Retry com `WiFiClientSecure`** no log do Google Sheets, pra contornar um
  bug conhecido do ESP32 core v3 com HTTPS (comentário do autor: "HTTP -1 é
  comum") — adotado, e ajudou a expor (e corrigir) um bug nosso separado:
  o timestamp tem espaço e não estava sendo escapado na URL, causando
  HTTP 400 silencioso.
- **Layout do display em paisagem + redesenho parcial** — adotado (ver seção
  TFT acima).
- **IP fixo** — o `.ino` original usava, mas para a rede antiga (`APT_07`).
  Optamos por manter IP dinâmico (DHCP) e resolver por nome via **mDNS**
  (`http://secadora.local`) em vez de fixar o IP.
- **Fan por delta topo-copa** (uma só ventoinha, sempre ligada, velocidade
  proporcional à diferença de temperatura entre os sensores, em vez de
  liga/desliga junto com o aquecedor) — **não adotado** nesta rodada, fica
  como ideia pra reconsiderar já que agora temos 2 MOSFETs de verdade
  (diferente da época desse `.ino`).

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

Acesso via `http://secadora.local` (mDNS, IP continua dinâmico). Display em
paisagem, sem flicker, layout denso (TOPO/COPA/ALVO/dT/potências/modo/uptime).
Log do Google Sheets confirmado funcionando (HTTP 302). Aquecedor em **modo
manual** até a falha física ser resolvida — ver pendência urgente abaixo.

## Próximos passos

1. ~~Revisar `secadora_esp32/src/main.cpp` e `config.h`~~ — feito.
2. ~~Gravar no ESP32 e acompanhar o comportamento~~ — feito, ver Status acima.
3. ~~Confirmar mapeamento FAN/EXT~~ — feito. Usuário confirmou fisicamente que
   TOPO exausta e COPA circula (invertido do que se assumia) — corrigido em
   `config.h`/`main.cpp` (`PIN_FAN`/`PIN_EXT`, ver seção TFT/pinagem acima).
4. ~~Ajustar controle do aquecedor~~ — parcialmente feito. Adicionado termo
   integral (PI), suavização de sensores, e anti-windup mais conservador
   (ver seção de controle acima). **Ainda não validado até o fim** — ver
   pendência urgente abaixo.
5. ~~mDNS + reconexão de WiFi~~ — feito. `http://secadora.local` funciona,
   IP continua dinâmico (DHCP). Reconecta sozinho se a conexão cair.
6. ~~Retry no log do Google Sheets~~ — feito, e corrigido um bug real junto
   (timestamp sem escapar na URL causava HTTP 400 silencioso).
7. ~~Display: paisagem + redesenho parcial + layout mais denso~~ — feito.
   Pisca-pisca branco resolvido (causa era nossa própria reinicialização
   periódica, não solda — usuário já trocou todos os cabos e confirmou).

## 🔴 Pendência urgente (próxima sessão): aquecedor parece não estar aquecendo

Ainda não resolvida — não foi mexida nesta sessão (o foco foi WiFi/display/
controle). Durante o teste do PI, o duty ficou travado em 255 (100%) o tempo
todo e a temperatura **caiu** de ~44°C para ~22°C (ambiente) em vez de subir
ou estabilizar. Ou seja, o software mandou aquecer no máximo e na prática não
houve aquecimento nenhum. Sistema fica em **modo manual (tudo desligado)**
por segurança a cada boot desta sessão em diante, até isso ser resolvido.

Checar fisicamente antes de religar (circuito de 110V/40A — cuidado):

1. Disjuntor/fusível do circuito do aquecedor (pode ter desarmado)
2. LED indicador do SSR (deveria acender com o comando em 100%)
3. Conexão/continuidade do elemento resistivo de 400W
4. Alimentação de 110V chegando no SSR

Isso não é bug de firmware — o software está calculando e mandando o duty
certo (`aplicarHeaterSSR()` não foi alterado nessa sessão). É elétrico/físico.

Depois de resolver:

1. Validar se o PI fecha o gap até o alvo sem overshoot significativo, e
   reajustar `HEATER_KI`/`HEATER_INTEGRAL_MAX` se necessário.
2. Considerar adotar o termo derivativo (KD) do `.ino` original, se o P+I
   sozinho não amortecer bem o overshoot na prática.
3. Considerar a estratégia de fan por delta topo-copa do `.ino` original
   (ventoinha sempre ligada, velocidade proporcional à diferença de
   temperatura) em vez do liga/desliga atual — não adotada ainda, ver seção
   do `.ino` original acima.
4. Se quiser manter a possibilidade de voltar ao firmware antigo: o backup
   completo (`esp32_backup/flash_backup_4MB.bin`) permite restaurar a
   qualquer momento com `esptool write-flash 0x0 flash_backup_4MB.bin`
   (a rede WiFi antiga salva nele não vai mais conectar, mas o resto volta
   como estava).
