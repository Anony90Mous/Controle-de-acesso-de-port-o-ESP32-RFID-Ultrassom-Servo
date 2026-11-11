#include <Arduino.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <ESP32Servo.h>

// ==============================================================================
// 1. CONFIGURAÇÕES DO LEITOR RFID (MFRC522)
// ==============================================================================
// Instancia o pino Chip Select (SDA/SS) no GPIO 5
MFRC522DriverPinSimple ssPin(5);

// Driver de comunicação SPI do ESP32 (SCK = 18, MISO = 19, MOSI = 23)
MFRC522DriverSPI driver{ssPin};

// Instancia principal de controle do CI MFRC522
MFRC522 mfrc522{driver};

// Pino físico de Reset conectado ao leitor
const int pinRst = 21;

// ==============================================================================
// 2. CONFIGURAÇÕES DO MICRO SERVO MOTOR (SG90)
// ==============================================================================
Servo travaServo;
const int pinServo = 27; // Pino PWM do servo

// Limites angulares mecânicos da cancela
const int anguloFechado = 90; // Cancela abaixada (fechada)
const int anguloAberto = 0;   // Cancela levantada (aberta)

// Variáveis de controle de movimento suave
int anguloAtual = anguloFechado;
int anguloAlvo = anguloFechado;
unsigned long ultimoPassoServo = 0;

const int passoGraus = 2;              // Incremento angular por passo
const unsigned long velocidadePassoMs = 20; // Intervalo entre passos (ms)

// ==============================================================================
// 3. CONFIGURAÇÕES DO SENSOR ULTRASSÔNICO (HC-SR04)
// ==============================================================================
const int pinTrig = 4;                 // Disparo ultrassônico
const int pinEcho = 15;                // Retorno do eco acústico
const float distanciaMaximaCm = 30.0;  // Distância de corte para presença (cm)

unsigned long ultimaLeituraUltrassom = 0;
const unsigned long intervaloLeituraUsMs = 100; // Intervalo de amostragem acústica

unsigned long ultimoMomentoComPresenca = 0;
const unsigned long toleranciaAusenciaMs = 1500; // Janela de 1.5s antes de desativar
bool presencaConfirmada = false;

// ==============================================================================
// 4. REGRAS DE NEGÓCIO E CONTROLE DE ACESSO
// ==============================================================================
const String UIDAutorizado = "D35B8DA9"; // Cartão cadastrado

bool portaAberta = false;
unsigned long tempoAbertura = 0;
const unsigned long tempoDestravadoMs = 3000; // Tempo do portão aberto (3s)

// Trava pós-fechamento para evitar leituras repetidas imediatas
unsigned long tempoUltimoFechamento = 0;
const unsigned long cooldownLeituraMs = 3000; // 3 segundos de intervalo após fechar

// ==============================================================================
// 5. FUNÇÕES AUXILIARES
// ==============================================================================

/**
 * Executa um reset físico completo no leitor RFID via pino RST e reinicializa a antena.
 */
void reiniciarLeitorRFID() {
  digitalWrite(pinRst, LOW);
  delay(15);
  digitalWrite(pinRst, HIGH);
  delay(25);
  mfrc522.PCD_Init();
  mfrc522.PCD_AntennaOn();
}

/**
 * Mede a distância em centímetros até o obstáculo mais próximo.
 */
float medirDistanciaCM() {
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);

  long duracao = pulseIn(pinEcho, HIGH, 25000);
  if (duracao == 0) {
    return 999.0;
  }
  return (duracao * 0.0343) / 2.0;
}

/**
 * Gerencia a movimentação suave grau a grau e desacopla o PWM ao atingir o alvo.
 */
void gerenciarMovimentoServo() {
  if (millis() - ultimoPassoServo >= velocidadePassoMs) {
    ultimoPassoServo = millis();

    // Conecta o servo apenas se houver necessidade real de movimento
    if (anguloAtual != anguloAlvo && !travaServo.attached()) {
      travaServo.attach(pinServo, 600, 2400); // 600us protege o batente mecânico
    }

    // Avanço angular progressivo
    if (anguloAtual < anguloAlvo) {
      anguloAtual += passoGraus;
      if (anguloAtual > anguloAlvo) anguloAtual = anguloAlvo;
      travaServo.write(anguloAtual);
    } 
    else if (anguloAtual > anguloAlvo) {
      anguloAtual -= passoGraus;
      if (anguloAtual < anguloAlvo) anguloAtual = anguloAlvo;
      travaServo.write(anguloAtual);
    }

    // Conclusão da abertura
    if (anguloAtual == anguloAberto && anguloAlvo == anguloAberto && !portaAberta) {
      portaAberta = true;
      tempoAbertura = millis();
      travaServo.detach(); // Desliga sinal e ruído elétrico durante a espera aberto
      Serial.println(F("[!] Portao aberto. Contando tempo limite..."));
    }

    // Conclusão do fechamento
    if (anguloAtual == anguloFechado && anguloAlvo == anguloFechado && portaAberta) {
      portaAberta = false;
      tempoUltimoFechamento = millis(); // Inicia contagem de cooldown
      travaServo.detach();              // Desliga sinal e ruído mecânico
      reiniciarLeitorRFID();            // Restaura o leitor após o fim do movimento
      Serial.println(F("[i] Portao totalmente fechado. Pronto para novo acesso."));
    }
  }
}

