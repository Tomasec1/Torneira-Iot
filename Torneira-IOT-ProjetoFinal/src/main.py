from flask import Flask, render_template, request, redirect, url_for
import psycopg2
import psycopg2.extras
import paho.mqtt.client as mqtt
import certifi
import json
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

app = Flask(__name__)

# --- Timezone ---
# O usuário digita e enxerga horário de Brasília; o banco (timestamptz) guarda
# tudo em UTC. As duas funções abaixo fazem essa conversão nos dois sentidos.
TZ_LOCAL = ZoneInfo("America/Sao_Paulo")


def parse_data_hora_local(data_hora_str):
    """Recebe string 'YYYY-MM-DDTHH:MM' do input datetime-local (horário de
    Brasília) e retorna datetime timezone-aware em UTC, pronto para salvar."""
    dt_local = datetime.fromisoformat(data_hora_str).replace(tzinfo=TZ_LOCAL)
    return dt_local.astimezone(timezone.utc)


def formatar_data_hora(dt, formato="%d/%m/%Y %H:%M"):
    """Filtro Jinja: converte um datetime UTC (vindo do banco) para horário de
    Brasília e formata para exibição."""
    if dt is None:
        return "-"
    return dt.astimezone(TZ_LOCAL).strftime(formato)


app.jinja_env.filters["data_local"] = formatar_data_hora

# --- Config MQTT ---
MQTT_HOST = "mqtt.janks.dev.br"
MQTT_PORT = 8883
MQTT_USER = "aula"
MQTT_PASS = "zowmad-tavQez"


def on_connect(client, userdata, flags, codigo):
    print("Conectado ao MQTT com código:", codigo)


def on_message(client, userdata, mensagem):
    texto = mensagem.payload.decode("utf-8")
    print(f"Tópico {mensagem.topic}: {texto}")


mqtt_client = mqtt.Client()
mqtt_client.tls_set(certifi.where())
mqtt_client.username_pw_set(username=MQTT_USER, password=MQTT_PASS)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message

mqtt_client.connect(MQTT_HOST, port=MQTT_PORT, keepalive=10)
mqtt_client.loop_start()  # roda em background, não trava o Flask

# --- Config Banco ---
CONFIG_BANCO = {
    "host": "postgresql.janks.dev.br",
    "port": 5432,
    "dbname": "projeto_b",
    "user": "iot",
    "password": "pepcon-garton",
}


def conectar():
    return psycopg2.connect(**CONFIG_BANCO)


def notificar_mqtt(mandante_id, visitante_id, mandante_nome, visitante_nome,
                    gols_mandante, gols_visitante, status, data):
    
    status_legivel = {
    "agendada": "Agendada",
    "em_andamento": "Em andamento",
    "finalizada": "Finalizada"
}
    mensagem = json.dumps({
        "mandante_id": mandante_id,
        "visitante_id": visitante_id,
        "mandante_nome": mandante_nome,
        "visitante_nome": visitante_nome,
        "gols_mandante": gols_mandante,
        "gols_visitante": gols_visitante,
        "status": status_legivel.get(status, status),
        "data": data.astimezone(ZoneInfo("America/Sao_Paulo")).strftime("%d/%m/%Y às %H:%M"),  # datetime -> string ISO (UTC) para o JSON
    })
    mqtt_client.publish("partidas/atualizada", payload=mensagem)


@app.route("/partidas")
def pagina_partidas():
    conexao = conectar()
    try:
        cursor = conexao.cursor(cursor_factory=psycopg2.extras.RealDictCursor)
        cursor.execute("""
                    SELECT
                        p.id,
                        p.data,
                        p.status,
                        p.gols_mandante,
                        p.gols_visitante,
                        tm.nome AS mandante_nome,
                        tv.nome AS visitante_nome
                    FROM partidas p
                    INNER JOIN times tm ON tm.id_time = p.mandante
                    INNER JOIN times tv ON tv.id_time = p.visitante
                    ORDER BY p.data DESC
                    LIMIT 20
                """)
        partidas = cursor.fetchall()
        cursor.close()
    finally:
        conexao.close()

    return render_template("partidas.html", partidas=partidas)


