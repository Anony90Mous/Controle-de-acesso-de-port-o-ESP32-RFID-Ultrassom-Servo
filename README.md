# Sistema de Controle de Acesso e Garagem Automatizada com ESP32

Projeto acadêmico de automação e prototipagem IoT desenvolvido para a disciplina de **Internet das Coisas (IoT)** no curso de **Ciência da Computação** da **Universidade do Extremo Sul Catarinense (UNESC)** — Criciúma, SC.

O sistema integra o microcontrolador **ESP32**, leitor **RFID MFRC522**, sensor ultrassônico **HC-SR04** e micro servo motor **SG90** para compor uma maquete de portão de garagem inteligente dotada de trava de segurança por presença física.

---

## Equipe do Projeto

* **Artur Fabricio da Silva**
* **Jose Augusto Pereira da Rocha**
* **Leandro Aníbal Tramontim Córneo**
* **Luiz Arthur Rosso**
* **Luiz Othávio Francisconi Alano**
* **Victor Gabriel Paulo dos Santos**

**Professor Orientador:**  
Prof. Valter Blauth Junior

---

## 📌 Visão Geral e Lógica de Segurança

Diferente de sistemas convencionais onde o leitor RFID fica escaneando continuamente, este projeto implementa uma **lógica de dupla verificação ("Acesso Seguro")**:

1. **Filtro de Presença Física:** O sensor ultrassônico monitora a área frontal do portão. O leitor RFID permanece em estado ocioso (*idle*) e só é liberado para leitura quando um objeto/veículo é detectado a uma distância igual ou inferior a **30 cm**.
2. **Debounce Acústico:** O ultrassom utiliza amostragem periódica de **100 ms** e janela de tolerância de **1,5 s** para evitar oscilações e falsos positivos causados por reflexão acústica.
3. **Autenticação por Tag RFID:** Com a presença confirmada, a aproximação de uma tag autorizada comanda a abertura do portão. Tags não cadastradas são rejeitadas com alerta no console serial.
4. **Movimentação Suave e Não-Bloqueante:** O acionamento do servo motor SG90 é escalonado em passos discretos controlados pela função nativa `millis()`, sem uso de `delay()`. O portão abre suavemente até 0°, aguarda 3 segundos aberto e fecha automaticamente retornando a 90°.

---

## 🛠️ Componentes de Hardware

* **1x** ESP32 DOIT DevKit V1 (30 pinos)
* **1x** Módulo Leitor RFID RC522 (13.56 MHz) + Tag/Chaveiro de acesso
* **1x** Sensor de Distância Ultrassônico HC-SR04
* **1x** Micro Servo Motor SG90 (9g)
* **1x** Protoboard e cabos jumpers (Macho-Macho / Macho-Fêmea)
* Cabo Micro-USB para alimentação e gravação

---

## 🔌 Tabela de Pinagem e Ligações

> **Importante:** Todos os módulos compartilham o mesmo plano de terra (**GND**) conectado ao ESP32.

| Módulo | Pino do Componente | Pino do ESP32 | Nível Lógico / Tensão |
| :--- | :--- | :--- | :--- |
| **MFRC522 (RFID)** | 3.3V | **3V3** | 3.3V (Não ligar em 5V!) |
| | GND | **GND** | 0V |
| | RST | **GPIO 21** | 3.3V |
| | SDA (SS) | **GPIO 5** | 3.3V (SPI CS) |
| | MOSI | **GPIO 23** | 3.3V (SPI MOSI) |
| | MISO | **GPIO 19** | 3.3V (SPI MISO) |
| | SCK | **GPIO 18** | 3.3V (SPI CLK) |
| **HC-SR04 (Ultrassom)** | VCC | **3V3** | 3.3V (Operação direta sem resistores) |
| | GND | **GND** | 0V |
| | TRIG | **GPIO 4** | Saída digital do ESP32 |
| | ECHO | **GPIO 15** | Entrada digital do ESP32 |
| **Servo SG90** | VCC (Vermelho) | **VIN (ou 5V)** | 5.0V (Alimentado direto da USB) |
| | GND (Marrom) | **GND** | 0V |
| | Sinal (Laranja) | **GPIO 27** | Sinal PWM (50 Hz) |

---

## 💻 Configuração do Ambiente de Desenvolvimento

O projeto foi estruturado utilizando o ecossistema **PlatformIO IDE** no Visual Studio Code.

### Arquivo `platformio.ini`

```ini
[env:esp32doit-devkit-v1]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
monitor_speed = 115200
lib_deps = 
    computer991/Arduino_MFRC522v2@^2.0.1
    madhephaestus/ESP32Servo@^3.0.5
