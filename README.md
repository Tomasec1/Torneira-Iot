# Torneira-Iot

## 👥 Integrantes

| Nome | Matrícula |
|---|---|
| Rafael Castro | 2211104 |
| Luísa | 2210875 |
| Tomás | 2310215 |
| Henrique | 2211802|

# 🚰 Torneira Inteligente — Sistema de Bolão

Sistema integrado de dispensação de bebidas com identificação por RFID, cobrança automática por volume e mecânica de bolão esportivo com desconto por acertos. Desenvolvido para uso em bar/evento, unindo firmware embarcado (ESP32-S3), automação em Node-RED, uma aplicação Web em Flask e dashboards em Grafana.

## 📋 Visão Geral

A torneira inteligente permite que o cliente se identifique via cartão RFID, aproxime o copo e receba a bebida, que é medida automaticamente por um sensor de fluxo. O valor a pagar é calculado com base no volume dispensado e o saldo de bônus do bolão (quando existir) é descontado automaticamente. Toda a lógica de negócio (validação do RFID, cálculo de consumo e persistência) roda no **Node-RED**, que se comunica com o firmware via **MQTT** e com o banco em **PostgreSQL**.

Em paralelo, uma aplicação Web em **Flask** permite cadastrar e editar partidas do bolão, e um serviço de monitoramento (`monitor_copa.py`) consulta a API de futebol para atualizar placares ao vivo e fechar as partidas automaticamente.

## 🔄 Fluxo de Funcionamento

1. O cliente aproxima o cartão RFID do leitor acoplado à torneira.
2. O ESP32 publica o UID lido em `torneira/rfid/in`; o Node-RED consulta o vínculo copo/cliente no banco e responde em `torneira/rfid/out` com a autorização, saldo de bônus e dados do cliente.
3. O display e-paper exibe as instruções e o cliente aproxima o copo do sensor infravermelho da torneira.
4. Ao pressionar o botão com o copo posicionado, a válvula (relé) é aberta e o sensor de fluxo (YF-S201) passa a contar pulsos.
5. Ao retirar o copo ou soltar o botão, a válvula fecha e o firmware calcula o volume dispensado, publicando o consumo em `torneira/consumo/in`.
6. O Node-RED calcula o valor a pagar, aplica o desconto do saldo de bônus (se houver), grava o consumo no banco e atualiza o bônus restante do cliente, respondendo em `torneira/consumo/out`.
7. O display mostra o volume servido e o valor final cobrado, e o sistema reinicia para o próximo cliente.

## 🔧 Hardware / Firmware (ESP32-S3)

Firmware em C++ (PlatformIO, `board = esp32-s3-devkitc-1`), implementado como uma máquina de estados (`AGUARDANDO_CONFIG → PRONTO → VALIDANDO_RFID → AGUARDANDO_COPO_NA_TORNEIRA → SIRVA_SE → SERVINDO → AGUARDANDO_RETIRAR_COPO → AGUARDANDO_SALVAR_CONSUMO → RESULTADO`, com tratamento de `ERRO_TEMPORARIO` para timeouts de RFID, copo ou MQTT).

Componentes controlados pelo firmware (`src/torneira.cpp`):

