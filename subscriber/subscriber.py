#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Processo Subscriber MQTT para Controle de Acesso
Escuta o tópico 'portao-tag' no broker local (Mosquitto)
Valida e persiste acessos no banco SQLite 'controle_acesso.db'
"""

import os
import sys
import time
import sqlite3
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("[ERRO] Biblioteca 'paho-mqtt' nao encontrada!", flush=True)
    print("Instale executando: pip install -r requirements.txt", flush=True)
    sys.exit(1)

# Configurações do Broker Local
BROKER_HOST = "localhost"
BROKER_PORT = 1883
TOPICO_TAGS = "portao-tag"

# Caminho absoluto para o banco SQLite (mantém na mesma pasta do script)
DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "controle_acesso.db")


def obter_hora_atual():
    return datetime.now().strftime("[%H:%M:%S]")


def inicializar_banco():
    """Cria as tabelas caso não existam e insere a tag de teste inicial."""
    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.cursor()

        # Tabela 1: tag (cadastro de usuários)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS tag (
                uid TEXT PRIMARY KEY,
                nome TEXT NOT NULL
            )
        """)

        # Tabela 2: log_acesso (histórico de eventos)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS log_acesso (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                data_hora TEXT NOT NULL,
                tag_uid TEXT NOT NULL,
                nome TEXT,
                status TEXT NOT NULL
            )
        """)

        # Cadastra o usuário inicial "João da Silva" caso a tag não exista
        tag_inicial = "D35B8DA9"
        nome_inicial = "João da Silva"
        cursor.execute("SELECT uid FROM tag WHERE UPPER(uid) = ?", (tag_inicial,))
        if not cursor.fetchone():
            cursor.execute("INSERT INTO tag (uid, nome) VALUES (?, ?)", (tag_inicial, nome_inicial))
            conn.commit()
            print(f"{obter_hora_atual()} [SQLITE] Tag inicial '{tag_inicial}' ('{nome_inicial}') cadastrada com sucesso!", flush=True)
        else:
            print(f"{obter_hora_atual()} [SQLITE] Banco carregado. Tag '{tag_inicial}' já está cadastrada.", flush=True)


def processar_tag(uid_bruto):
    """
    Verifica a tag no banco SQLite, registra o log e retorna se está autorizada.
    """
    uid = uid_bruto.strip().upper()
    data_hora = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.cursor()

        # Consulta se a tag existe na tabela
        cursor.execute("SELECT nome FROM tag WHERE UPPER(uid) = ?", (uid,))
        resultado = cursor.fetchone()

        if resultado:
            nome_usuario = resultado[0]
            # Registra no log como AUTORIZADO
            cursor.execute(
                "INSERT INTO log_acesso (data_hora, tag_uid, nome, status) VALUES (?, ?, ?, ?)",
                (data_hora, uid, nome_usuario, "AUTORIZADO")
            )
            conn.commit()
            return True, nome_usuario
        else:
            # Registra no log como NEGADO
            cursor.execute(
                "INSERT INTO log_acesso (data_hora, tag_uid, nome, status) VALUES (?, ?, ?, ?)",
                (data_hora, uid, "Desconhecido", "NEGADO")
            )
            conn.commit()
            return False, None


def on_connect(client, userdata, flags, rc, properties=None):
    """Callback disparado ao conectar com sucesso ao broker."""
    codigo_sucesso = 0 if isinstance(rc, int) else (rc.value == 0)
    if rc == 0 or codigo_sucesso:
        print(f"{obter_hora_atual()} [MQTT] Conectado ao broker Mosquitto ({BROKER_HOST}:{BROKER_PORT}) com sucesso!", flush=True)
        client.subscribe(TOPICO_TAGS)
        print(f"{obter_hora_atual()} [MQTT] Inscrito no topico: '{TOPICO_TAGS}'", flush=True)
        print(f"{obter_hora_atual()} [INFO] Aguardando leitura de tags pelo ESP32...", flush=True)
        print("-" * 65, flush=True)
    else:
        print(f"{obter_hora_atual()} [ERRO] Falha ao conectar ao broker. Codigo de retorno: {rc}", flush=True)


def on_message(client, userdata, msg):
    """Callback disparado automaticamente quando uma mensagem chega no tópico."""
    try:
        conteudo = msg.payload.decode("utf-8").strip()
    except Exception:
        conteudo = str(msg.payload)

    hora = obter_hora_atual()

    # Se for comando de abertura remoto gerado pelo próprio script ou administrador, ignora processamento de tag
    if conteudo.upper() in ["ABRIR", "OPEN"]:
        print(f"{hora} [COMANDO] Sinal de abertura remoto em trânsito no tópico: '{conteudo}'", flush=True)
        return

    # Mensagem com UID de tag lida
    print(f"{hora} NOVA TAG LIDA: {conteudo}", flush=True)

    # Valida no SQLite
    autorizado, nome_usuario = processar_tag(conteudo)

    if autorizado:
        print(f"{hora} [ACESSO] -> AUTORIZADO para '{nome_usuario}'! Publicando 'ABRIR'...", flush=True)
        client.publish(TOPICO_TAGS, "ABRIR")
    else:
        print(f"{hora} [ACESSO] -> NEGADO! Tag nao cadastrada no banco de dados.", flush=True)


def on_disconnect(client, userdata, disconnect_flags_or_rc, reason_code=None, properties=None):
    """Callback disparado em caso de desconexão inesperada."""
    print(f"\n{obter_hora_atual()} [AVISO] Conexao com o broker perdida. Tentando reconectar...", flush=True)


def main():
    print("=" * 65, flush=True)
    print("  SISTEMA DE CONTROLE DE ACESSO - SUBSCRIBER MQTT & SQLITE", flush=True)
    print("=" * 65, flush=True)
    print(f"[i] Broker Alvo: {BROKER_HOST}:{BROKER_PORT}", flush=True)
    print(f"[i] Topico:      {TOPICO_TAGS}", flush=True)
    print(f"[i] Banco Dados: {DB_PATH}", flush=True)
    print("=" * 65, flush=True)

    # 1. Inicializa o banco de dados e cria as tabelas
    inicializar_banco()

    # 2. Configura cliente MQTT com compatibilidade v1 e v2
    client = None
    try:
        # paho-mqtt >= 2.0.0
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="Python_Subscriber_Acesso")
    except AttributeError:
        # paho-mqtt < 2.0.0
        client = mqtt.Client(client_id="Python_Subscriber_Acesso")

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    try:
        print(f"{obter_hora_atual()} Conectando ao Mosquitto local...", flush=True)
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
        client.loop_forever()
    except KeyboardInterrupt:
        print(f"\n{obter_hora_atual()} Encerrando subscriber pelo usuario.", flush=True)
        client.disconnect()
        sys.exit(0)
    except ConnectionRefusedError:
        print(f"\n[ERRO] Nao foi possivel conectar ao broker em {BROKER_HOST}:{BROKER_PORT}.", flush=True)
        print("Verifique se o servico do Mosquitto esta em execucao: Start-Service mosquitto", flush=True)
        sys.exit(1)
    except Exception as e:
        print(f"\n[ERRO] Ocorreu uma excecao: {e}", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
