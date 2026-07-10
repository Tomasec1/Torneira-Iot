#include <Arduino.h>

#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <QRCodeGFX.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "certificados.h"
#include <MQTT.h>

#include <ArduinoJson.h>

#include <SPI.h>
#include <MFRC522.h>

#include <GFButton.h>

// ==================== TELA EPAPER ====================

U8G2_FOR_ADAFRUIT_GFX fontes;
GxEPD2_290_T94_V2 modeloTela(10, 14, 15, 16);
GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> tela(modeloTela);
QRCodeGFX qrcode(tela);

// ==================== RFID ====================

MFRC522 rfid(46, 17);

MFRC522::MIFARE_Key chaveA = {
  {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};

// ==================== PINOS DO PROJETO ====================

const int PINO_BOTAO = 18;
const int PINO_LED = 47;
const int PINO_INFRA = 4;
const int PINO_RELE = 21;
const int PINO_FLUXO = 48;

GFButton botao(PINO_BOTAO);

// ==================== LÓGICA DOS MÓDULOS ====================

const int RELE_LIGADO = LOW;
const int RELE_DESLIGADO = HIGH;

const int LED_LIGADO = HIGH;
const int LED_DESLIGADO = LOW;

const int INFRA_COM_COPO = LOW;

// ==================== CONFIGURAÇÕES DO PROJETO ====================

const int ID_TORNEIRA = 1;

float fatorCalibracao = 8.0;

const unsigned long TEMPO_TIMEOUT_RFID_MS = 15000;
const unsigned long TEMPO_TIMEOUT_COPO_MS = 30000;
const unsigned long TEMPO_RESET_MS = 5000;
const unsigned long TEMPO_ERRO_MS = 5000;
const unsigned long TEMPO_REPEDIR_CONFIG_MS = 5000;
const unsigned long TEMPO_TIMEOUT_MQTT_MS = 10000;
const unsigned long TEMPO_DEBOUNCE_INFRA_MS = 300;
const unsigned long TEMPO_PISCA_LED_MS = 350;

// ==================== WIFI / MQTT ====================

const char* WIFI_NOME = "LabIoT";
const char* WIFI_SENHA = "4n1m4l5@))!!";

const char* MQTT_HOST = "mqtt.janks.dev.br";
const int MQTT_PORTA = 8883;

const char* MQTT_CLIENT_ID = "torneira_01";
const char* MQTT_USUARIO = "aula";
const char* MQTT_SENHA = "zowmad-tavQez";

const char* TOPICO_CONFIG_IN = "torneira/config/in";
const char* TOPICO_CONFIG_OUT = "torneira/config/out";

const char* TOPICO_RFID_IN = "torneira/rfid/in";
const char* TOPICO_RFID_OUT = "torneira/rfid/out";

const char* TOPICO_CONSUMO_IN = "torneira/consumo/in";
const char* TOPICO_CONSUMO_OUT = "torneira/consumo/out";

WiFiClientSecure conexaoSegura;
MQTTClient mqtt(2048);

// ==================== SENSOR DE FLUXO ====================

volatile unsigned long pulsosFluxo = 0;
volatile bool fluxoAtivo = false;
portMUX_TYPE muxFluxo = portMUX_INITIALIZER_UNLOCKED;

// ==================== DADOS DA TORNEIRA ====================

bool configRecebida = false;
unsigned long ultimoPedidoConfig = 0;

String bebidaAtual = "";
String linkAtual = "";
float precoLitro = 0.0;

// ==================== DADOS DO COPO / CLIENTE ====================

String ultimoRFIDEnviado = "";

int idVinculo = 0;
int idCliente = 0;
int idCopo = 0;
float bonusTotal = 0.0;

// ==================== DADOS DO CONSUMO ====================

float ultimoVolumeMl = 0.0;
float ultimoValorReal = 0.0;
float ultimoValorPosDesconto = 0.0;
float ultimoBonusAntes = 0.0;
float ultimoBonusRestante = 0.0;
float ultimoDescontoUsado = 0.0;

unsigned long ultimoPrintServico = 0;

// ==================== INFRAVERMELHO / LED ====================

bool estadoCopoEstavel = false;
bool ledPiscando = false;
unsigned long ultimoPiscaLed = 0;

// ==================== ESTADOS ====================

enum EstadoSistema {
  AGUARDANDO_CONFIG,
  PRONTO,
  VALIDANDO_RFID,
  AGUARDANDO_COPO_NA_TORNEIRA,
  SIRVA_SE,
  SERVINDO,
  AGUARDANDO_RETIRAR_COPO,
  AGUARDANDO_SALVAR_CONSUMO,
  RESULTADO,
  ERRO_TEMPORARIO
};

EstadoSistema estado = AGUARDANDO_CONFIG;
unsigned long inicioEstado = 0;

// ==================== INTERRUPÇÃO DO FLUXO ====================

void IRAM_ATTR contarPulsoFluxo() {
  if (fluxoAtivo) {
    portENTER_CRITICAL_ISR(&muxFluxo);
    pulsosFluxo++;
    portEXIT_CRITICAL_ISR(&muxFluxo);
  }
}

void zerarPulsosFluxo() {
  portENTER_CRITICAL(&muxFluxo);
  pulsosFluxo = 0;
  portEXIT_CRITICAL(&muxFluxo);
}

unsigned long lerPulsosFluxo() {
  portENTER_CRITICAL(&muxFluxo);
  unsigned long copia = pulsosFluxo;
  portEXIT_CRITICAL(&muxFluxo);

  return copia;
}

// ==================== FUNÇÕES AUXILIARES ====================

void mudarEstado(EstadoSistema novoEstado) {
  estado = novoEstado;
  inicioEstado = millis();
}

bool leituraBrutaCopoPerto() {
  return digitalRead(PINO_INFRA) == INFRA_COM_COPO;
}

void atualizarSensorInfra() {
  static bool primeiraLeitura = true;
  static bool ultimaLeituraBruta = false;
  static unsigned long momentoMudanca = 0;

  bool leituraAtual = leituraBrutaCopoPerto();

  if (primeiraLeitura) {
    primeiraLeitura = false;
    ultimaLeituraBruta = leituraAtual;
    estadoCopoEstavel = leituraAtual;
    momentoMudanca = millis();
    return;
  }

  if (leituraAtual != ultimaLeituraBruta) {
    ultimaLeituraBruta = leituraAtual;
    momentoMudanca = millis();
  }

  if (millis() - momentoMudanca >= TEMPO_DEBOUNCE_INFRA_MS) {
    estadoCopoEstavel = leituraAtual;
  }
}

bool copoPerto() {
  return estadoCopoEstavel;
}

bool botaoPressionado() {
  return botao.isPressed() == true;
}

String formatarReais(float valor) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "R$ %.2f", valor);
  return String(buffer);
}

