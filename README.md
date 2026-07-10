# Torneira-Iot

## 👥 Integrantes

| Nome | Matrícula |
|---|---|
| Rafael Castro | [matrícula] |
| Luísa | [matrícula] |
| Tomás | [matrícula] |
| Henrique | [matrícula] |

# 🚰 Torneira Inteligente — Sistema de Bolão

Sistema integrado de dispensação de bebidas com identificação por RFID, cobrança automática por volume e mecânica de bolão esportivo com desconto por acertos. Desenvolvido para uso em bar/evento, unindo eletrônica embarcada, automação e dashboards de acompanhamento em tempo real.

## 📋 Visão Geral

A torneira inteligente permite que o cliente se identifique via cartão RFID, escolha a bebida desejada, e tenha o consumo medido automaticamente por um sensor de fluxo, que calcula o valor a pagar com base no volume dispensado. O sistema também integra a lógica do bolão: acertos em apostas esportivas geram descontos aplicados diretamente no consumo do cliente.

## 🔄 Fluxo de Funcionamento

1. O cliente aproxima o copo da torneira.
2. Escaneia seu cartão RFID na leitora acoplada ao circuito.
3. O display exibe os dados do cliente: nome, saldo de bônus/desconto disponível.
4. O cliente seleciona a bebida desejada.
5. O sensor de fluxo mede o volume dispensado em tempo real.
6. O sistema calcula o valor a pagar com base no volume e exibe no display.
7. O saldo de bônus (proveniente dos acertos no bolão) é descontado automaticamente, quando aplicável.

## 🔧 Hardware / Circuito

Placa de circuito desenvolvida em **EasyEDA**, projetada e impressa sob medida para o projeto, contendo:

- **ESP32** — microcontrolador central, responsável pela leitura dos sensores, controle do display e comunicação via MQTT.
- **Leitor RFID** — identificação do cliente.
- **Display** — exibição de nome do cliente, saldo de bônus, bebida selecionada e valor a pagar.
- **Sensor de fluxo** — mede o volume de líquido dispensado para cálculo do consumo.
- **Sensores de distância infravermelhos** — detecção de aproximação do copo/cliente.
- **Relés** — acionamento/controle da liberação de bebida.
- **LEDs** — sinalização visual de status (aguardando, servindo, concluído, erro).
- **Resistores** — suporte aos circuitos de sinalização e leitura dos sensores.

## 🌐 Comunicação

- **MQTT** é o protocolo utilizado para integrar os diferentes subsistemas: ESP32 (torneira) ↔ Node-RED ↔ banco de dados ↔ dashboards.
- Cada evento relevante (identificação de cliente, início/fim de dispensação, volume medido, valor calculado) é publicado em tópicos MQTT específicos, permitindo que os demais serviços reajam em tempo real.

## 🎲 Lógica do Bolão

- **Node-RED** processa a lógica de apostas: recebe os palpites dos participantes e os resultados reais dos jogos, calculando os acertos de cada cliente.
- Os **resultados dos jogos** são obtidos via **API esportiva externa**, com uma **aplicação Web complementar** para inserção manual dos resultados caso a API esteja indisponível ou incompleta.
- Acertos no bolão geram **desconto/bônus**, que fica associado ao cliente (via RFID) e é aplicado automaticamente no momento da compra de bebida na torneira.

## 📊 Dashboards

Dashboards em Grafana, com filtros por jogo e por cliente, exibindo em tempo real:

Taxa de acerto geral — percentual de acertos considerando todas as apostas do cliente/grupo.
Taxa de acerto por jogo — percentual de acerto filtrado para uma partida específica.
Quantidade acumulada por cliente — nome do cliente, número de acertos e saldo de bônus acumulado (R$).
Total de apostas em cada resultado — detalhes da partida (data, status) e quantidade de apostas registradas para mandante, empate e visitante.
Quantidade apostada — valor total apostado (R$) em cada resultado possível (mandante, empate, visitante) e o total geral apostado na partida.
Ranking de clientes por bônus acumulado — classificação geral dos participantes pelo saldo de bônus acumulado no bolão.


## 🛠️ Stack Tecnológica

| Camada | Tecnologia |
|---|---|
| Circuito / Hardware | EasyEDA (design da PCB), ESP32, RFID, sensores de fluxo, IV, relés, LEDs |
| Comunicação | MQTT |
| Lógica de negócio / Automação | Node-RED |
| Persistência | Banco de dados (PostgreSQL) |
| Resultados esportivos | API externa + aplicação Web para inserção manual |
| Visualização | Dashboards Grafana |