- **ESP32-S3** — microcontrolador central; conecta ao Wi-Fi e ao broker MQTT via TLS (certificado em `certificados.h`).
- **Display e-paper (GxEPD2, 2.9")** — exibe bebida, preço por litro, QR Code do link da bebida, instruções e resultado do consumo (volume e valor).
- **Leitor RFID (MFRC522)** — identifica o cliente pelo UID do cartão.
- **Sensor de fluxo (YF-S201)** — mede o volume dispensado por contagem de pulsos via interrupção, convertido em mL com um fator de calibração.
- **Sensor infravermelho** — detecta a presença do copo, com debounce para evitar leituras instáveis.
- **Relé** — controla a abertura/fechamento da válvula da torneira.
- **LED de status** — aceso fixo durante o serviço e piscando durante "Sirva-se".
- **Botão** — aciona o início da dispensação enquanto pressionado com o copo posicionado.

## 🌐 Comunicação MQTT

O ESP32 se comunica com o Node-RED através de três pares de tópicos request/response (broker `mqtt.janks.dev.br`, TLS na porta 8883):

| Fluxo | Tópico de entrada (ESP32 → Node-RED) | Tópico de saída (Node-RED → ESP32) |
|---|---|---|
| Configuração da torneira | `torneira/config/in` | `torneira/config/out` |
| Validação do RFID / copo | `torneira/rfid/in` | `torneira/rfid/out` |
| Registro do consumo | `torneira/consumo/in` | `torneira/consumo/out` |

A aplicação Flask e o `monitor_copa.py` publicam atualizações de partidas no tópico `partidas/atualizada`, permitindo que outros serviços (como o Node-RED do bolão) reajam em tempo real a mudanças de placar e status.

## 🎲 Lógica do Bolão

- **`Node-RED/torneira.json`** trata a operação da torneira em si: valida a torneira e o vínculo copo/cliente no PostgreSQL, autoriza ou nega o RFID, calcula o consumo e atualiza o saldo de bônus do cliente.
- **`Node-RED/calculo_bolao.json`** roda periodicamente (via *inject* com repetição) e processa o fechamento do bolão:
  1. Busca as partidas finalizadas ainda não calculadas.
  2. Para cada partida, busca todas as apostas (`bets`) e define o vencedor (mandante, visitante ou empate) a partir do placar.
  3. Calcula o pagamento no modelo de **bolão parimutuel**: separa apostas vencedoras e perdedoras, soma o total apostado em cada grupo e distribui o total perdido entre os vencedores, proporcionalmente ao valor apostado por cada um.
  4. Agrega o bônus ganho por cliente em cada partida e depois entre todas as partidas do ciclo.
  5. Atualiza o saldo de bônus (`bonus_total`) de cada cliente e marca as partidas como calculadas.
- O saldo de bônus é aplicado automaticamente no consumo da torneira: o firmware recebe `bonus_total` na resposta do RFID e o Node-RED da torneira desconta esse valor do preço do consumo no momento da compra.

## 🌍 Gestão de Partidas — Aplicação Web (Flask)

Aplicação Web (`src/main.py`) para administrar as partidas do bolão, com páginas renderizadas via Jinja (`src/templates/`):

- **`/partidas`** — lista as últimas 20 partidas cadastradas, com times, placar e status.
- **`/cadastrar`** — formulário para criar uma nova partida (mandante, visitante, data/hora), que é salva como `agendada` e notificada via MQTT.
- **`/editar`** — formulário para editar uma partida existente (times, data/hora, status e placar).

Detalhes de implementação:

- Todos os horários digitados/exibidos são em horário de Brasília (`America/Sao_Paulo`); internamente são convertidos e armazenados em UTC no banco (`timestamptz`).
- A cada cadastro/edição, a aplicação publica no tópico MQTT `partidas/atualizada` um JSON com os dados da partida, para que outros serviços fiquem sincronizados em tempo real.

## ⚽ Monitoramento Automático de Partidas (`monitor_copa.py`)

Serviço independente que mantém os placares sincronizados sem intervenção manual, consultando a **API-Football (api-sports.io)**:

- Busca no banco as partidas ainda não finalizadas e verifica quais já estão dentro da janela de horário (já deveriam ter começado).
- Enquanto houver partidas "na janela", consulta os jogos ao vivo na API a cada 5 minutos, comparando o placar retornado com o salvo no banco; ao detectar mudança, atualiza a partida (`em_andamento`) e notifica via MQTT.
- Quando uma partida some da lista de jogos ao vivo, busca o placar final da API (status `FT`/`AET`/`PEN`) para confirmar o encerramento e marca a partida como `finalizada`.
- Quando não há partidas na janela, o serviço calcula quanto tempo falta para a próxima e "dorme" até esse momento — podendo ser acordado antes via MQTT (tópicos `partidas/atualizada` e `partidas/inject`), por exemplo quando uma nova partida é cadastrada pela aplicação Web.

## 📊 Dashboards

Dashboards em Grafana (queries em `SQL-Dashboard/queries_sql_bolao.sql`), com filtros por jogo e por cliente, exibindo em tempo real:

- **Taxa de acerto geral** — percentual de acertos considerando todas as apostas do cliente/grupo.
- **Taxa de acerto por jogo** — percentual de acerto filtrado para uma partida específica.
- **Quantidade acumulada por cliente** — nome do cliente, número de acertos e saldo de bônus acumulado (R$).
- **Total de apostas em cada resultado** — detalhes da partida (data, status) e quantidade de apostas registradas para mandante, empate e visitante.
- **Quantidade apostada** — valor total apostado (R$) em cada resultado possível (mandante, empate, visitante) e o total geral apostado na partida.
- **Ranking de clientes por bônus acumulado** e **ranking de clientes por total de acertos** — top 5 participantes.

## 🛠️ Stack Tecnológica

| Camada | Tecnologia |
|---|---|
| Firmware / Hardware | ESP32-S3, PlatformIO, display e-paper (GxEPD2), RFID (MFRC522), sensor de fluxo YF-S201, sensor IV, relé, LED |
| Comunicação | MQTT (TLS) |
| Lógica de negócio / Automação | Node-RED (PostgreSQL nodes, functions em JS) |
| Aplicação Web | Python, Flask, Jinja2 |
| Monitoramento de placares | Python, API-Football (api-sports.io), paho-mqtt |
| Persistência | PostgreSQL |
| Visualização | Dashboards Grafana |

## Easy EDA
<img width="627" height="405" alt="PCB_PCB_PCB_CircuitoReduzido_2026-07-08_2026-07-13" src="https://github.com/user-attachments/assets/b0e7cc54-c7fa-4830-bc97-f19055b58b3e" />
<img width="1169" height="828" alt="Schematic_Circuito-Bolão-Atualizado_2026-07-13" src="https://github.com/user-attachments/assets/60093a5b-d48c-4ab1-affb-56207dd668d3" />