def buscar_times():
    conexao = conectar()
    try:
        cursor = conexao.cursor(cursor_factory=psycopg2.extras.RealDictCursor)
        cursor.execute("SELECT id_time, nome, sigla FROM times ORDER BY nome ASC")
        times = cursor.fetchall()
        cursor.close()
    finally:
        conexao.close()
    return times


@app.route("/cadastrar", methods=["GET"])
def pagina_cadastrar():
    times = buscar_times()
    return render_template("cadastrar.html", times=times)


@app.route("/cadastrar", methods=["POST"])
def tratar_cadastro():
    mandante = request.form["mandante"]
    visitante = request.form["visitante"]
    data = parse_data_hora_local(request.form["data_hora"])

    conexao = conectar()

    cursor = conexao.cursor(cursor_factory=psycopg2.extras.RealDictCursor)
    cursor.execute("""
        INSERT INTO partidas (mandante, visitante, data, status, gols_mandante, gols_visitante)
        VALUES (%s, %s, %s, %s, %s, %s)
    """, (mandante, visitante, data, "agendada", 0, 0))
    conexao.commit()

    cursor.execute("SELECT nome FROM times WHERE id_time = %s", (mandante,))
    mandante_nome = cursor.fetchone()["nome"]

    cursor.execute("SELECT nome FROM times WHERE id_time = %s", (visitante,))
    visitante_nome = cursor.fetchone()["nome"]

    cursor.close()
    conexao.close()

    notificar_mqtt(mandante, visitante, mandante_nome, visitante_nome, 0, 0, "agendada", data)

    return redirect(url_for("pagina_partidas"))


@app.route("/editar", methods=["GET"])
def pagina_editar():
    id_partida = request.args.get("id", type=int)

    conexao = conectar()
    cursor = conexao.cursor(cursor_factory=psycopg2.extras.RealDictCursor)

    cursor.execute("""
        SELECT id, mandante, visitante, data, status, gols_mandante, gols_visitante
        FROM partidas
        WHERE id = %s
    """, (id_partida,))
    partida = cursor.fetchone()

    cursor.close()
    conexao.close()

    if partida is None:
        return "Partida nao encontrada!", 404

    times = buscar_times()

    return render_template("editar.html", partida=partida, times=times)


@app.route("/editar", methods=["POST"])
def tratar_edicao():
    id_partida = request.form["id"]
    mandante = request.form["mandante"]
    visitante = request.form["visitante"]
    data = parse_data_hora_local(request.form["data_hora"])
    status = request.form["status"]
    if status == "agendada":
        gols_mandante = 0
        gols_visitante = 0
    else:
        gols_mandante = request.form["gols_mandante"]
        gols_visitante = request.form["gols_visitante"]

    conexao = conectar()
    cursor = conexao.cursor(cursor_factory=psycopg2.extras.RealDictCursor)

    cursor.execute("""
        UPDATE partidas
        SET mandante = %s,
            visitante = %s,
            data = %s,
            status = %s,
            gols_mandante = %s,
            gols_visitante = %s
        WHERE id = %s
    """, (mandante, visitante, data, status, gols_mandante,
          gols_visitante, id_partida))

    conexao.commit()

    cursor.execute("SELECT nome FROM times WHERE id_time = %s", (mandante,))
    mandante_nome = cursor.fetchone()["nome"]

    cursor.execute("SELECT nome FROM times WHERE id_time = %s", (visitante,))
    visitante_nome = cursor.fetchone()["nome"]

    cursor.close()
    conexao.close()

    notificar_mqtt(mandante, visitante, mandante_nome, visitante_nome,
                    gols_mandante, gols_visitante, status, data)

    return redirect(url_for("pagina_partidas"))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)