String formatarMl(float valor) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%.0f mL", valor);
  return String(buffer);
}

void atualizarLedEstado() {
  if (estado == SERVINDO) {
    digitalWrite(PINO_LED, LED_LIGADO);
    return;
  }

  if (estado == SIRVA_SE) {
    if (millis() - ultimoPiscaLed > TEMPO_PISCA_LED_MS) {
      ledPiscando = !ledPiscando;
      digitalWrite(PINO_LED, ledPiscando ? LED_LIGADO : LED_DESLIGADO);
      ultimoPiscaLed = millis();
    }
    return;
  }

  ledPiscando = false;
  digitalWrite(PINO_LED, LED_DESLIGADO);
}

// ==================== TELAS ====================

void limparTela() {
  tela.fillScreen(GxEPD_WHITE);
  tela.display(true);
}

void desenharCarregando() {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(35, 45);
  fontes.print("Carregando...");

  fontes.setFont(u8g2_font_helvR10_te);
  fontes.setFontMode(1);
  fontes.setCursor(25, 80);
  fontes.print("Buscando dados da torneira");

  tela.display(true);
}

void desenharInicial() {
  limparTela();

  fontes.setFont(u8g2_font_helvB12_te);
  fontes.setFontMode(1);
  fontes.setCursor(10, 25);
  fontes.print(bebidaAtual);

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(10, 50);
  fontes.print(formatarReais(precoLitro));
  fontes.print(" / litro");

  if (linkAtual.length() > 0) {
    qrcode.setScale(2);
    qrcode.draw(linkAtual, 205, 6);
  }

  fontes.setFont(u8g2_font_helvR10_te);
  fontes.setFontMode(1);
  fontes.setCursor(10, 98);
  fontes.print("Aproxime o copo do RFID");

  fontes.setCursor(10, 116);
  fontes.print("para comecar");

  tela.display(true);
}

