import time
import json
import threading
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
import requests
import psycopg2
import psycopg2.extras
import paho.mqtt.client as mqtt
import certifi

# --- Config API/Banco ---
API_KEY = "e2872af56f808cbac9be0ad1fadca6f7"
CONFIG_BANCO = {
    "host": "postgresql.janks.dev.br",
    "port": 5432,
    "dbname": "projeto_b",
    "user": "iot",
    "password": "pepcon-garton",
}

URL_API = "https://v3.football.api-sports.io/fixtures?live=all"
HEADERS = {"x-apisports-key": API_KEY}

SLEEP_QUANDO_AO_VIVO = 300  # 5 min entre checagens quando tem jogo na janela

# --- Config MQTT ---
MQTT_HOST = "mqtt.janks.dev.br"
MQTT_PORT = 8883
MQTT_USER = "aula"
MQTT_PASS = "zowmad-tavQez"
MQTT_TOPIC = "partidas/atualizada"
MQTT_TOPIC2 = "partidas/inject"

# Evento usado para "acordar" o loop principal antes do tempo calculado
acordar = threading.Event()


def on_connect(client, userdata, flags, codigo):
    print("MQTT conectado, código:", codigo)
    client.subscribe(MQTT_TOPIC)
    client.subscribe(MQTT_TOPIC2)


def on_message(client, userdata, mensagem):
    try:
        dados = json.loads(mensagem.payload.decode("utf-8"))
        print(f"Notificação MQTT recebida: {dados}")
    except Exception:
        print("Notificação MQTT recebida (payload não-JSON)")
    acordar.set()


mqtt_client = mqtt.Client()
mqtt_client.tls_set(certifi.where())
mqtt_client.username_pw_set(username=MQTT_USER, password=MQTT_PASS)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_HOST, port=MQTT_PORT, keepalive=30)
mqtt_client.loop_start()


def conectar():
    return psycopg2.connect(**CONFIG_BANCO)


def notificar_mqtt(partida, gols_mandante, gols_visitante, status):
    """Publica no mesmo tópico que o Flask usa. """

    status_legivel = {
    "agendada": "Agendada",
    "em_andamento": "Em andamento",
    "finalizada": "Finalizada"
    }
    mensagem = json.dumps({
        "mandante_id": partida["mandante"],
        "visitante_id": partida["visitante"],
        "mandante_nome": partida["nome_mandante"],
        "visitante_nome": partida["nome_visitante"],
        "gols_mandante": gols_mandante,
        "gols_visitante": gols_visitante,
        "status": status_legivel.get(status, status),
        "data": partida["data"].astimezone(ZoneInfo("America/Sao_Paulo")).strftime("%d/%m/%Y às %H:%M"),
    })
    mqtt_client.publish(MQTT_TOPIC, payload=mensagem)


def buscar_partidas_pendentes():
    conexao = conectar()
    cursor = conexao.cursor(cursor_factory=psycopg2.extras.RealDictCursor)
    cursor.execute("""
        SELECT p.id, p.data, p.status, p.gols_mandante, p.gols_visitante,
               p.mandante, p.visitante,
               tm.nome AS nome_mandante, tm.nome_ingles AS nome_mandante_en,
               tv.nome AS nome_visitante, tv.nome_ingles AS nome_visitante_en
        FROM partidas p
        JOIN times tm ON tm.id_time = p.mandante
        JOIN times tv ON tv.id_time = p.visitante
        WHERE p.status != 'finalizada'
        ORDER BY p.data ASC
    """)
    partidas = cursor.fetchall()
    cursor.close()
    conexao.close()
    return partidas


def partida_esta_na_janela(partida, agora):
    return agora >= partida["data"]


def atualizar_partida(id_partida, gols_mandante, gols_visitante, status):
    """Grava placar e status juntos. Chamada tanto quando o placar muda quanto
    quando só o status precisa avançar."""
    conexao = conectar()
    cursor = conexao.cursor()
    cursor.execute("""
        UPDATE partidas
        SET gols_mandante = %s, gols_visitante = %s, status = %s
        WHERE id = %s
    """, (gols_mandante, gols_visitante, status, id_partida))
    conexao.commit()
    cursor.close()
    conexao.close()


def buscar_placar_final(data_partida, nome_mandante, nome_visitante, gm_fallback, gv_fallback):
    """Consulta a API pelo placar final do jogo no dia em que ocorreu.
    Retorna (gols_mandante, gols_visitante, encontrado). Se não encontrar, usa o fallback."""
    data_str = data_partida.strftime("%Y-%m-%d")
    url = f"https://v3.football.api-sports.io/fixtures?date={data_str}"
    try:
        resp = requests.get(url, headers=HEADERS, timeout=10)
        if resp.status_code != 200:
            print(f"[DEBUG] buscar_placar_final: status {resp.status_code}")
            return gm_fallback, gv_fallback, False
        for jogo in resp.json().get("response", []):
            home = jogo["teams"]["home"]["name"].lower()
            away = jogo["teams"]["away"]["name"].lower()
            if home == nome_mandante and away == nome_visitante:
                gm = jogo["goals"]["home"]
                gv = jogo["goals"]["away"]
                status_api = jogo["fixture"]["status"]["short"]
                finalizado = status_api in ("FT", "AET", "PEN")
                print(f"[DEBUG] buscar_placar_final: encontrou {gm} x {gv} (status API: {status_api})")
                return gm, gv, finalizado
    except Exception as e:
        print(f"[DEBUG] buscar_placar_final: erro {e}")
    return gm_fallback, gv_fallback, False


