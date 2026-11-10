#include <Arduino.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <ESP32Servo.h>

// ==============================================================================
// 1. CONFIGURAÇÕES DE LEITOR RFID
// ==============================================================================
// Define o pino Chip Select (SDA/SS) do leitor no GPIO 5
MFRC522DriverPinSimple ssPin(5);

// Inicializa o driver de comunicação SPI do barramento padrão do ESP32
// (Pinos padrão VSPI: SCK = GPIO 18, MISO = GPIO 19, MOSI = GPIO 23)
MFRC522DriverSPI driver{ssPin};

// Instancia o objeto principal de controle do leitor MFRC522
MFRC522 mfrc522{driver};

// Pino de Reset do leitor RFID (usado para acordar ou reiniciar o módulo)
const int pinRst = 21;

// ==============================================================================
// 2. CONFIGURAÇÕES DE MICRO SERVO MOTOR
// ==============================================================================
Servo travaServo;
const int pinServo = 27; // Pino que envia o sinal PWM para o servo motor

// Calibração dos ângulos da tranca mecânica
const int anguloFechado = 90; // Posição angular em graus com o portão fechado
const int anguloAberto = 0;   // Posição angular em graus com o portão aberto

// Variáveis de estado para controle suave de posição
int anguloAtual = anguloFechado;    // Posição física real onde o servo está agora
int anguloAlvo = anguloFechado;     // Posição para onde o servo precisa se deslocar
unsigned long ultimoPassoServo = 0; // Armazena o tempo do último movimento angular
//OBS: unsigned long garante apenas valores positivos (0 a 4.294.967.295), tipo nativo retornado pela função millis(). Evita valores negativos em cálculos de tempo e dobra o tempo de operação contínua antes do overflow.
unsigned long ultimoPassoServo = 0;

// Parâmetros de velocidade do servo motor:
// - passoGraus: quantidade de graus deslocados a cada iteração
// - velocidadePassoMs: intervalo em milissegundos entre cada passo
const int passoGraus = 2;
const unsigned long velocidadePassoMs = 20;

// ==============================================================================
// 3. CONFIGURAÇÕES DE SENSOR ULTRASSÔNICO
// ==============================================================================
const int pinTrig = 4;                 // Pino de saída: emite o pulso de som
const int pinEcho = 15;                // Pino de entrada: recebe o retorno (eco) da onda
const float distanciaMaximaCm = 30.0;  // Distância de corte para validar presença (em cm)

// Temporizadores para estabilização de leitura (debounce / filtro de ruído acústico)
unsigned long ultimaLeituraUltrassom = 0;
const unsigned long intervaloLeituraUsMs = 100; // Intervalo para não saturar o ar com ecos

unsigned long ultimoMomentoComPresenca = 0;
const unsigned long toleranciaAusenciaMs = 1500; // Aguarda 1.5s sem eco antes de desativar o leitor
bool presencaConfirmada = false;                 // Flag que indica se há alguém estável na frente

// ==============================================================================
// 4. REGRAS DE NEGÓCIO E CONTROLE DE ACESSO
// ==============================================================================
// UID hexadecimal do cartão cadastrado com permissão de acesso
const String UIDAutorizado = "D35B8DA9";

bool portaAberta = false;       // Flag para rastrear se o portão concluiu a abertura
unsigned long tempoAbertura = 0; // Marca temporal de quando o portão abriu por completo
const unsigned long tempoDestravadoMs = 3000; // Tempo de permanência aberto (3 segundos)

// ==============================================================================
// 5. FUNÇÕES AUXILIARES
// ==============================================================================

/**
 * Dispara um pulso ultrassônico e calcula a distância até o obstáculo mais próximo.
 * Retorna a distância em centímetros ou 999.0 em caso de timeout / sem obstáculo.
 */
float medirDistanciaCM() {
  // Garante linha limpa em nível baixo antes do disparo
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);

  // Envia pulso de disparo de 10 microssegundos exigido pelo HC-SR04
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);

  // pulseIn mede o tempo (em microssegundos) que o pino ECHO fica em nível ALTO.
  // Timeout de 25000us (~25ms) evita congelar o processador caso a onda não volte.
  long duracao = pulseIn(pinEcho, HIGH, 25000);

  // Se o tempo for zero, a leitura expirou sem receber eco (fora de alcance)
  if (duracao == 0) {
    return 999.0;
  }

  // Velocidade do som: ~0.0343 cm/us. Divide por 2 porque o trajeto é de ida e volta.
  return (duracao * 0.0343) / 2.0;
}

/**
 * Movimenta o servo motor grau a grau sem usar delay().
 * Mantém o processador livre para executar o restante do loop continuamente.
 */