void desenharValidandoRFID() {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(45, 50);
  fontes.print("Validando copo...");

  tela.display(true);
}

void desenharAproximeNaTorneira() {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(30, 35);
  fontes.print("Copo autorizado");

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(25, 70);
  fontes.print("Aproxime o copo");

  fontes.setCursor(45, 95);
  fontes.print("na torneira");

  tela.display(true);
}

void desenharSirvaSe() {
  limparTela();

  fontes.setFont(u8g2_font_helvB18_te);
  fontes.setFontMode(1);
  fontes.setCursor(85, 50);
  fontes.print("Sirva-se!");

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(35, 85);
  fontes.print("Pressione o botao");

  tela.display(true);
}

void desenharServindo() {
  limparTela();

  fontes.setFont(u8g2_font_helvB18_te);
  fontes.setFontMode(1);
  fontes.setCursor(70, 50);
  fontes.print("Servindo...");

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(42, 85);
  fontes.print("Mantenha o copo");

  tela.display(true);
}

void desenharRetireCopo() {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(35, 45);
  fontes.print("Valvula fechada");

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(25, 80);
  fontes.print("Retire o copo");

  tela.display(true);
}

void desenharSalvando() {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(55, 50);
  fontes.print("Calculando...");

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(45, 85);
  fontes.print("Salvando consumo");

  tela.display(true);
}

void desenharResultado() {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(30, 28);
  fontes.print("Consumo finalizado");

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(25, 60);
  fontes.print("Volume: ");
  fontes.print(formatarMl(ultimoVolumeMl));

  fontes.setCursor(25, 85);
  fontes.print("Valor: ");
  fontes.print(formatarReais(ultimoValorPosDesconto));

  fontes.setFont(u8g2_font_helvR10_te);
  fontes.setFontMode(1);
  fontes.setCursor(20, 115);
  fontes.print("Sistema reiniciara em 5s");

  tela.display(true);
}

void desenharErro(String linha1, String linha2) {
  limparTela();

  fontes.setFont(u8g2_font_helvB14_te);
  fontes.setFontMode(1);
  fontes.setCursor(20, 45);
  fontes.print(linha1);

  fontes.setFont(u8g2_font_helvR12_te);
  fontes.setFontMode(1);
  fontes.setCursor(15, 85);
  fontes.print(linha2);

  tela.display(true);
}

// ==================== WIFI / MQTT ====================

void reconectarWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(WIFI_NOME, WIFI_SENHA);

    Serial.print("Conectando ao WiFi");

    while (WiFi.status() != WL_CONNECTED) {
      Serial.print(".");
      delay(1000);
    }

    Serial.println();
    Serial.print("WiFi conectado. IP: ");
    Serial.println(WiFi.localIP());
  }
}

void solicitarConfigTorneira() {
  if (!mqtt.connected()) {
    return;
  }

  JsonDocument doc;
  doc["id_torneira"] = ID_TORNEIRA;

  String saida;
  serializeJson(doc, saida);

  mqtt.publish(TOPICO_CONFIG_IN, saida);

  ultimoPedidoConfig = millis();

  Serial.println();
  Serial.println("===== PEDIDO CONFIG TORNEIRA =====");
  Serial.println(saida);
  Serial.println("==================================");
}

void reconectarMQTT() {
  if (!mqtt.connected()) {
    Serial.print("Conectando MQTT");

    while (!mqtt.connected()) {
      mqtt.connect(MQTT_CLIENT_ID, MQTT_USUARIO, MQTT_SENHA);
      Serial.print(".");
      delay(1000);
    }

    Serial.println();
    Serial.println("MQTT conectado!");

    mqtt.subscribe(TOPICO_CONFIG_OUT);
    mqtt.subscribe(TOPICO_RFID_OUT);
    mqtt.subscribe(TOPICO_CONSUMO_OUT);
  }
}

// ==================== RFID ====================

String lerRFID() {
  String id = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i > 0) {
      id += " ";
    }

    if (rfid.uid.uidByte[i] < 0x10) {
      id += "0";
    }

    id += String(rfid.uid.uidByte[i], HEX);
  }

  id.toUpperCase();

  return id;
}