/**
 * Dispara o comando de abertura reconectando o PWM se necessário.
 */
void abrirTranca() {
  if (!travaServo.attached()) {
    travaServo.attach(pinServo, 600, 2400);
  }
  anguloAlvo = anguloAberto;
  Serial.println(F("[!] ACESSO PERMITIDO: Abrindo portao..."));
}

// ==============================================================================
// 6. SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(pinTrig, OUTPUT);
  pinMode(pinEcho, INPUT);

  // Inicialização e reset de hardware do leitor RFID
  pinMode(pinRst, OUTPUT);
  reiniciarLeitorRFID();

  // Posiciona o servo inicialmente em 90° e relaxa o motor para evitar trancos
  travaServo.setPeriodHertz(50);
  travaServo.attach(pinServo, 600, 2400);
  travaServo.write(anguloFechado);
  delay(350);
  travaServo.detach(); // Desacopla o motor para cessar ruído na linha de alimentação

  Serial.println(F("\n========================================================"));
  Serial.println(F("ESP32: Controle de Acesso Estabilizado"));
  Serial.println(F("========================================================"));
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  Serial.println(F("Sistema pronto. Aguardando presenca...\n"));
}

// ==============================================================================
// 7. LOOP PRINCIPAL
// ==============================================================================
void loop() {
  // PASSO 1: Movimentação suave contínua
  gerenciarMovimentoServo();

  // PASSO 2: Temporizador de fechamento automático (3s após abrir)
  if (portaAberta && anguloAlvo == anguloAberto && (millis() - tempoAbertura >= tempoDestravadoMs)) {
    if (!travaServo.attached()) {
      travaServo.attach(pinServo, 600, 2400);
    }
    anguloAlvo = anguloFechado;
    Serial.println(F("[i] Tempo esgotado (3s). Fechando portao..."));
  }

  // PASSO 3: Monitoramento de presença ultrassônica (a cada 100ms)
  if (millis() - ultimaLeituraUltrassom >= intervaloLeituraUsMs) {
    ultimaLeituraUltrassom = millis();

    float distancia = medirDistanciaCM();

    if (distancia > 0 && distancia <= distanciaMaximaCm) {
      ultimoMomentoComPresenca = millis();

      if (!presencaConfirmada) {
        presencaConfirmada = true;
        reiniciarLeitorRFID(); // Garante o leitor acordado e limpo ao detectar aproximação
        Serial.print(F("[+] Presenca detectada a "));
        Serial.print(distancia, 1);
        Serial.println(F(" cm. Leitor RFID liberado!"));
      }
    } 
    else if (presencaConfirmada && (millis() - ultimoMomentoComPresenca >= toleranciaAusenciaMs)) {
      presencaConfirmada = false;
      Serial.println(F("[i] Ninguem detectado por 1.5s. Leitor RFID desativado.\n"));
    }
  }

  // PASSO 4: FILTROS DE SEGURANÇA E OPERAÇÃO
  // Bloqueia leituras se:
  // - Ausência de presença confirmada
  // - Servo ainda se movendo
  // - Portão já estiver aberto
  // - Intervalo pós-fechamento ativo (< 3 segundos)
  if (!presencaConfirmada || anguloAtual != anguloAlvo || portaAberta || (millis() - tempoUltimoFechamento < cooldownLeituraMs)) {
    return;
  }

  // PASSO 5: VERIFICAÇÃO DO CARTÃO RFID
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  // Conversão do UID para String hexadecimal
  String uidString = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      uidString += "0";
    }
    uidString += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();

  Serial.println(F("----------------------------------------"));
  Serial.print(F("[+] Tag Lida: "));
  Serial.println(uidString);

  // PASSO 6: VALIDAÇÃO DO ACESSO
  if (uidString == UIDAutorizado) {
    abrirTranca();
  } else {
    Serial.println(F("[X] ACESSO NEGADO: Tag nao cadastrada!"));
  }
  Serial.println(F("----------------------------------------\n"));

  // Adormece o chip da tag para evitar leituras repetidas em cascata
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}