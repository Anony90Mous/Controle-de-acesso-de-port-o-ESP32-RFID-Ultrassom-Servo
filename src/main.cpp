#include <Arduino.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <ESP32Servo.h>

// ==============================================================================
// 1. CONFIGURAÇÕES DO LEITOR RFID (MFRC522)
// ==============================================================================
// Instancia o pino Chip Select (SDA/SS) no GPIO 5 do ESP32
MFRC522DriverPinSimple ssPin(5);

// Inicializa o driver de comunicação no barramento SPI padrão do ESP32 (VSPI)
// Pinos nativos do VSPI: SCK = GPIO 18, MISO = GPIO 19, MOSI = GPIO 23
MFRC522DriverSPI driver{ssPin};

// Instancia o objeto principal para gerenciar os comandos do CI MFRC522
MFRC522 mfrc522{driver};

// Pino físico de Reset conectado ao leitor (usado para reinicialização por hardware)
const int pinRst = 21;

// ==============================================================================
// 2. CONFIGURAÇÕES DO MICRO SERVO MOTOR (SG90)
// ==============================================================================
Servo travaServo;
const int pinServo = 27; // Pino que fornece o sinal de controle PWM ao servo

// Calibração dos limites angulares mecânicos da cancela/portão
const int anguloFechado = 90; // Ângulo com o portão fechado (cancela abaixada)
const int anguloAberto = 0;   // Ângulo com o portão aberto (cancela levantada)

// Variáveis de estado para controle suave de posição angular
int anguloAtual = anguloFechado; // Posição angular real em que o eixo está neste ciclo
int anguloAlvo = anguloFechado;  // Posição angular desejada para onde o eixo deve ir

// Marca temporal do último avanço angular do motor (em milissegundos)
// unsigned long: armazena valores positivos de 0 a 4.294.967.295, evitando números negativos
// e comportando com segurança os retornos acumulados da função millis()
unsigned long ultimoPassoServo = 0;

// Parâmetros de suavização de movimento:
// - passoGraus: variação em graus a cada incremento (evita trancos mecânicos)
// - velocidadePassoMs: intervalo de tempo em ms entre cada passo angular
const int passoGraus = 2;
const unsigned long velocidadePassoMs = 20;

// ==============================================================================
// 3. CONFIGURAÇÕES DO SENSOR ULTRASSÔNICO (HC-SR04)
// ==============================================================================
const int pinTrig = 4;                 // Pino de saída: emite a rajada de ondas ultrassônicas
const int pinEcho = 15;                // Pino de entrada: detecta o retorno (eco) da onda
const float distanciaMaximaCm = 30.0;  // Distância de corte para validar presença física (cm)

// Temporizadores para controle de amostragem acústica
unsigned long ultimaLeituraUltrassom = 0;
const unsigned long intervaloLeituraUsMs = 100; // Intervalo para evitar sobreposição de ecos

unsigned long ultimoMomentoComPresenca = 0;
const unsigned long toleranciaAusenciaMs = 1500; // Janela de 1.5s sem leitura antes de desarmar
bool presencaConfirmada = false;                 // Flag indicadora de objeto estável na área

// ==============================================================================
// 4. REGRAS DE NEGÓCIO E CONTROLE DE ACESSO
// ==============================================================================
// UID hexadecimal do cartão cadastrado no sistema com permissão de entrada
const String UIDAutorizado = "D35B8DA9";

bool portaAberta = false;                    // Flag que rastreia se a abertura foi completada
unsigned long tempoAbertura = 0;             // Marca temporal do momento em que atingiu 0°
const unsigned long tempoDestravadoMs = 3000; // Tempo de permanência do portão aberto (3s)

// ==============================================================================
// 5. FUNÇÕES AUXILIARES
// ==============================================================================

/**
 * Executa um reset físico completo no leitor RFID.
 * Aplica um pulso em nível BAIXO no pino RST para descarregar os registradores
 * internos do CI MFRC522 e reinicializa a antena transmissora de 13.56 MHz via SPI.
 */
void reiniciarLeitorRFID() {
  digitalWrite(pinRst, LOW);
  delay(10);
  digitalWrite(pinRst, HIGH);
  delay(20);
  mfrc522.PCD_Init();
  mfrc522.PCD_AntennaOn();
}

/**
 * Dispara um pulso sonoro e calcula o tempo de voo até o obstáculo.
 * Retorna a distância em centímetros ou 999.0 em caso de ausência de eco/timeout.
 */
float medirDistanciaCM() {
  // Garante linha limpa em nível baixo antes de emitir o pulso
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);

  // Pulso de disparo em nível ALTO de 10 microssegundos exigido pelo sensor
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);

  // pulseIn mede o tempo (us) que o pino ECHO permanece em nível ALTO
  // Timeout de 25000us (~25ms) evita bloquear a CPU caso o eco se perca
  long duracao = pulseIn(pinEcho, HIGH, 25000);

  // Se o tempo for zero, a leitura expirou sem retorno de onda
  if (duracao == 0) {
    return 999.0;
  }

  // Velocidade do som no ar: ~0.0343 cm/us
  // Divide-se por 2 porque a onda percorre o trajeto de ida e de volta
  return (duracao * 0.0343) / 2.0;
}

/**
 * Gerencia o deslocamento suave do servo motor sem utilizar a função delay().
 * Mantém o microcontrolador livre para executar os demais processos no loop().
 */