void publicarRFID(String uid) {
  ultimoRFIDEnviado = uid;

  JsonDocument doc;

  doc["id_torneira"] = ID_TORNEIRA;
  doc["rfid"] = uid;

  String saida;
  serializeJson(doc, saida);

  bool enviado = mqtt.publish(TOPICO_RFID_IN, saida);

  Serial.println();
  Serial.println("===== RFID ENVIADO MQTT =====");
  Serial.println(saida);
  Serial.print("Enviado: ");
  Serial.println(enviado ? "SIM" : "NAO");
  Serial.println("=============================");

  if (!enviado) {
    desenharErro("Erro MQTT", "RFID nao enviado");
    mudarEstado(ERRO_TEMPORARIO);
  }
}

void processarLeituraRFID() {
  if ((rfid.PICC_IsNewCardPresent()) &&
      (rfid.PICC_ReadCardSerial())) {

    String id = lerRFID();

    Serial.println("UID: " + id);

    publicarRFID(id);

    if (estado != ERRO_TEMPORARIO) {
      desenharValidandoRFID();
      mudarEstado(VALIDANDO_RFID);
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
}

// ==================== CÁLCULO DO CONSUMO ====================

float calcularVolumeMl(unsigned long pulsos) {
  float volumeLitros = pulsos / (fatorCalibracao * 60.0);
  float volumeMl = volumeLitros * 1000.0;

  return volumeMl;
}

void calcularValores(float quantidadeMl) {
  ultimoVolumeMl = quantidadeMl;

  ultimoValorReal = precoLitro * quantidadeMl / 1000.0;

  ultimoBonusAntes = bonusTotal;

  ultimoValorPosDesconto = ultimoValorReal - ultimoBonusAntes;
  if (ultimoValorPosDesconto < 0) {
    ultimoValorPosDesconto = 0;
  }

  ultimoBonusRestante = ultimoBonusAntes - ultimoValorReal;
  if (ultimoBonusRestante < 0) {
    ultimoBonusRestante = 0;
  }

  ultimoDescontoUsado = ultimoBonusAntes - ultimoBonusRestante;
  if (ultimoDescontoUsado < 0) {
    ultimoDescontoUsado = 0;
  }

  Serial.println();
  Serial.println("===== CALCULO DO CONSUMO =====");

  Serial.print("Bebida: ");
  Serial.println(bebidaAtual);

  Serial.print("Quantidade servida: ");
  Serial.print(ultimoVolumeMl);
  Serial.println(" mL");

  Serial.print("Preco por litro: R$ ");
  Serial.println(precoLitro, 2);

  Serial.print("Valor antes do desconto: R$ ");
  Serial.println(ultimoValorReal, 2);

  Serial.print("Bonus antes: R$ ");
  Serial.println(ultimoBonusAntes, 2);

  Serial.print("Desconto usado: R$ ");
  Serial.println(ultimoDescontoUsado, 2);

  Serial.print("Valor pos desconto: R$ ");
  Serial.println(ultimoValorPosDesconto, 2);

  Serial.print("Bonus restante: R$ ");
  Serial.println(ultimoBonusRestante, 2);

  Serial.println("==============================");
}

bool publicarConsumo() {
  JsonDocument doc;

  doc["id_torneira"] = ID_TORNEIRA;
  doc["id_vinculo"] = idVinculo;
  doc["id_cliente"] = idCliente;
  doc["id_copo"] = idCopo;

  doc["bebida"] = bebidaAtual;
  doc["quantidade_ml"] = ultimoVolumeMl;
  doc["preco_litro"] = precoLitro;

  doc["valor_real"] = ultimoValorReal;
  doc["valor_pos_desconto"] = ultimoValorPosDesconto;

  doc["bonus_antes"] = ultimoBonusAntes;
  doc["desconto_usado"] = ultimoDescontoUsado;
  doc["bonus_restante"] = ultimoBonusRestante;

  String saida;
  serializeJson(doc, saida);

  bool enviado = mqtt.publish(TOPICO_CONSUMO_IN, saida);

  Serial.println();
  Serial.println("===== CONSUMO ENVIADO MQTT =====");
  Serial.println(saida);
  Serial.print("Enviado: ");
  Serial.println(enviado ? "SIM" : "NAO");
  Serial.println("================================");

  return enviado;
}

// ==================== CONTROLE DA VÁLVULA ====================

void iniciarServico() {
  Serial.println();
  Serial.println("===== INICIO DO SERVICO =====");

  zerarPulsosFluxo();
  fluxoAtivo = true;

  digitalWrite(PINO_RELE, RELE_LIGADO);
  digitalWrite(PINO_LED, LED_LIGADO);

  ultimoPrintServico = millis();

  desenharServindo();
  mudarEstado(SERVINDO);

  Serial.println("Valvula aberta");
  Serial.println("Contando pulsos do YF-S201...");
  Serial.println("=============================");
}

void finalizarServico() {
  fluxoAtivo = false;

  digitalWrite(PINO_RELE, RELE_DESLIGADO);
  digitalWrite(PINO_LED, LED_DESLIGADO);

  unsigned long pulsosDose = lerPulsosFluxo();

  Serial.println();
  Serial.println("===== SERVICO PAUSADO =====");
  Serial.print("Pulsos ate agora: ");
  Serial.println(pulsosDose);
  Serial.println("Valvula fechada");
  Serial.println("===========================");
}

void finalizarEEnviarConsumo() {
  fluxoAtivo = false;

  digitalWrite(PINO_RELE, RELE_DESLIGADO);
  digitalWrite(PINO_LED, LED_DESLIGADO);

  unsigned long pulsosDose = lerPulsosFluxo();
  float quantidadeMl = calcularVolumeMl(pulsosDose);

  Serial.println();
  Serial.println("===== FIM DO SERVICO =====");
  Serial.print("Pulsos totais: ");
  Serial.println(pulsosDose);

  Serial.print("Volume calculado: ");
  Serial.print(quantidadeMl);
  Serial.println(" mL");
  Serial.println("==========================");

  calcularValores(quantidadeMl);

  desenharSalvando();

  bool enviado = publicarConsumo();

  if (enviado) {
    mudarEstado(AGUARDANDO_SALVAR_CONSUMO);
  } else {
    desenharErro("Erro MQTT", "Consumo nao enviado");
    mudarEstado(ERRO_TEMPORARIO);
  }
}

// ==================== RECEBIMENTO MQTT ====================

void tratarConfig(String conteudo) {
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, conteudo);

  if (erro) {
    Serial.println("Erro ao ler JSON de config");
    desenharErro("Erro config", "JSON invalido");
    mudarEstado(ERRO_TEMPORARIO);
    return;
  }

  bool encontrado = doc["encontrado"] | false;

  if (!encontrado) {
    Serial.println("Torneira nao encontrada no banco");
    desenharErro("Erro na torneira", "ID nao encontrado");
    mudarEstado(ERRO_TEMPORARIO);
    configRecebida = false;
    return;
  }

  const char* bebida = doc["bebida"] | "";
  const char* link = doc["link"] | "";

  bebidaAtual = String(bebida);
  linkAtual = String(link);
  precoLitro = doc["preco_litro"] | 0.0;

  configRecebida = true;

  Serial.println();
  Serial.println("===== CONFIG RECEBIDA =====");

  Serial.print("Bebida: ");
  Serial.println(bebidaAtual);

  Serial.print("Preco por litro: R$ ");
  Serial.println(precoLitro, 2);

  Serial.print("Link: ");
  Serial.println(linkAtual);

  Serial.println("===========================");

  desenharInicial();
  mudarEstado(PRONTO);
}

void tratarRFID(String conteudo) {
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, conteudo);

  if (erro) {
    Serial.println("Erro ao ler JSON de RFID");
    desenharErro("Erro RFID", "JSON invalido");
    mudarEstado(ERRO_TEMPORARIO);
    return;
  }

  if (estado != VALIDANDO_RFID) {
    Serial.println("Resposta RFID fora de hora ignorada");
    return;
  }

  String rfidRecebido = doc["rfid"].as<String>();

  if (rfidRecebido.length() > 0 &&
      ultimoRFIDEnviado.length() > 0 &&
      rfidRecebido != ultimoRFIDEnviado) {
    Serial.println("Resposta RFID ignorada: UID diferente");
    return;
  }

  bool autorizado = doc["autorizado"] | false;

  if (!autorizado) {
    Serial.println("Copo nao autorizado");

    idVinculo = 0;
    idCliente = 0;
    idCopo = 0;
    bonusTotal = 0;
    ultimoRFIDEnviado = "";

    desenharErro("Copo nao identificado", "Procure um atendente");
    mudarEstado(ERRO_TEMPORARIO);
    return;
  }

  idVinculo = doc["id_vinculo"] | 0;
  idCliente = doc["id_cliente"] | 0;
  idCopo = doc["id_copo"] | 0;
  bonusTotal = doc["bonus_total"] | 0.0;
  ultimoRFIDEnviado = "";

  Serial.println();
  Serial.println("===== RFID AUTORIZADO =====");

  Serial.print("id_vinculo: ");
  Serial.println(idVinculo);

  Serial.print("id_cliente: ");
  Serial.println(idCliente);

  Serial.print("id_copo: ");
  Serial.println(idCopo);

  Serial.print("bonus_total: R$ ");
  Serial.println(bonusTotal, 2);

  Serial.println("===========================");

  desenharAproximeNaTorneira();
  mudarEstado(AGUARDANDO_COPO_NA_TORNEIRA);
}

