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

## Próximos passos sugeridos

1. Decidir se quer que eu **escreva um novo `.ino`** reproduzindo esse
   comportamento (dashboard, PID/histerese do aquecedor, endpoints, logging
   no Google Sheets) como ponto de partida do novo repositório.
2. Definir as novas credenciais WiFi (SSID/senha) a embutir ou configurar via
   portal cativo (ex: `WiFiManager`) para evitar hardcode.
3. Criar `.gitignore` cobrindo `wifi_credentials_OLD.txt`, `esp32_backup/`
   (binários grandes) e qualquer futuro arquivo de segredos.