def checar_jogos_ao_vivo(partidas_na_janela):
    response = requests.get(URL_API, headers=HEADERS)

    if response.status_code != 200:
        print(f"Erro na API: {response.status_code}")
        return

    jogos_ao_vivo = response.json().get("response", [])
    print(f"[DEBUG] {len(jogos_ao_vivo)} jogo(s) ao vivo no mundo agora:")
    for j in jogos_ao_vivo:
        print(f"  - {j['teams']['home']['name']} x {j['teams']['away']['name']}")

    for partida in partidas_na_janela:
        nome_mandante = (partida["nome_mandante_en"] or "").lower()
        nome_visitante = (partida["nome_visitante_en"] or "").lower()

        encontrou = False
        for jogo in jogos_ao_vivo:
            home = jogo["teams"]["home"]["name"].lower()
            away = jogo["teams"]["away"]["name"].lower()

            if home == nome_mandante and away == nome_visitante:
                encontrou = True
                gm = jogo["goals"]["home"]
                gv = jogo["goals"]["away"]
                print(f"[DEBUG] Correspondência encontrada! Placar na API: {gm} x {gv} "
                      f"(banco tem: {partida['gols_mandante']} x {partida['gols_visitante']}, "
                      f"status atual: {partida['status']})")

                placar_mudou = gm != partida["gols_mandante"] or gv != partida["gols_visitante"]
                precisa_marcar_em_andamento = partida["status"] != "em_andamento"

                if placar_mudou:
                    atualizar_partida(partida["id"], gm, gv, "em_andamento")
                    notificar_mqtt(partida, gm, gv, "em_andamento")
                    print(f"Partida {partida['id']} atualizada (gol!): {gm} x {gv}")
                elif precisa_marcar_em_andamento:
                    atualizar_partida(partida["id"], gm, gv, "em_andamento")
                    notificar_mqtt(partida, gm, gv, "em_andamento")
                    print(f"Partida {partida['id']} marcada como em_andamento.")
                break

        if not encontrou:
            print(f"[DEBUG] Nenhuma correspondência para a partida {partida['id']} nesta checagem.")

            # Se a partida já estava em_andamento e sumiu da lista de jogos ao
            # vivo, assumimos que terminou -> busca o placar final na API antes
            # de marcar como finalizada.
            if partida["status"] in ("em_andamento", "agendada"):
                gm_final, gv_final, confirmado = buscar_placar_final(
                    partida["data"], nome_mandante, nome_visitante,
                    partida["gols_mandante"], partida["gols_visitante"]
                )
                # Só finaliza se a API confirmou que o jogo existiu e terminou (FT/AET/PEN)
                if confirmado:
                    atualizar_partida(partida["id"], gm_final, gv_final, "finalizada")
                    notificar_mqtt(partida, gm_final, gv_final, "finalizada")
                    print(f"Partida {partida['id']} marcada como finalizada "
                          f"com placar {gm_final} x {gv_final}.")
                else:
                    print(f"[DEBUG] Partida {partida['id']} não encontrada na API, nenhuma alteração feita.")


def calcular_proxima_pausa(partidas, agora):
    """Retorna a pausa em segundos, ou None se não há partida futura
    (nesse caso o script dorme indefinidamente até o MQTT acordar)."""
    futuras = []
    for p in partidas:
        if p["data"] > agora:
            futuras.append(p)
    if not futuras:
        return None

    proxima = min(futuras, key=lambda p: p["data"])
    tempo_ate_janela = proxima["data"] - agora
    return max(tempo_ate_janela.total_seconds(), 60)


def dormir_interrompivel(segundos):
    """Dorme até `segundos` passarem OU até uma notificação MQTT chegar.
    Se `segundos` for None, dorme indefinidamente até o MQTT acordar."""
    if segundos is None:
        print("Nenhuma partida futura cadastrada. Aguardando notificação MQTT...")
    else:
        print(f"Aguardando até {int(segundos)}s (ou notificação MQTT)...")

    acordado_por_mqtt = acordar.wait(timeout=segundos)  # timeout=None espera para sempre
    if acordado_por_mqtt:
        print("Loop acordado por notificação MQTT.")
        acordar.clear()


print("Monitoramento iniciado...")

while True:
    agora = datetime.now(timezone.utc)
    partidas = buscar_partidas_pendentes()
    partidas_na_janela = []
    for p in partidas:
        if partida_esta_na_janela(p, agora):
            partidas_na_janela.append(p)

    if partidas_na_janela:
        print(f"{len(partidas_na_janela)} partida(s) na janela. Consultando API...")
        checar_jogos_ao_vivo(partidas_na_janela)
        # Rebusca para ver se ainda há partidas ativas após a checagem
        agora = datetime.now(timezone.utc)
        partidas = buscar_partidas_pendentes()
        ainda_na_janela = []
        for p in partidas:
            if partida_esta_na_janela(p, agora):
                ainda_na_janela.append(p)

        if ainda_na_janela:
            proxima_pausa = SLEEP_QUANDO_AO_VIVO
        else:
            proxima_pausa = calcular_proxima_pausa(partidas, agora)
    else:
        print("Nenhuma partida na janela agora.")
        proxima_pausa = calcular_proxima_pausa(partidas, agora)

    dormir_interrompivel(proxima_pausa)