void tratarConsumoSalvo(String conteudo) {
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, conteudo);

  if (erro) {
    Serial.println("Erro ao ler JSON de consumo salvo");
    desenharErro("Erro consumo", "JSON invalido");
    mudarEstado(ERRO_TEMPORARIO);
    return;
  }

  bool salvo = doc["salvo"] | false;

  if (!salvo) {
    Serial.println("Node-RED informou erro ao salvar consumo");
    desenharErro("Erro ao salvar", "Tente novamente");
    mudarEstado(ERRO_TEMPORARIO);
    return;
  }

  ultimoVolumeMl = doc["quantidade_ml"] | ultimoVolumeMl;
  ultimoValorReal = doc["valor_real"] | ultimoValorReal;
  ultimoValorPosDesconto = doc["valor_pos_desconto"] | ultimoValorPosDesconto;
  ultimoBonusRestante = doc["bonus_restante"] | ultimoBonusRestante;

  bonusTotal = ultimoBonusRestante;

  Serial.println();
  Serial.println("===== CONSUMO SALVO NO BANCO =====");

  Serial.print("Volume salvo: ");
  Serial.print(ultimoVolumeMl);
  Serial.println(" mL");

  Serial.print("Valor antes desconto: R$ ");
  Serial.println(ultimoValorReal, 2);

  Serial.print("Valor pos desconto: R$ ");
  Serial.println(ultimoValorPosDesconto, 2);

  Serial.print("Bonus restante: R$ ");
  Serial.println(ultimoBonusRestante, 2);

  Serial.println("==================================");

  desenharResultado();
  mudarEstado(RESULTADO);
}