void gerenciarMovimentoServo() {
  if (millis() - ultimoPassoServo >= velocidadePassoMs) {
    ultimoPassoServo = millis();

    // Movimentação no sentido de fechamento (ângulo aumentando até 90°)
    if (anguloAtual < anguloAlvo) {
      anguloAtual += passoGraus;
      if (anguloAtual > anguloAlvo) anguloAtual = anguloAlvo;
      travaServo.write(anguloAtual);
    } 
    // Movimentação no sentido de abertura (ângulo diminuindo até 0°)
    else if (anguloAtual > anguloAlvo) {
      anguloAtual -= passoGraus;
      if (anguloAtual < anguloAlvo) anguloAtual = anguloAlvo;
      travaServo.write(anguloAtual);
    }

    // Identifica o término do curso de abertura
    if (anguloAtual == anguloAberto && anguloAlvo == anguloAberto && !portaAberta) {
      portaAberta = true;
      tempoAbertura = millis(); // Registra o início do temporizador com a porta aberta
      Serial.println(F("[!] Portao aberto. Contando tempo limite..."));
    }

    // Identifica o término do curso de fechamento
    if (anguloAtual == anguloFechado && anguloAlvo == anguloFechado && portaAberta) {
      portaAberta = false;
      reiniciarLeitorRFID(); // Força o reset físico do leitor para destravar a antena
      Serial.println(F("[i] Portao totalmente fechado. Leitor RFID reiniciado."));
    }
  }
}

/**
 * Modifica o ângulo alvo do servo para 0°, disparando a transição suave de abertura.
 */
void abrirTranca() {
  anguloAlvo = anguloAberto;
  Serial.println(F("[!] ACESSO PERMITIDO: Abrindo portao..."));
}

// ==============================================================================
// 6. SETUP (INICIALIZAÇÃO DO MICROCONTROLADOR)
// ==============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial); // Aguarda a inicialização do canal USB nativo

  // Configuração dos pinos do sensor ultrassônico
  pinMode(pinTrig, OUTPUT);
  pinMode(pinEcho, INPUT);

  // Inicializa o pino de reset do leitor e executa o ciclo de partida por hardware
  pinMode(pinRst, OUTPUT);
  reiniciarLeitorRFID();

  // Configurações do servo motor no ESP32:
  // Frequência padrão de 50Hz e larguras de pulso PWM entre 500us e 2400us
  travaServo.setPeriodHertz(50);
  travaServo.attach(pinServo, 500, 2400);
  travaServo.write(anguloFechado); // Garante que a cancela inicie em 90° (fechada)

  Serial.println(F("\n========================================================"));
  Serial.println(F("ESP32: Controle de Acesso - RFID + Ultrassom + Servo"));
  Serial.println(F("========================================================"));

  // Imprime no terminal a versão do firmware lida no MFRC522 (0x91 ou 0x92 = conexão SPI OK)
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  Serial.println(F("Sistema pronto. Aguardando presenca...\n"));
}

// ==============================================================================
// 7. LOOP PRINCIPAL
// ==============================================================================
void loop() {
  // PASSO 1: Atualiza continuamente os passos do servo motor de forma suave
  gerenciarMovimentoServo();

  // PASSO 2: Temporizador do portão aberto (comanda fechamento após 3 segundos)
  if (portaAberta && anguloAlvo == anguloAberto && (millis() - tempoAbertura >= tempoDestravadoMs)) {
    anguloAlvo = anguloFechado;
    Serial.println(F("[i] Tempo esgotado (3s). Fechando portao..."));
  }

  // PASSO 3: Amostragem do sensor ultrassônico a cada 100 ms
  if (millis() - ultimaLeituraUltrassom >= intervaloLeituraUsMs) {
    ultimaLeituraUltrassom = millis();

    float distancia = medirDistanciaCM();

    // Se detectou objeto dentro da distância de corte (<= 30 cm)
    if (distancia > 0 && distancia <= distanciaMaximaCm) {
      ultimoMomentoComPresenca = millis(); // Renova o tempo de presença contínua

      if (!presencaConfirmada) {
        presencaConfirmada = true;
        reiniciarLeitorRFID(); // Garante registradores limpos e antena ativa na aproximação
        Serial.print(F("[+] Presenca detectada a "));
        Serial.print(distancia, 1);
        Serial.println(F(" cm. Leitor RFID liberado!"));
      }
    } 
    // Se não detectou objeto, aguarda a tolerância de 1.5s antes de desativar
    else if (presencaConfirmada && (millis() - ultimoMomentoComPresenca >= toleranciaAusenciaMs)) {
      presencaConfirmada = false;
      Serial.println(F("[i] Ninguem detectado por 1.5s. Leitor RFID desativado.\n"));
    }
  }

  // PASSO 4: FILTRO DE OPERAÇÃO E SEGURANÇA
  // Se não há presença estável OU o servo está em movimento, o leitor não faz leituras
  if (!presencaConfirmada || anguloAtual != anguloAlvo) {
    return;
  }

  // PASSO 5: VERIFICAÇÃO DO CARTÃO RFID (só processa sob presença física confirmada)
  // Verifica se há uma nova tag no campo de rádio do leitor
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Tenta realizar a leitura do número de série (UID) da tag
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  // Converte o array de bytes do UID para uma String hexadecimal maiúscula
  String uidString = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      uidString += "0"; // Adiciona zero à esquerda para bytes menores que 0x10 (ex: 0A)
    }
    uidString += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();

  Serial.println(F("----------------------------------------"));
  Serial.print(F("[+] Tag Lida: "));
  Serial.println(uidString);

  // PASSO 6: VALIDAÇÃO DE ACESSO
  // Compara o UID lido com o UID cadastrado
  if (uidString == UIDAutorizado) {
    abrirTranca();
  } else {
    Serial.println(F("[X] ACESSO NEGADO: Tag nao cadastrada!"));
  }
  Serial.println(F("----------------------------------------\n"));

  // Envia comando para adormecer o chip do cartão e finaliza a criptografia da sessão
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}