void gerenciarMovimentoServo() {
  if (millis() - ultimoPassoServo >= velocidadePassoMs) {
    ultimoPassoServo = millis();

    // Movimento no sentido crescente de ângulo
    if (anguloAtual < anguloAlvo) {
      anguloAtual += passoGraus;
      if (anguloAtual > anguloAlvo) anguloAtual = anguloAlvo;
      travaServo.write(anguloAtual);
    } 
    // Movimento no sentido decrescente de ângulo
    else if (anguloAtual > anguloAlvo) {
      anguloAtual -= passoGraus;
      if (anguloAtual < anguloAlvo) anguloAtual = anguloAlvo;
      travaServo.write(anguloAtual);
    }

    // Detecta o momento exato em que o portão concluiu o curso de abertura (90 -> 0)
    if (anguloAtual == anguloAberto && anguloAlvo == anguloAberto && !portaAberta) {
      portaAberta = true;
      tempoAbertura = millis(); // Inicia o cronômetro dos 3 segundos com a porta já aberta
      Serial.println(F("[!] Portao aberto. Contando tempo limite..."));
    }

    // Detecta o momento exato em que o portão concluiu o curso de fechamento (0 -> 90)
    if (anguloAtual == anguloFechado && anguloAlvo == anguloFechado && portaAberta) {
      portaAberta = false;
      Serial.println(F("[i] Portao totalmente fechado."));
    }
  }
}

/**
 * Altera o alvo do servo para a posição aberta, iniciando a transição suave.
 */
void abrirTranca() {
  anguloAlvo = anguloAberto;
  Serial.println(F("[!] ACESSO PERMITIDO: Abrindo portao..."));
}

// ==============================================================================
// 6. SETUP (INICIALIZAÇÃO DO SISTEMA)
// ==============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial); // Aguarda abertura da porta serial (para placas com USB nativo)

  // Configuração dos pinos do sensor ultrassônico
  pinMode(pinTrig, OUTPUT);
  pinMode(pinEcho, INPUT);

  // Inicializa o pino RST do leitor RFID em nível ALTO para mantê-lo ativo
  pinMode(pinRst, OUTPUT);
  digitalWrite(pinRst, HIGH);
  delay(50);

  // Inicializa os registradores e a comunicação do CI MFRC522
  mfrc522.PCD_Init();

  // Configurações do servo motor no ESP32:
  // Frequência de 50Hz padrão de servos e larguras de pulso mínima (500us) e máxima (2400us)
  travaServo.setPeriodHertz(50);
  travaServo.attach(pinServo, 500, 2400);
  travaServo.write(anguloFechado); // Garante que a cancela inicie trancada

  Serial.println(F("\n========================================================"));
  Serial.println(F("ESP32: Ultrassom + RFID + Servo Motor"));
  Serial.println(F("========================================================"));

  // Imprime no terminal a versão do firmware do leitor para checar comunicação SPI
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  Serial.println(F("Sistema pronto. Aguardando presenca estavel...\n"));
}

// ==============================================================================
// 7. LOOP PRINCIPAL
// ==============================================================================
void loop() {
  // PASSO 1: Atualiza continuamente os passos angulares do servo motor
  gerenciarMovimentoServo();

  // PASSO 2: Temporizador do portão aberto (fecha sozinho após 3 segundos)
  if (portaAberta && anguloAlvo == anguloAberto && (millis() - tempoAbertura >= tempoDestravadoMs)) {
    anguloAlvo = anguloFechado; // Comanda o fechamento suave
    Serial.println(F("[i] Tempo esgotado (3s). Fechando portao..."));
  }

  // PASSO 3: Amostragem do sensor ultrassônico a cada 100 milissegundos
  if (millis() - ultimaLeituraUltrassom >= intervaloLeituraUsMs) {
    ultimaLeituraUltrassom = millis();

    float distancia = medirDistanciaCM();

    // Se detectou objeto dentro da distância de corte (ex: <= 30 cm)
    if (distancia > 0 && distancia <= distanciaMaximaCm) {
      ultimoMomentoComPresenca = millis(); // Renova a marcação de tempo de presença ativa

      if (!presencaConfirmada) {
        presencaConfirmada = true;
        Serial.print(F("[+] Presenca detectada a "));
        Serial.print(distancia, 1);
        Serial.println(F(" cm. Leitor RFID liberado!"));
      }
    } 
    // Se não detectou objeto, aguarda a janela de tolerância de 1.5s antes de desligar
    else if (presencaConfirmada && (millis() - ultimoMomentoComPresenca >= toleranciaAusenciaMs)) {
      presencaConfirmada = false;
      Serial.println(F("[i] Ninguem detectado por 1.5s. Leitor RFID desativado.\n"));
    }
  }

  // PASSO 4: FILTRO DE SEGURANÇA E OPERAÇÃO
  // Se não houver presença confirmada OU se o portão estiver se movendo, ignora o RFID
  if (!presencaConfirmada || anguloAtual != anguloAlvo) {
    return;
  }

  // PASSO 5: VERIFICAÇÃO DO CARTÃO RFID (só executa se houver presença física ativa)
  // Checa se há uma nova tag no campo eletromagnético da antena
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Tenta ler o número de série (UID) do cartão apresentado
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  // Converte o array de bytes do UID em uma String formatada em hexadecimal maiúsculo
  String uidString = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      uidString += "0"; // Adiciona zero à esquerda em bytes menores que 0x10
    }
    uidString += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();

  Serial.println(F("----------------------------------------"));
  Serial.print(F("[+] Tag Lida: "));
  Serial.println(uidString);

  // PASSO 6: VALIDAÇÃO DE ACESSO
  // Compara o UID lido com a lista de identificadores autorizados
  if (uidString == UIDAutorizado) {
    abrirTranca();
  } else {
    Serial.println(F("[X] ACESSO NEGADO: Tag nao autorizada!"));
  }
  Serial.println(F("----------------------------------------\n"));

  // Encerra a comunicação com o cartão para evitar leituras repetidas em sequência
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}