void recebeuMensagem(String topico, String conteudo) {
  Serial.println();
  Serial.print("MQTT recebido [");
  Serial.print(topico);
  Serial.print("]: ");
  Serial.println(conteudo);

  if (topico == TOPICO_CONFIG_OUT) {
    tratarConfig(conteudo);
  } else if (topico == TOPICO_RFID_OUT) {
    tratarRFID(conteudo);
  } else if (topico == TOPICO_CONSUMO_OUT) {
    tratarConsumoSalvo(conteudo);
  }
}

// ==================== RESET ====================

void resetarParaInicial() {
  Serial.println();
  Serial.println("===== RESET DO SISTEMA =====");

  fluxoAtivo = false;

  digitalWrite(PINO_RELE, RELE_DESLIGADO);
  digitalWrite(PINO_LED, LED_DESLIGADO);

  zerarPulsosFluxo();

  idVinculo = 0;
  idCliente = 0;
  idCopo = 0;
  bonusTotal = 0;
  ultimoRFIDEnviado = "";

  ultimoVolumeMl = 0;
  ultimoValorReal = 0;
  ultimoValorPosDesconto = 0;
  ultimoBonusAntes = 0;
  ultimoBonusRestante = 0;
  ultimoDescontoUsado = 0;

  Serial.println("Sistema pronto para outro usuario");
  Serial.println("=============================");

  if (configRecebida) {
    desenharInicial();
    mudarEstado(PRONTO);
  } else {
    desenharCarregando();
    mudarEstado(AGUARDANDO_CONFIG);
  }
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Iniciando torneira IoT...");

  pinMode(PINO_LED, OUTPUT);
  digitalWrite(PINO_LED, LED_DESLIGADO);

  pinMode(PINO_INFRA, INPUT);

  pinMode(PINO_RELE, OUTPUT);
  digitalWrite(PINO_RELE, RELE_DESLIGADO);

  pinMode(PINO_FLUXO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PINO_FLUXO), contarPulsoFluxo, RISING);

  SPI.begin();
  rfid.PCD_Init();

  tela.init();
  tela.setRotation(1);
  tela.fillScreen(GxEPD_WHITE);
  tela.display(true);

  fontes.begin(tela);
  fontes.setForegroundColor(GxEPD_BLACK);

  desenharCarregando();

  reconectarWiFi();

  conexaoSegura.setCACert(certificado1);

  mqtt.begin(MQTT_HOST, MQTT_PORTA, conexaoSegura);
  mqtt.onMessage(recebeuMensagem);
  mqtt.setKeepAlive(10);
  mqtt.setWill("torneira/status", "offline");

  reconectarMQTT();

  solicitarConfigTorneira();

  mudarEstado(AGUARDANDO_CONFIG);
}

