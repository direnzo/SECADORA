#include <Arduino.h>

// Pinos candidatos: evita UART0 (1,3), SPI da flash (6-11), strapping (0,2,12,15),
// input-only (34-39) e I2C ja reservado para os SHT31 (21,22).
const int candidatos[] = {4, 5, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33};
const int numCandidatos = sizeof(candidatos) / sizeof(candidatos[0]);

void listar() {
  Serial.println();
  Serial.println("=== TESTE DE PINOS - SECADORA ===");
  Serial.print("Pinos candidatos: ");
  for (int i = 0; i < numCandidatos; i++) {
    Serial.print(candidatos[i]);
    if (i < numCandidatos - 1) Serial.print(", ");
  }
  Serial.println();
  Serial.println("Digite um numero de pino e ENTER para pulsar HIGH por 3s.");
  Serial.println("Digite 'a' para testar todos em sequencia (3s cada, 1s de intervalo).");
  Serial.println("==================================");
}

void pulsar(int pino) {
  Serial.printf(">>> Pino %d: LIGADO (observe agora) <<<\n", pino);
  digitalWrite(pino, HIGH);
  delay(3000);
  digitalWrite(pino, LOW);
  Serial.printf(">>> Pino %d: desligado.\n", pino);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  for (int i = 0; i < numCandidatos; i++) {
    pinMode(candidatos[i], OUTPUT);
    digitalWrite(candidatos[i], LOW);
  }
  listar();
}

void loop() {
  if (Serial.available()) {
    String linha = Serial.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) return;

    if (linha == "a" || linha == "A") {
      for (int i = 0; i < numCandidatos; i++) {
        pulsar(candidatos[i]);
        delay(1000);
      }
      Serial.println("Sequencia completa.");
      listar();
      return;
    }

    int pino = linha.toInt();
    bool valido = false;
    for (int i = 0; i < numCandidatos; i++) {
      if (candidatos[i] == pino) { valido = true; break; }
    }
    if (valido) {
      pulsar(pino);
    } else {
      Serial.println("Pino invalido. Use um dos pinos listados.");
      listar();
    }
  }
}