// ==================== LOOP ====================

void loop() {
  reconectarWiFi();
  reconectarMQTT();

  mqtt.loop();
  botao.process();
  atualizarSensorInfra();

  if (!configRecebida) {
    if (millis() - ultimoPedidoConfig > TEMPO_REPEDIR_CONFIG_MS) {
      solicitarConfigTorneira();
    }
  }

  switch (estado) {
    case AGUARDANDO_CONFIG:
      break;

    case PRONTO:
      processarLeituraRFID();
      break;

    case VALIDANDO_RFID:
      if (millis() - inicioEstado > TEMPO_TIMEOUT_MQTT_MS) {
        desenharErro("Tempo esgotado", "RFID sem resposta");
        mudarEstado(ERRO_TEMPORARIO);
      }
      break;

    case AGUARDANDO_COPO_NA_TORNEIRA:
      if (millis() - inicioEstado > TEMPO_TIMEOUT_COPO_MS) {
        resetarParaInicial();
      } else if (copoPerto()) {
        desenharSirvaSe();
        mudarEstado(SIRVA_SE);
      }
      break;

    case SIRVA_SE:
      if (millis() - inicioEstado > TEMPO_TIMEOUT_COPO_MS) {
        resetarParaInicial();
      } else if (!copoPerto()) {
        desenharAproximeNaTorneira();
        mudarEstado(AGUARDANDO_COPO_NA_TORNEIRA);
      } else if (copoPerto() && botaoPressionado()) {
        iniciarServico();
      }
      break;

    case SERVINDO:
      if (!copoPerto()) {
        finalizarServico();
        finalizarEEnviarConsumo();
      } else if (!botaoPressionado()) {
        finalizarServico();
        desenharRetireCopo();
        mudarEstado(AGUARDANDO_RETIRAR_COPO);
      } else {
        if (millis() - ultimoPrintServico > 1000) {
          unsigned long pulsosAgora = lerPulsosFluxo();
          float volumeAgoraMl = calcularVolumeMl(pulsosAgora);

          Serial.print("Servindo | Pulsos: ");
          Serial.print(pulsosAgora);
          Serial.print(" | Volume parcial: ");
          Serial.print(volumeAgoraMl);
          Serial.println(" mL");

          ultimoPrintServico = millis();
        }
      }
      break;

    case AGUARDANDO_RETIRAR_COPO:
      if (!copoPerto()) {
        finalizarEEnviarConsumo();
      } else if (millis() - inicioEstado > TEMPO_TIMEOUT_COPO_MS) {
        finalizarEEnviarConsumo();
      }
      break;

    case AGUARDANDO_SALVAR_CONSUMO:
      if (millis() - inicioEstado > TEMPO_TIMEOUT_MQTT_MS) {
        desenharErro("Erro ao salvar", "Sem resposta MQTT");
        mudarEstado(ERRO_TEMPORARIO);
      }
      break;

    case RESULTADO:
      if (millis() - inicioEstado > TEMPO_RESET_MS) {
        resetarParaInicial();
      }
      break;

    case ERRO_TEMPORARIO:
      if (millis() - inicioEstado > TEMPO_ERRO_MS) {
        resetarParaInicial();
      }
      break;
  }

  atualizarLedEstado();
}
