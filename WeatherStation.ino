/*
MIT License

Copyright (c) 2018 Daniel Eichhorn - ThingPulse (código original)
Copyright (c) 2026 LuismarPavani

É concedida permissão, gratuitamente, a qualquer pessoa que obtenha uma cópia deste
software e dos arquivos de documentação associados, para lidar com o Software sem
restrições, incluindo, sem limitação, os direitos de usar, copiar, modificar, mesclar,
publicar, distribuir, sublicenciar e/ou vender cópias do Software, sujeito às condições
do texto completo da licença MIT original (https://opensource.org/licenses/MIT).
*/

#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>                 // atualização de firmware pela rede
#include <EEPROM.h>                     // guarda chave/cidade/idioma na flash
#include <Ticker.h>                     // pisca o LED de status sem travar o loop
#include <ESP8266HTTPClient.h>          // envio das leituras ao painel (API)
#include <WiFiClientSecure.h>           // HTTPS
#include <coredecls.h>                  // settimeofday_cb()
#include <ESPHTTPClient.h>
#include <JsonListener.h>

#include <DNSServer.h>
#include <WiFiManager.h>                // https://github.com/tzapu/WiFiManager

// time
#include <time.h>                       // time() ctime()
#include <sys/time.h>                   // struct timeval

#include "SSD1306Wire.h"
#include "OLEDDisplayUi.h"
#include "Wire.h"
#include "OpenWeatherMapCurrent.h"
#include "OpenWeatherMapForecast.h"
#include "WeatherStationFonts.h"
#include "WeatherStationImages.h"

/* DHT22 (AM2302): dado no D5/GPIO14, 3,3 V e resistor de 10k entre dado e 3,3 V (se o módulo não tiver) */
#define DHTPIN 14                       // GPIO14 = D5
#define DHTTYPE DHT22
#include "DHT.h"
DHT dht(DHTPIN, DHTTYPE);
float localHum = 0;
float localTemp = 0;

/***************************
 * Begin Settings
 **************************/
ESP8266WebServer server(80);

#define HOSTNAME "ESP8266-OTA-"

// LED azul de status (GPIO2 = D4 e, em algumas NodeMCU, também GPIO16 = D0; os dois acendem em LOW).
//  - pisca rápido: conectando ao WiFi / portal de configuração aberto
//  - pisca devagar (500 ms a cada 2 s): sem WiFi
//  - pisca rápido durante o envio ao painel; ao fim, 1 acendida (ok) ou 3 piscadas curtas (falha)
// Normal: apagado. false = LED sempre apagado.
#define LED_STATUS_ATIVO true

// Bateria medida no A0 (divisor de tensão: 100k externo + 220k/100k da placa = fator 4,2).
// Ativar/desativar: aba "Bateria" em /config (salvo na flash). Este é só o padrão de fábrica.
#define BATERIA_ATIVA_PADRAO false
#define PIN_BATERIA     A0
const float BAT_FATOR = 4.2f;           // (R1 + R2) / R2 do divisor
const float BAT_CALIB = 0.99947619f;    // medido com multímetro (tensão real ÷ tensão lida)
const int   BAT_AMOSTRAS = 16;
#define BAT_AVISO_PCT   20              // abaixo disso: aviso de bateria baixa
#define BAT_CRITICA_PCT 10              // abaixo disso: aviso crítico

// Painel web (API REST): URL e DEVICE_KEY são configuradas em /config e salvas na flash.
#define API_URL_PADRAO "https://weatherstation-web.vercel.app/api/readings"   // pode ser trocada em /config
#define FIRMWARE_VERSAO "2.1"           // enviado ao painel no campo "firmware"
const unsigned long PAINEL_INTERVAL_MS = 15UL * 60UL * 1000UL;   // envio automático ao painel a cada 15 min
const unsigned long PAINEL_RETRY_MS    = 2UL * 60UL * 1000UL;    // se o envio falhar, tenta de novo em 2 min
#define API_TLS_RX_BUF 1024             // buffer de recepção do HTTPS (mesmo valor do WeatherApi.h; aumente se a conexão falhar)

// Fuso horário PADRÃO (o fuso pode ser trocado em /config e fica salvo na flash).
// Brasília = -3, sem horário de verão.
// (No código original: TZ -4 + DST 60, que na prática resultava em -3.)
#define TZ              -3       // (utc+) TZ in hours
#define DST_MN          0        // use 60mn for summer time in some countries

// Intervalos de atualização
const unsigned long UPDATE_INTERVAL_MS = 15UL * 60UL * 1000UL; // clima externo: a cada 15 min
const unsigned long RETRY_INTERVAL_MS  = 60UL * 1000UL;        // nova tentativa se estiver sem WiFi
const unsigned long API_RETRY_MS       = 5UL * 60UL * 1000UL;  // nova tentativa se a API falhar
const unsigned long SENSOR_INTERVAL_MS    = 10UL * 1000UL;        // sensor interno: a cada 10 s

// Display Settings
const int I2C_DISPLAY_ADDRESS = 0x3c;
// Display OLED (SSD1306/SSD1315) em NodeMCU/ESP8266 comum: pinos I2C padrão, sem restrição de boot e sem o LED.
// SDA = D2 (GPIO4), SCL = D1 (GPIO5). Se a tela ficar em branco, troque os dois valores entre si.
const int SDA_PIN = D2;
const int SDC_PIN = D1;

// OpenWeatherMap Settings
// Estes 3 valores são configurados pelo navegador em http://IP-DA-ESTACAO/config
// e ficam salvos na flash; os valores abaixo são só os padrões de fábrica.
// Nunca publique a chave no GitHub.
String OPEN_WEATHER_MAP_APP_ID = "";
String OPEN_WEATHER_MAP_LOCATION_ID = "3463011";
String OPEN_WEATHER_MAP_LANGUAGE = "pt_br";
const uint8_t MAX_FORECASTS = 3;

const boolean IS_METRIC = true;

// Adjust according to your language
const String WDAY_NAMES[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
const String MONTH_NAMES[] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN", "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};

/***************************
 * End Settings
 **************************/
SSD1306Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SDC_PIN);
OLEDDisplayUi   ui( &display );

OpenWeatherMapCurrentData currentWeather;
OpenWeatherMapCurrent currentWeatherClient;

OpenWeatherMapForecastData forecasts[MAX_FORECASTS];
OpenWeatherMapForecast forecastClient;

#define TZ_SEC          ((TZ)*3600)
#define DST_SEC         ((DST_MN)*60)
time_t now;

bool readyForWeatherUpdate = false;
unsigned long nextWeatherUpdate = 0;
unsigned long nextSensorRead = 0;
unsigned long nextRede = 0;

// Histórico interno (144 pontos x 5 min = 12 h), guardado em anel na RAM
const uint8_t HIST_SIZE = 144;
const unsigned long HIST_INTERVAL_MS = 5UL * 60UL * 1000UL;
int16_t histTemp[HIST_SIZE];            // temperatura x10
uint16_t histHum[HIST_SIZE];            // umidade x10 (1 casa decimal)
uint8_t histHead = 0;
uint8_t histCount = 0;
unsigned long nextHist = 0;
bool sensorOk = false;                  // true depois da primeira leitura válida
bool weatherOk = false;                 // true se a última consulta ao OpenWeatherMap trouxe dados
bool displayLigado = true;              // false = tela OLED apagada (salvo na EEPROM)

// Agenda do display: liga/apaga sozinho nos horários (minutos desde 00:00)
bool agendaAtiva = false;
uint16_t agendaLiga = 6 * 60;           // 06:00
uint16_t agendaDesliga = 23 * 60;       // 23:00
int8_t agendaUltimoEstado = -1;         // -1 = ainda não avaliada
unsigned long nextAgenda = 0;

// Fuso horário (minutos em relação ao UTC) e horário de verão
int16_t fusoMin = TZ * 60;
bool horarioVerao = (DST_MN != 0);

// Bateria (média móvel da tensão) e percentual estimado
bool bateriaAtiva = BATERIA_ATIVA_PADRAO;   // escolhido em /config
float bateriaV = 0;
int bateriaPct = 0;
unsigned long nextBateria = 0;

// Envio ao painel web (API)
String apiUrl = API_URL_PADRAO;
String deviceKey = "";
String apiDeviceId = "";                // device_id enviado ao painel ("" = esp8266-<chip>)
bool apiTentou = false;
bool apiUltimoOk = false;
String apiUltimoMsg = "";
unsigned long apiUltimoMs = 0;
unsigned long nextApi = 20000UL;        // 1º envio 20 s depois de ligar

// Configuração persistente (EEPROM)
#define CFG_MAGIC 0x57533031UL          // "WS01"
struct Config {
  uint32_t magic;
  char apiKey[41];
  char locationId[12];
  char lang[8];
  uint8_t displayLigado;                // 0 = apagado, qualquer outro valor = ligado
  uint8_t agendaAtiva;                  // 1 = agenda ligada
  uint16_t agendaLiga;                  // minutos desde 00:00
  uint16_t agendaDesliga;
  uint8_t fusoOk;                       // 0xA5 = fuso salvo pelo usuário
  int16_t fuso;                         // minutos em relação ao UTC (ex.: -180)
  uint8_t verao;                        // 1 = horário de verão (+1 h)
  uint8_t bateriaOk;                    // 0xA5 = escolha da bateria salva pelo usuário
  uint8_t bateria;                      // 1 = este ESP tem bateria
  uint8_t apiOk;                        // 0xA5 = URL e DEVICE_KEY salvas
  char apiUrl[121];
  char deviceKey[65];
  uint8_t deviceIdOk;                   // 0xA5 = device_id salvo
  char deviceId[33];
};

//declaring prototypes
void drawProgress(OLEDDisplay *display, int percentage, String label);
void updateData(OLEDDisplay *display);
void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawCurrentWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawForecast(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawCurrentWeatherDetails(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawClimaInterno(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawWiFi(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawForecastDetails(OLEDDisplay *display, int x, int y, int dayIndex);
void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state);
void leSensor();
void atualizaBateria();
void varreduraI2C();
void ledInicia();
void ledPulsoOk();
void ledPulsoFalha();
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void paginaInicial();
void paginaConfig();
void paginaJson();
void paginaHistorico();
void carregaConfig();
void salvaConfig();
void handleSalvar();
void handleDisplay();
void handleBateria();
void handleApi();
void handleEnviar();
bool enviaParaApi();
String idDispositivo();
void aplicaDisplay();
void handleAgenda();
void verificaAgenda();
void aplicaFuso();
void guardaHistorico();
void handleResetWiFi();
void handlePortal();

// frames are the single views that slide from right to left
FrameCallback frames[] = {drawDateTime, drawClimaInterno, drawCurrentWeather, drawCurrentWeatherDetails, drawForecast, drawWiFi};
int numberOfFrames = 6;

OverlayCallback overlays[] = { drawHeaderOverlay };
int numberOfOverlays = 1;

WiFiManager wifiManager;

/***************************************************
* LED azul de status (Ticker a cada 100 ms: continua piscando mesmo com o loop() ocupado)
****************************************************/
enum { LED_NORMAL = 0, LED_CONECTANDO = 1, LED_SEM_REDE = 2 };
volatile uint8_t ledRede = LED_NORMAL;  // estado contínuo da rede
volatile bool ledEnviando = false;      // true durante o envio ao painel
volatile uint8_t ledPulso = 0;          // 0 = nenhum, 1 = ok, 2 = falha
volatile uint8_t ledPulsoPos = 0;
volatile uint8_t ledPos = 0;
Ticker ledTicker;

void ledEscreve(bool aceso) {
  digitalWrite(2, aceso ? LOW : HIGH);  // LED do módulo (GPIO2 = D4), aceso em LOW
  digitalWrite(16, aceso ? LOW : HIGH); // 2º LED de algumas NodeMCU (GPIO16 = D0)
}

void ledTick() {
  bool aceso = false;
  if (LED_STATUS_ATIVO) {
    if (ledPulso) {
      // cada posição = 100 ms: ok = 300 ms aceso; falha = 3 piscadas curtas
      uint16_t bits = (ledPulso == 1) ? 0b0000000111 : 0b0000010101;
      aceso = (bits >> ledPulsoPos) & 1;
      if (++ledPulsoPos >= 10) { ledPulso = 0; ledPulsoPos = 0; }
    } else {
      if (ledEnviando || ledRede == LED_CONECTANDO) aceso = (ledPos % 2) == 0;   // pisca rápido (100/100 ms)
      else if (ledRede == LED_SEM_REDE) aceso = (ledPos % 20) < 5;                // 500 ms aceso a cada 2 s
      if (++ledPos >= 40) ledPos = 0;
    }
  }
  ledEscreve(aceso);
}

void ledInicia() {
  pinMode(2, OUTPUT);
  pinMode(16, OUTPUT);
  ledEscreve(false);
  ledTicker.attach_ms(100, ledTick);
}

void ledPulsoOk()    { ledPulsoPos = 0; ledPulso = 1; }
void ledPulsoFalha() { ledPulsoPos = 0; ledPulso = 2; }

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  EEPROM.begin(sizeof(Config));
  carregaConfig();

  ledInicia();                          // LED de status (apagado até algo acontecer)

  delay(50);
  dht.begin();

  // initialize display
  display.init();
  display.clear();
  display.display();
  aplicaDisplay();                      // respeita o estado salvo já na inicialização

  varreduraI2C();                       // lista no Serial o que respondeu no barramento (0x3C = display)

  //display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setContrast(255);

  display.drawString(64, 3, "Conectando ao WiFi ");
  display.drawXbm(33, 20, 60, 36, WiFi_Logo_bits);
  display.display();

  String hostname(HOSTNAME);
  hostname += String(ESP.getChipId(), HEX);
  WiFi.hostname(hostname);

  // Uncomment for testing wifi manager
  //wifiManager.resetSettings();
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  std::vector<const char *> menu = {"wifi","wifinoscan","info","param","close","sep","erase","restart","exit"};
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");

  // Se não conectar em 30 s abre o portal; se ninguém configurar em 3 min, reinicia.
  // As credenciais salvas NUNCA são apagadas automaticamente (uma queda do roteador não pode zerar o WiFi).
  wifiManager.setConnectTimeout(30);
  wifiManager.setConfigPortalTimeout(180);
  ledRede = LED_CONECTANDO;             // pisca rápido enquanto conecta (e com o portal aberto)
  if (!wifiManager.autoConnect("ESP_WeatherStation")) {
    Serial.println("Sem WiFi, reiniciando...");
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, "Conexão falhou...");
    display.drawString(64, 34, "Reiniciando");
    display.display();
    delay(2000);
    ESP.restart();
  }

  ledRede = LED_NORMAL;                 // conectou
  ledPulsoOk();

  // Get time from network time service
  aplicaFuso();

  // OTA: permite enviar novo firmware pelo WiFi (Arduino IDE > Porta de rede)
  ArduinoOTA.setHostname(hostname.c_str());
  ArduinoOTA.setPassword("2258");       // senha pedida pela IDE no envio por rede
  ArduinoOTA.begin();

  currentWeatherClient.setMetric(IS_METRIC);
  currentWeatherClient.setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  forecastClient.setMetric(IS_METRIC);
  forecastClient.setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  static uint8_t allowedHours[] = {12};
  forecastClient.setAllowedHours(allowedHours, sizeof(allowedHours));

  ui.setTargetFPS(30);
  ui.setActiveSymbol(activeSymbole);
  ui.setInactiveSymbol(inactiveSymbole);
  ui.setIndicatorPosition(BOTTOM);
  ui.setIndicatorDirection(LEFT_RIGHT);
  ui.setFrameAnimation(SLIDE_LEFT);
  ui.setFrames(frames, numberOfFrames);
  ui.setOverlays(overlays, numberOfOverlays);

  // Inital UI takes care of initalising the display too.
  ui.init();
  aplicaDisplay();                      // ui.init() religa a tela; reaplica o estado salvo

  Serial.println("");

  leSensor();
  if (bateriaAtiva) atualizaBateria();
  updateData(&display);

  server.on("/", HTTP_GET, paginaInicial);
  server.on("/json", HTTP_GET, paginaJson);
  server.on("/historico", HTTP_GET, paginaHistorico);
  server.on("/config", HTTP_GET, paginaConfig);
  server.on("/salvar", HTTP_POST, handleSalvar);
  server.on("/display", HTTP_POST, handleDisplay);
  server.on("/agenda", HTTP_POST, handleAgenda);
  server.on("/bateria", HTTP_POST, handleBateria);
  server.on("/api", HTTP_POST, handleApi);
  server.on("/enviar", HTTP_POST, handleEnviar);
  server.on("/reset-wifi", HTTP_POST, handleResetWiFi);
  server.on("/portal", HTTP_POST, handlePortal);
  server.begin();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();

  unsigned long ms = millis();

  // Estado da rede: pisca devagar se o WiFi cair (como em interruptores inteligentes)
  if ((long)(ms - nextRede) >= 0) {
    nextRede = ms + 1000UL;
    bool conectado = (WiFi.status() == WL_CONNECTED);
    if (!conectado && ledRede != LED_SEM_REDE) {
      ledRede = LED_SEM_REDE;
      Serial.println("WiFi caiu");
    } else if (conectado && ledRede == LED_SEM_REDE) {
      ledRede = LED_NORMAL;
      ledPulsoOk();
      Serial.println("WiFi voltou");
    }
  }

  // Sensor interno lido a cada 10 s
  if ((long)(ms - nextSensorRead) >= 0) {
    leSensor();
    nextSensorRead = ms + SENSOR_INTERVAL_MS;
  }

  if (sensorOk && (long)(ms - nextHist) >= 0) {
    guardaHistorico();
    nextHist = ms + HIST_INTERVAL_MS;
  }

  if ((long)(ms - nextApi) >= 0) {
    if (apiUrl.length() > 0 && deviceKey.length() > 0) {
      bool ok = enviaParaApi();
      nextApi = ms + (ok ? PAINEL_INTERVAL_MS : PAINEL_RETRY_MS);
    } else {
      nextApi = ms + 60000UL;           // API não configurada: confere de novo em 1 min
    }
  }

  if (bateriaAtiva && (long)(ms - nextBateria) >= 0) {
    atualizaBateria();
    nextBateria = ms + 30000UL;         // a cada 30 s
  }

  if ((long)(ms - nextAgenda) >= 0) {
    verificaAgenda();
    nextAgenda = ms + 5000UL;
  }

  if ((long)(ms - nextWeatherUpdate) >= 0) {
    readyForWeatherUpdate = true;
  }

  if (readyForWeatherUpdate && (!displayLigado || ui.getUiState()->frameState == FIXED)) {
    updateData(&display);
  }

  if (displayLigado) {
    int remainingTimeBudget = ui.update();

    if (remainingTimeBudget > 0) {
      delay(remainingTimeBudget);
    }
  } else {
    delay(50);                          // tela apagada: não gasta CPU nem I2C desenhando
  }
}

void drawProgress(OLEDDisplay *display, int percentage, String label) {
  display->clear();
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(64, 10, label);
  display->drawProgressBar(2, 28, 124, 10, percentage);
  display->display();
}

void updateData(OLEDDisplay *display) {
  readyForWeatherUpdate = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem WiFi, nova tentativa em 1 min");
    nextWeatherUpdate = millis() + RETRY_INTERVAL_MS;
    return;
  }

  if (OPEN_WEATHER_MAP_APP_ID.length() == 0) {
    Serial.println("Sem chave da API: configure em /config");
    weatherOk = false;
    nextWeatherUpdate = millis() + RETRY_INTERVAL_MS;
    return;
  }

  drawProgress(display, 20, "Atualizando o clima...");
  currentWeather.cityName = "";          // se continuar vazio depois, a consulta falhou
  currentWeatherClient.updateCurrentById(&currentWeather, OPEN_WEATHER_MAP_APP_ID, OPEN_WEATHER_MAP_LOCATION_ID);

  drawProgress(display, 60, "Atualizando previsões...");
  forecastClient.updateForecastsById(forecasts, OPEN_WEATHER_MAP_APP_ID, OPEN_WEATHER_MAP_LOCATION_ID, MAX_FORECASTS);

  weatherOk = currentWeather.cityName.length() > 0;
  drawProgress(display, 100, weatherOk ? "Pronto..." : "Falha ao obter o clima");
  delay(300);

  nextWeatherUpdate = millis() + (weatherOk ? UPDATE_INTERVAL_MS : API_RETRY_MS);
}

void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  now = time(nullptr);
  struct tm* timeInfo;
  timeInfo = localtime(&now);
  char buff[24];

  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);

  snprintf_P(buff, sizeof(buff), PSTR("%s, %02d/%02d/%04d"), WDAY_NAMES[timeInfo->tm_wday].c_str(), timeInfo->tm_mday, timeInfo->tm_mon + 1, timeInfo->tm_year + 1900);
  display->drawString(64 + x, 5 + y, String(buff));
  display->setFont(ArialMT_Plain_24);

  snprintf_P(buff, sizeof(buff), PSTR("%02d:%02d:%02d"), timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
  display->drawString(64 + x, 15 + y, String(buff));
  display->setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawCurrentWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64 + x, 38 + y, currentWeather.description);

  display->setFont(ArialMT_Plain_24);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  String temp = String(currentWeather.temp, 1) + (IS_METRIC ? "°C" : "°F");
  display->drawString(60 + x, 1 + y, temp);

  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(60 + x, 26 + y, currentWeather.cityName);

  display->setFont(Meteocons_Plain_36);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(32 + x, 0 + y, currentWeather.iconMeteoCon);
}

void drawForecast(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  drawForecastDetails(display, x, y, 0);
  drawForecastDetails(display, x + 44, y, 1);
  drawForecastDetails(display, x + 88, y, 2);
}

void drawForecastDetails(OLEDDisplay *display, int x, int y, int dayIndex) {
  time_t observationTimestamp = forecasts[dayIndex].observationTime;
  struct tm* timeInfo;
  timeInfo = localtime(&observationTimestamp);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(x + 20, y, WDAY_NAMES[timeInfo->tm_wday]);

  display->setFont(Meteocons_Plain_21);
  display->drawString(x + 20, y + 12, forecasts[dayIndex].iconMeteoCon);
  String temp = String(forecasts[dayIndex].temp, 0) + (IS_METRIC ? "°C" : "°F");
  display->setFont(ArialMT_Plain_10);
  display->drawString(x + 20, y + 34, temp);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  now = time(nullptr);
  struct tm* timeInfo;
  timeInfo = localtime(&now);
  char buff[14];
  snprintf_P(buff, sizeof(buff), PSTR("%02d:%02d"), timeInfo->tm_hour, timeInfo->tm_min);

  display->setColor(WHITE);
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 54, String(buff));
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  String temp = String(currentWeather.temp, 0) + (IS_METRIC ? "°C" : "°F");
  display->drawString(128, 54, temp);
  display->drawHorizontalLine(0, 52, 128);
}

void drawCurrentWeatherDetails(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(20 + x, y, "PREVISÃO HOJE");

  String unit = IS_METRIC ? "°C" : "°F";

  display->drawString(x, 12 + y, "Min: " + String(currentWeather.tempMin, 1) + unit);
  display->drawString(x, 24 + y, "Max: " + String(currentWeather.tempMax, 1) + unit);
  display->drawString(x, 36 + y, "Hum: " + String(currentWeather.humidity) + "%");

  // A API devolve: pressão em hPa, visibilidade em metros e vento em m/s (metric) ou mph (imperial)
  float wind = IS_METRIC ? currentWeather.windSpeed * 3.6 : currentWeather.windSpeed;
  display->drawString(60 + x, 12 + y, "Pre: " + String(currentWeather.pressure) + "hPa");
  display->drawString(60 + x, 24 + y, "Vis: " + String(currentWeather.visibility / 1000.0, 1) + "km");
  display->drawString(60 + x, 36 + y, "Ven: " + String(wind, 0) + (IS_METRIC ? "km/h" : "mph"));
}

/***************************************************
* Draw Indoor Page
****************************************************/
void drawClimaInterno(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(22 + x, y, "CLIMA INTERNO");
  display->drawString(0 + x, 12 + y, "Umid");
  display->drawString(66 + x, 12 + y, "Temp");

  // Valores com 1 casa decimal; a unidade vai em fonte pequena logo depois, para caber nas duas colunas
  String hs = sensorOk ? String(localHum, 1) : String("--");
  String ts = sensorOk ? String(localTemp, 1) : String("--");
  display->setFont(ArialMT_Plain_24);
  int wh = display->getStringWidth(hs);
  int wt = display->getStringWidth(ts);
  display->drawString(0 + x, 22 + y, hs);
  display->drawString(66 + x, 22 + y, ts);
  if (sensorOk) {
    display->setFont(ArialMT_Plain_10);
    display->drawString(0 + x + wh + 1, 36 + y, "%");
    display->drawString(66 + x + wt + 1, 36 + y, "°C");
  }
}

// Lista no Serial os endereços que responderam no barramento I2C (diagnóstico da ligação do display)
void varreduraI2C() {
  Serial.println("Varredura I2C:");
  int achados = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  dispositivo em 0x%02X\n", a);
      achados++;
    }
  }
  if (!achados) Serial.println("  nenhum dispositivo encontrado");
}

// Chamada só pelo loop() a cada SENSOR_INTERVAL_MS (o DHT22 só entrega leitura nova a cada ~2 s)
void leSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(h) || isnan(t)) {
    Serial.println("Falhou a leitura do sensor DHT22!");
    return;                             // mantém o último valor válido
  }
  localTemp = t;
  localHum = h;
  sensorOk = true;
}

/***************************************************
* Bateria (A0): leitura, percentual e ícone no OLED
****************************************************/
float lerTensaoBateria() {
  long soma = 0;
  for (int k = 0; k < BAT_AMOSTRAS; k++) {
    soma += analogRead(PIN_BATERIA);
    delay(2);
  }
  float adc = soma / (float)BAT_AMOSTRAS;          // 0..1023 corresponde a 0..1 V no chip
  return (adc / 1023.0f) * BAT_FATOR * BAT_CALIB;
}

// Curva aproximada de uma célula Li-ion (interpolação linear)
int bateriaPercentual(float v) {
  // Curva ajustada comparando com o multímetro (tensão em V -> carga em %)
  static const float   V[] = {4.20, 4.15, 4.10, 4.015, 3.93, 3.84, 3.755, 3.68, 3.61, 3.48, 3.29, 3.00};
  static const uint8_t P[] = { 100,   95,   90,    80,   70,   60,    50,   40,   30,   20,   10,    0};
  const int N = sizeof(V) / sizeof(V[0]);
  if (v >= V[0]) return 100;
  if (v <= V[N - 1]) return 0;
  for (int k = 0; k < N - 1; k++) {
    if (v <= V[k] && v > V[k + 1]) {
      return P[k + 1] + (int)((v - V[k + 1]) * (P[k] - P[k + 1]) / (V[k] - V[k + 1]));
    }
  }
  return 0;
}

void atualizaBateria() {
  float v = lerTensaoBateria();
  bateriaV = (bateriaV == 0) ? v : (bateriaV * 0.9f + v * 0.1f);   // média móvel: evita saltos
  bateriaPct = bateriaPercentual(bateriaV);
}

// Ícone de bateria no OLED (20x9 px)
void drawBateria(OLEDDisplay *display, int x, int y, int pct) {
  display->drawRect(x, y, 18, 9);                          // corpo
  display->fillRect(x + 18, y + 2, 2, 5);                  // ponta
  display->fillRect(x + 2, y + 2, (pct * 14) / 100, 5);    // nível
}

// Ícone de sinal de WiFi no OLED (4 barras crescentes, 15 x 10 px), igual ao da página web.
// Barras cheias = força do sinal; as demais ficam só com o contorno.
void drawSinalWiFi(OLEDDisplay *display, int x, int y, int pct) {
  int n = 0;
  if (pct >= 75) n = 4;
  else if (pct >= 50) n = 3;
  else if (pct >= 25) n = 2;
  else if (pct > 0) n = 1;
  for (int k = 0; k < 4; k++) {
    int h = 3 + k * 2;                  // alturas 3, 5, 7 e 9 px
    int bx = x + k * 4;                 // barras de 3 px com 1 px de espaço
    int by = y + 10 - h;                // alinhadas pela base
    if (k < n) display->fillRect(bx, by, 3, h);
    else display->drawRect(bx, by, 3, h);
  }
}

/***************************************************
* Draw WiFi
****************************************************/
int getRSSIasQuality(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void drawWiFi(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  char aux[48];
  int32_t rssi = WiFi.RSSI();

  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);

  // Linha 1: bateria
  if (bateriaAtiva) {
    drawBateria(display, x, y + 2, bateriaPct);
    display->drawString(x + 24, y, String(bateriaPct) + "%  " + String(bateriaV, 2) + "V");
  } else {
    display->drawString(x, y, "Bateria: desativada");
  }

  // Linha 2: potência do sinal (ícone + dBm + %)
  if (WiFi.status() == WL_CONNECTED) {
    int pct = getRSSIasQuality(rssi);
    drawSinalWiFi(display, x, 12 + y, pct);
    snprintf(aux, sizeof(aux), "%ddBm  %d%%", (int)rssi, pct);
    display->drawString(x + 24, 12 + y, aux);
  } else {
    drawSinalWiFi(display, x, 12 + y, 0);
    display->drawString(x + 24, 12 + y, "sem WiFi");
  }

  // Linha 3: nome da rede
  snprintf(aux, sizeof(aux), "SSID: %s", WiFi.SSID().c_str());
  display->drawString(x, 24 + y, aux);

  // Linha 4: IP
  snprintf(aux, sizeof(aux), "IP: %s", WiFi.localIP().toString().c_str());
  display->drawString(x, 36 + y, aux);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  display.displayOn();                  // mostra as instruções do portal mesmo com a tela apagada
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 10, "Wifi Manager");
  display.drawString(64, 20, "Conecte ao AP");
  display.drawString(64, 30, myWiFiManager->getConfigPortalSSID());
  display.drawString(64, 40, "Para configurar o Wifi");
  display.display();
}

void saveConfigCallback() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 10, "Configuração salva");
  display.display();
}

/***************************************************
* Servidor web
****************************************************/
// Página principal: HTML estático guardado na flash. Os números chegam por fetch('/json')
// e o gráfico por fetch('/historico'), então a página não recarrega e não usa a RAM do ESP.
static const char PAGE_INDEX[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Weather Station</title>
<style>
:root{--card:rgba(255,255,255,.09);--mut:#a9c1cf}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color:#fff;background:linear-gradient(160deg,#0f2027,#2c5364) fixed}
.wrap{max-width:720px;margin:0 auto;padding:16px}
.card{background:var(--card);border-radius:18px;padding:16px}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:12px;margin:12px 0}
@media(min-width:560px){.grid.g3{grid-template-columns:repeat(3,1fr)}}
.hero{display:flex;align-items:center;justify-content:space-between;margin-top:0}
.hero .t{font-size:64px;font-weight:300;line-height:1}
.hero .ico{font-size:76px;line-height:1}
.mut{color:var(--mut);font-size:.85rem}
.lbl{color:var(--mut);font-size:.75rem;text-transform:uppercase;letter-spacing:.06em;margin-bottom:6px}
.big{font-size:1.6rem;font-weight:500}
.bar{height:8px;border-radius:4px;background:rgba(255,255,255,.15);overflow:hidden;margin-top:10px}
.bar>i{display:block;height:100%;width:0;background:linear-gradient(90deg,#4fc3f7,#81d4fa);transition:width .6s}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.ring{--p:0;width:96px;height:96px;border-radius:50%;flex:none;display:grid;place-items:center;background:conic-gradient(#4fc3f7 calc(var(--p)*1%),rgba(255,255,255,.15) 0)}
.ring span{width:76px;height:76px;border-radius:50%;background:#17323f;display:grid;place-items:center;font-weight:500}
.fc{display:flex;justify-content:space-around;text-align:center}
.fc .e{font-size:2.2rem;margin:4px 0}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin:0 4px 0 10px}
svg text{fill:#a9c1cf;font-size:10px}
#chart{display:block;margin:0 auto;max-width:520px;touch-action:pan-y;cursor:crosshair}
#chart text{font-size:9px}
a{color:#81d4fa}
footer{text-align:center;margin:8px 0 24px}
.ibtn{display:inline-flex;align-items:center;justify-content:center;width:40px;height:40px;border-radius:12px;background:rgba(255,255,255,.1);border:0;color:#fff;cursor:pointer;padding:0;margin:0;text-decoration:none}
.ibtn:hover{background:rgba(255,255,255,.18)}
</style></head><body><div class="wrap">

<div id="top" style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px">
  <div style="display:flex;gap:8px">
    <a class="ibtn" href="/config" title="Configurações" aria-label="Configurações"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor"><circle cx="12" cy="12" r="8.6" stroke-width="3.4" stroke-dasharray="3.377 3.377"/><circle cx="12" cy="12" r="6.2" stroke-width="2.4"/><circle cx="12" cy="12" r="2.4" stroke-width="2"/></svg></a>
    <button class="ibtn" id="btnd" onclick="tog()" title="Ligar ou apagar a tela" aria-label="Ligar ou apagar a tela"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/><path id="slash" d="M3 3l18 18" stroke="#ef5350" style="display:none"/></svg></button>
  </div>
  <div style="display:flex;align-items:center;gap:14px">
    <svg id="wf" width="26" height="18" viewBox="0 0 26 18"><title id="wft">WiFi</title><rect id="w1" x="0" y="12" width="5" height="6" rx="1" fill="#ffffff40"/><rect id="w2" x="7" y="8" width="5" height="10" rx="1" fill="#ffffff40"/><rect id="w3" x="14" y="4" width="5" height="14" rx="1" fill="#ffffff40"/><rect id="w4" x="21" y="0" width="5" height="18" rx="1" fill="#ffffff40"/></svg>
    <div id="bat" style="display:none;align-items:center;gap:6px">
      <svg width="34" height="16" viewBox="0 0 34 16"><rect x="1" y="1" width="28" height="14" rx="3.5" fill="none" stroke="#ffffffcc" stroke-width="2"/><rect id="batn" x="4" y="4" width="0" height="8" rx="1.5" fill="#66bb6a"/><rect x="30" y="5" width="3" height="6" rx="1.2" fill="#ffffffcc"/></svg>
      <span id="batp" style="font-size:.9rem;font-weight:500">--%</span>
    </div>
  </div>
</div>
<div class="card" id="bataviso" style="display:none;margin-bottom:12px"></div>
<div class="card" id="erro" style="display:none;background:rgba(255,183,77,.2);margin-bottom:12px">&#9888;&#65039; Não consegui obter o clima externo. Confira a chave da API e o ID da cidade em <a href="/config">Configurações</a>. Chaves novas podem levar um tempo para ativar.</div>

<div class="card hero">
  <div>
    <div class="mut" id="cid">--</div>
    <div class="t" id="tmp">--</div>
    <div id="desc" style="text-transform:capitalize;margin-top:6px">--</div>
    <div class="mut" id="mm"></div>
  </div>
  <div class="ico" id="ico">&#10067;</div>
</div>

<div class="grid g3">
  <div class="card"><div class="lbl">Umidade</div><div class="big" id="hum">--</div><div class="bar"><i id="humb"></i></div></div>
  <div class="card"><div class="lbl">Pressão</div><div class="big" id="pres">--</div><div class="mut">hPa</div></div>
  <div class="card"><div class="lbl">Vento</div>
    <div class="row"><div><div class="big" id="ven">--</div><div class="mut" id="vdir"></div></div>
    <svg width="44" height="44" viewBox="-22 -22 44 44"><circle r="20" fill="none" stroke="#ffffff33" stroke-width="2"/><g id="seta"><path d="M0-15L6 8L0 4L-6 8Z" fill="#81d4fa"/></g></svg></div></div>
  <div class="card"><div class="lbl">Visibilidade</div><div class="big" id="vis">--</div></div>
  <div class="card"><div class="lbl">Nuvens</div><div class="big" id="nuv">--</div><div class="bar"><i id="nuvb"></i></div></div>
  <div class="card"><div class="lbl">Sol</div><div>&#127749; <span id="nas">--</span></div><div>&#127751; <span id="por">--</span></div></div>
</div>

<div class="card row">
  <div><div class="lbl">Clima interno</div><div class="big" style="font-size:2.4rem" id="tin">--</div><div class="mut">Temperatura</div></div>
  <div class="ring" id="ring"><span id="hin">--</span></div>
</div>

<div class="card" style="margin-top:12px"><div class="lbl">Próximos dias</div><div class="fc" id="fc"></div></div>

<div class="card" style="margin-top:12px">
  <div class="lbl">Histórico interno</div>
  <div id="hsel" style="font-size:.95rem;margin-bottom:6px">--</div>
  <svg id="chart" viewBox="0 0 320 170" width="100%"></svg>
  <div class="mut"><span class="dot" style="background:#ffb74d;margin-left:0"></span>Temperatura (°C, eixo esquerdo)<span class="dot" style="background:#4fc3f7"></span>Umidade (%, eixo direito)</div>
  <div class="mut" id="hlab"></div>
</div>

<footer class="mut"><div id="foot">Carregando...</div></footer>
</div>
<script>
const $=id=>document.getElementById(id);
const IC={'01':'☀️','02':'⛅','03':'☁️','04':'☁️','09':'🌧️','10':'🌦️','11':'⛈️','13':'❄️','50':'🌫️'};
function emo(c){if(!c)return'❔';let e=IC[c.slice(0,2)]||'❔';if(c.slice(0,2)==='01'&&c[2]==='n')e='🌙';return e}
const hm=t=>t?new Date(t*1000).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit'}):'--';
const dia=t=>new Date(t*1000).toLocaleDateString('pt-BR',{weekday:'short'}).replace('.','').toUpperCase();
const DIRS=['N','NE','L','SE','S','SO','O','NO'];
const f=(v,d)=>v==null?'--':Number(v).toFixed(d);

async function load(){
  try{
    const d=await (await fetch('/json',{cache:'no-store'})).json();
    const e=d.externo,i=d.interno,u=d.unid;
    $('erro').style.display=d.erro?'block':'none';
    const b=d.bateria;
    $('bat').style.display=b?'flex':'none';
    if(b){
      const p=b.pct,c=p<=10?'#ef5350':(p<=20?'#ffb74d':'#66bb6a'),av=$('bataviso');
      $('batn').setAttribute('width',Math.round(22*p/100));$('batn').setAttribute('fill',c);
      $('batp').textContent=p+'%';$('bat').title=b.v+' V';
      if(p<=20){av.style.display='block';av.style.background=p<=10?'rgba(239,83,80,.25)':'rgba(255,183,77,.2)';
        av.textContent=p<=10?'⚠️ Bateria crítica ('+p+'%). A estação pode desligar a qualquer momento.':'⚠️ Bateria baixa ('+p+'%). Conecte ao carregador.';}
      else av.style.display='none';
    }else $('bataviso').style.display='none';
    $('slash').style.display=d.display?'none':'block';
    $('btnd').title=d.display?'Apagar a tela':'Ligar a tela';
    const q=d.rssiPct,nb=q>=75?4:(q>=50?3:(q>=25?2:(q>0?1:0)));
    for(let k=1;k<=4;k++)$('w'+k).setAttribute('fill',k<=nb?'#ffffff':'#ffffff40');
    $('wft').textContent='WiFi '+d.rssi+' dBm ('+q+'%)';
    $('cid').textContent=e.cidade||'--';
    $('tmp').textContent=f(e.temp,0)+u;
    $('desc').textContent=e.desc||'--';
    $('ico').textContent=emo(e.icone);
    $('mm').textContent='Mín '+f(e.min,0)+u+'  ·  Máx '+f(e.max,0)+u;
    $('hum').textContent=e.umid+'%';$('humb').style.width=e.umid+'%';
    $('pres').textContent=e.pressao;
    $('ven').textContent=e.vento+' km/h';
    $('vdir').textContent='de '+DIRS[Math.round(e.ventoDir/45)%8];
    $('seta').setAttribute('transform','rotate('+(e.ventoDir+180)+')');
    $('vis').textContent=f(e.visib,1)+' km';
    $('nuv').textContent=e.nuvens+'%';$('nuvb').style.width=e.nuvens+'%';
    $('nas').textContent=hm(e.nascer);$('por').textContent=hm(e.por);
    $('tin').textContent=i.temp==null?'--':f(i.temp,1)+u;
    $('hin').textContent=i.umid==null?'--':f(i.umid,1)+'%';
    $('ring').style.setProperty('--p',i.umid==null?0:i.umid);
    $('fc').innerHTML=d.previsao.filter(p=>p.dia>0).map(p=>'<div><div class="mut">'+dia(p.dia)+'</div><div class="e">'+emo(p.icone)+'</div><div>'+f(p.temp,0)+u+'</div></div>').join('');
    $('foot').textContent='Atualizado às '+new Date().toLocaleTimeString('pt-BR')+'  ·  WiFi '+d.rssi+' dBm ('+d.rssiPct+'%)';
  }catch(x){$('foot').textContent='Sem resposta da estação (atualizando dados?)...'}
}

const CW=320,CH=170,CL=40,CR=40,CT=10,CB=24;
let HD=null;
const fmtH=ms=>new Date(ms).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit'});
const fmtDur=min=>{min=Math.round(min);const h=Math.floor(min/60),m=min%60;return h?(m?h+' h '+m+' min':h+' h'):m+' min'};
function ext(a,span){
  let mn=Math.min(...a),mx=Math.max(...a);
  if(mx-mn<span){const c=(mn+mx)/2;mn=c-span/2;mx=c+span/2}
  else{const p=(mx-mn)*.08;mn-=p;mx+=p}
  return[mn,mx];
}
function draw(sel){
  if(!HD)return;
  const n=HD.t.length,pw=CW-CL-CR,ph=CH-CT-CB;
  const[t0,t1]=ext(HD.t,1),[u0,u1]=ext(HD.u,2);
  const X=k=>CL+k*pw/(n-1);
  const Yt=v=>CT+ph-(v-t0)/(t1-t0)*ph,Yu=v=>CT+ph-(v-u0)/(u1-u0)*ph;
  const tm=k=>fmtH(HD.t0-(n-1-k)*HD.iv);
  let g='';
  for(let i=0;i<3;i++){
    const y=CT+i*ph/2;
    g+='<line x1="'+CL+'" x2="'+(CW-CR)+'" y1="'+y+'" y2="'+y+'" stroke="#ffffff1f"/>';
    g+='<text x="'+(CL-5)+'" y="'+(y+3.5)+'" text-anchor="end" style="fill:#ffb74d">'+(t1-(t1-t0)*i/2).toFixed(1)+'</text>';
    g+='<text x="'+(CW-CR+5)+'" y="'+(y+3.5)+'" style="fill:#4fc3f7">'+(u1-(u1-u0)*i/2).toFixed(0)+'</text>';
  }
  const pts=(a,Y)=>a.map((v,k)=>X(k).toFixed(1)+','+Y(v).toFixed(1)).join(' ');
  g+='<polyline fill="none" stroke="#ffb74d" stroke-width="2" stroke-linejoin="round" points="'+pts(HD.t,Yt)+'"/>';
  g+='<polyline fill="none" stroke="#4fc3f7" stroke-width="2" stroke-linejoin="round" points="'+pts(HD.u,Yu)+'"/>';
  g+='<text x="'+CL+'" y="'+(CH-6)+'">'+tm(0)+'</text>'+
     '<text x="'+(CL+pw/2)+'" y="'+(CH-6)+'" text-anchor="middle">'+tm(Math.round((n-1)/2))+'</text>'+
     '<text x="'+(CW-CR)+'" y="'+(CH-6)+'" text-anchor="end">'+tm(n-1)+'</text>';
  const k=sel==null?n-1:sel;
  g+='<line x1="'+X(k)+'" x2="'+X(k)+'" y1="'+CT+'" y2="'+(CT+ph)+'" stroke="#ffffff66" stroke-dasharray="3 3"/>'+
     '<circle cx="'+X(k)+'" cy="'+Yt(HD.t[k])+'" r="4" fill="#ffb74d"/><circle cx="'+X(k)+'" cy="'+Yu(HD.u[k])+'" r="4" fill="#4fc3f7"/>';
  $('chart').innerHTML=g;
  $('hsel').innerHTML=(sel==null?'Última leitura ('+tm(k)+')':tm(k))+' &nbsp;·&nbsp; <b style="color:#ffb74d">'+HD.t[k].toFixed(1)+'°C</b> &nbsp;·&nbsp; <b style="color:#4fc3f7">'+HD.u[k].toFixed(1)+'%</b>';
}
function pick(e){
  if(!HD)return;
  const r=$('chart').getBoundingClientRect(),n=HD.t.length;
  const x=(e.clientX-r.left)/r.width*CW;
  draw(Math.max(0,Math.min(n-1,Math.round((x-CL)/(CW-CL-CR)*(n-1)))));
}
async function hist(){
  try{
    const d=await (await fetch('/historico',{cache:'no-store'})).json();
    const n=d.temp.length;
    if(!d.ok){HD=null;$('chart').innerHTML='';$('hsel').textContent='';$('hlab').textContent='Sensor DHT22 sem leitura válida. Confira a ligação (dado no pino D5).';return}
    if(n<2){HD=null;$('chart').innerHTML='';$('hsel').textContent='';$('hlab').textContent='Coletando dados... o gráfico aparece em cerca de '+Math.max(1,Math.ceil(d.proxima/60))+' min.';return}
    HD={t:d.temp,u:d.umid,iv:d.intervalo*1000,t0:Date.now()-(d.intervalo-Math.min(d.proxima,d.intervalo))*1000};
    draw();
    const dur=fmtDur((n-1)*d.intervalo/60);
    $('hlab').textContent='Últimas '+dur+' · Temp '+Math.min(...d.temp).toFixed(1)+' a '+Math.max(...d.temp).toFixed(1)+'°C · Umid '+Math.min(...d.umid).toFixed(1)+' a '+Math.max(...d.umid).toFixed(1)+'%';
  }catch(x){}
}
$('chart').addEventListener('pointermove',pick);
$('chart').addEventListener('pointerdown',pick);
$('chart').addEventListener('pointerleave',()=>draw());
async function tog(){try{await fetch('/display',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'estado=t'});load()}catch(x){}}
load();hist();setInterval(load,5000);setInterval(hist,60000);
</script></body></html>)rawliteral";

void paginaInicial() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX);
}

/***************************************************
* Configuração salva na flash (EEPROM): chave, cidade e idioma
* Sobrevive a reinicializações e a novos uploads de firmware
****************************************************/
static const char* const LANG_CODES[] = {"pt_br", "pt", "en", "es", "fr", "de", "it"};
static const char* const LANG_NAMES[] = {"Português (Brasil)", "Português (Portugal)", "English", "Español", "Français", "Deutsch", "Italiano"};
static const uint8_t LANG_COUNT = 7;

struct Fuso { int16_t min; const char* nome; };
static const Fuso FUSOS[] = {
  {-720, "UTC-12:00"},
  {-660, "UTC-11:00"},
  {-600, "UTC-10:00 (Havaí)"},
  {-540, "UTC-09:00 (Alasca)"},
  {-480, "UTC-08:00 (Califórnia)"},
  {-420, "UTC-07:00 (Colorado)"},
  {-360, "UTC-06:00 (México, Chicago)"},
  {-300, "UTC-05:00 (Acre, Nova York, Colômbia)"},
  {-240, "UTC-04:00 (Amazonas, Mato Grosso, Venezuela)"},
  {-210, "UTC-03:30 (Terra Nova)"},
  {-180, "UTC-03:00 (Brasília, Argentina)"},
  {-120, "UTC-02:00 (Fernando de Noronha)"},
  {-60,  "UTC-01:00 (Açores)"},
  {0,    "UTC+00:00 (Lisboa, Londres)"},
  {60,   "UTC+01:00 (Paris, Berlim)"},
  {120,  "UTC+02:00 (Atenas, Cairo)"},
  {180,  "UTC+03:00 (Moscou, Istambul)"},
  {210,  "UTC+03:30 (Irã)"},
  {240,  "UTC+04:00 (Dubai)"},
  {270,  "UTC+04:30 (Afeganistão)"},
  {300,  "UTC+05:00 (Paquistão)"},
  {330,  "UTC+05:30 (Índia)"},
  {345,  "UTC+05:45 (Nepal)"},
  {360,  "UTC+06:00 (Bangladesh)"},
  {390,  "UTC+06:30 (Mianmar)"},
  {420,  "UTC+07:00 (Bangkok)"},
  {480,  "UTC+08:00 (Pequim, Singapura)"},
  {540,  "UTC+09:00 (Tóquio)"},
  {570,  "UTC+09:30 (Adelaide)"},
  {600,  "UTC+10:00 (Sydney)"},
  {660,  "UTC+11:00"},
  {720,  "UTC+12:00 (Auckland)"},
  {780,  "UTC+13:00"},
  {840,  "UTC+14:00"}
};
static const uint8_t FUSO_COUNT = sizeof(FUSOS) / sizeof(FUSOS[0]);

// (Re)configura o NTP com o fuso escolhido. Pode ser chamada a qualquer momento.
void aplicaFuso() {
  configTime((int)fusoMin * 60, horarioVerao ? 3600 : 0, "pool.ntp.org");
}

void carregaConfig() {
  Config c;
  EEPROM.get(0, c);
  if (c.magic != CFG_MAGIC) return;     // nada salvo ainda: mantém os padrões do código
  c.apiKey[sizeof(c.apiKey) - 1] = 0;
  c.locationId[sizeof(c.locationId) - 1] = 0;
  c.lang[sizeof(c.lang) - 1] = 0;
  OPEN_WEATHER_MAP_APP_ID = String(c.apiKey);
  OPEN_WEATHER_MAP_LOCATION_ID = String(c.locationId);
  OPEN_WEATHER_MAP_LANGUAGE = String(c.lang);
  displayLigado = (c.displayLigado != 0);
  agendaAtiva = (c.agendaAtiva == 1);
  if (c.fusoOk == 0xA5 && c.fuso >= -720 && c.fuso <= 840) {   // só usa se foi salvo de fato
    fusoMin = c.fuso;
    horarioVerao = (c.verao == 1);
  }
  if (c.bateriaOk == 0xA5) bateriaAtiva = (c.bateria == 1);
  if (c.apiOk == 0xA5) {
    c.apiUrl[sizeof(c.apiUrl) - 1] = 0;
    c.deviceKey[sizeof(c.deviceKey) - 1] = 0;
    apiUrl = String(c.apiUrl);
    deviceKey = String(c.deviceKey);
  }
  if (c.deviceIdOk == 0xA5) {
    c.deviceId[sizeof(c.deviceId) - 1] = 0;
    apiDeviceId = String(c.deviceId);
  }
  if (c.agendaLiga < 1440 && c.agendaDesliga < 1440) {   // dados antigos/vazios são ignorados
    agendaLiga = c.agendaLiga;
    agendaDesliga = c.agendaDesliga;
  }
}

void salvaConfig() {
  Config c;
  memset(&c, 0, sizeof(c));
  c.magic = CFG_MAGIC;
  snprintf(c.apiKey, sizeof(c.apiKey), "%s", OPEN_WEATHER_MAP_APP_ID.c_str());
  snprintf(c.locationId, sizeof(c.locationId), "%s", OPEN_WEATHER_MAP_LOCATION_ID.c_str());
  snprintf(c.lang, sizeof(c.lang), "%s", OPEN_WEATHER_MAP_LANGUAGE.c_str());
  c.displayLigado = displayLigado ? 1 : 0;
  c.agendaAtiva = agendaAtiva ? 1 : 0;
  c.fusoOk = 0xA5;
  c.fuso = fusoMin;
  c.verao = horarioVerao ? 1 : 0;
  c.bateriaOk = 0xA5;
  c.bateria = bateriaAtiva ? 1 : 0;
  c.apiOk = 0xA5;
  snprintf(c.apiUrl, sizeof(c.apiUrl), "%s", apiUrl.c_str());
  snprintf(c.deviceKey, sizeof(c.deviceKey), "%s", deviceKey.c_str());
  c.deviceIdOk = 0xA5;
  snprintf(c.deviceId, sizeof(c.deviceId), "%s", apiDeviceId.c_str());
  c.agendaLiga = agendaLiga;
  c.agendaDesliga = agendaDesliga;
  EEPROM.put(0, c);
  EEPROM.commit();
}

String htmlEsc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t k = 0; k < s.length(); k++) {
    char c = s[k];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String formataHora(int minutos) {
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", minutos / 60, minutos % 60);
  return String(b);
}

// "HH:MM" -> minutos desde 00:00, ou -1 se inválido
int parseHora(const String& t) {
  int c = t.indexOf(':');
  if (c < 1 || !isDigit(t[0])) return -1;
  int h = t.substring(0, c).toInt();
  int m = t.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

void redirecionaConfig(const char* r) {
  const char* aba = "clima";
  if (!strcmp(r, "disp") || !strcmp(r, "agenda") || !strcmp(r, "hora") || !strcmp(r, "horaigual")) aba = "tela";
  else if (!strcmp(r, "bat")) aba = "bateria";
  server.sendHeader("Location", String("/config?aba=") + aba + "&r=" + r);
  server.send(303, "text/plain", "");
}

/***************************************************
* Informações do WiFi e do ESP8266 (página /config)
****************************************************/

// Ícone clássico de sinal: 4 barras crescentes, preenchidas conforme a qualidade (0-100%)
String iconeSinal(int pct) {
  int n = 0;
  if (pct >= 75) n = 4;
  else if (pct >= 50) n = 3;
  else if (pct >= 25) n = 2;
  else if (pct > 0) n = 1;

  String s = "<svg width=\"34\" height=\"22\" viewBox=\"0 0 34 22\" style=\"vertical-align:middle\">";
  for (int k = 0; k < 4; k++) {
    int h = 6 + k * 5;                  // alturas 6, 11, 16, 21
    s += "<rect x=\"" + String(k * 9) + "\" y=\"" + String(22 - h) + "\" width=\"7\" height=\"" + String(h) +
         "\" rx=\"1.5\" fill=\"" + (k < n ? "#81d4fa" : "#ffffff40") + "\"/>";
  }
  s += "</svg>";
  return s;
}

String linhaInfo(const char* rotulo, const String& valor) {
  return "<tr><td>" + String(rotulo) + "</td><td>" + valor + "</td></tr>";
}

String fmtBytes(uint32_t b) {
  if (b >= 1048576UL) return String(b / 1048576.0, 1) + " MB";
  return String(b / 1024UL) + " KB";
}

String formataUptime(unsigned long s) {
  unsigned long d = s / 86400UL;
  unsigned long h = (s % 86400UL) / 3600UL;
  unsigned long m = (s % 3600UL) / 60UL;
  unsigned long seg = s % 60UL;
  String o = "";
  if (d) o += String(d) + "d ";
  o += String(h) + "h " + String(m) + "min " + String(seg) + "s";
  return o;
}

// Ícone de bateria parecido com o de celular (SVG), cor muda conforme a carga
String iconeBateria(int pct) {
  const char* cor = pct <= BAT_CRITICA_PCT ? "#ef5350" : (pct <= BAT_AVISO_PCT ? "#ffb74d" : "#66bb6a");
  int w = (22 * pct) / 100;
  String s = "<svg width=\"34\" height=\"16\" viewBox=\"0 0 34 16\" style=\"vertical-align:middle\">"
             "<rect x=\"1\" y=\"1\" width=\"28\" height=\"14\" rx=\"3.5\" fill=\"none\" stroke=\"#ffffffcc\" stroke-width=\"2\"/>"
             "<rect x=\"4\" y=\"4\" width=\"" + String(w) + "\" height=\"8\" rx=\"1.5\" fill=\"" + cor + "\"/>"
             "<rect x=\"30\" y=\"5\" width=\"3\" height=\"6\" rx=\"1.2\" fill=\"#ffffffcc\"/></svg>";
  return s;
}

// Descrição da faixa de tensão, conforme a tabela da bateria
const char* bateriaSituacao(float v) {
  if (v >= 4.15) return "Totalmente carregada";
  if (v >= 4.06) return "Carga alta";
  if (v >= 3.98) return "Carga alta / boa";
  if (v >= 3.89) return "Carga média-alta";
  if (v >= 3.80) return "Carga média";
  if (v >= 3.72) return "Meia carga";
  if (v >= 3.65) return "Carga moderada";
  if (v >= 3.58) return "Carga baixa";
  if (v >= 3.39) return "Nível crítico (recomenda-se recarregar)";
  if (v >= 3.20) return "Muito baixa";
  return "Descarga profunda (risco de danificar a célula)";
}

String tabelaBateria() {
  String t = "<table>";
  if (!bateriaAtiva) {
    t += linhaInfo("Estado", "Desativada (ative na aba Bateria)");
  } else {
    const char* estado = bateriaPct <= BAT_CRITICA_PCT ? "⚠️ Crítica" : (bateriaPct <= BAT_AVISO_PCT ? "⚠️ Baixa" : "✅ Normal");
    t += linhaInfo("Carga", iconeBateria(bateriaPct) + " " + String(bateriaPct) + "%");
    t += linhaInfo("Tensão", String(bateriaV, 2) + " V");
    t += linhaInfo("Estado", estado);
    t += linhaInfo("Situação", bateriaSituacao(bateriaV));
  }
  t += "</table><div class=\"mut\">Estimativa pela tensão lida no A0. Ajuste BAT_CALIB no código comparando com um multímetro.</div>";
  return t;
}

String tabelaWiFi() {
  String t;
  t.reserve(1600);
  t = "<table>";

  bool conectado = (WiFi.status() == WL_CONNECTED);
  t += linhaInfo("Estado", conectado ? "✅ Conectado" : "⚠️ Desconectado");

  if (conectado) {
    int rssi = WiFi.RSSI();
    int pct = getRSSIasQuality(rssi);
    const char* nivel = pct >= 75 ? "Excelente" : (pct >= 50 ? "Bom" : (pct >= 25 ? "Regular" : "Fraco"));

    const char* phy = "?";
    switch (WiFi.getPhyMode()) {
      case WIFI_PHY_MODE_11B: phy = "802.11b"; break;
      case WIFI_PHY_MODE_11G: phy = "802.11g"; break;
      case WIFI_PHY_MODE_11N: phy = "802.11n"; break;
      default: break;
    }

    t += linhaInfo("Rede (SSID)", htmlEsc(WiFi.SSID()));
    t += linhaInfo("Sinal", iconeSinal(pct) + " " + String(rssi) + " dBm · " + String(pct) + "% · " + nivel);
    t += linhaInfo("BSSID (roteador)", WiFi.BSSIDstr());
    t += linhaInfo("Canal", String(WiFi.channel()));
    t += linhaInfo("Padrão WiFi", phy);
    t += linhaInfo("IP", WiFi.localIP().toString());
    t += linhaInfo("Máscara", WiFi.subnetMask().toString());
    t += linhaInfo("Gateway", WiFi.gatewayIP().toString());
    t += linhaInfo("DNS", WiFi.dnsIP().toString());
  }

  String hn = String(WiFi.hostname());
  t += linhaInfo("Hostname", htmlEsc(hn));
  t += linhaInfo("MAC (estação)", WiFi.macAddress());
  t += linhaInfo("MAC (AP)", WiFi.softAPmacAddress());
  t += linhaInfo("Reconexão automática", WiFi.getAutoReconnect() ? "Sim" : "Não");

  const char* sono = "?";
  switch (WiFi.getSleepMode()) {
    case WIFI_NONE_SLEEP: sono = "Desligado"; break;
    case WIFI_LIGHT_SLEEP: sono = "Light sleep"; break;
    case WIFI_MODEM_SLEEP: sono = "Modem sleep"; break;
    default: break;
  }
  t += linhaInfo("Economia de energia", sono);

  t += "</table>";
  return t;
}

String tabelaSistema() {
  String t;
  t.reserve(1800);
  t = "<table>";

  t += linhaInfo("Tempo ligado", formataUptime(millis() / 1000UL));
  t += linhaInfo("Último reinício", htmlEsc(ESP.getResetReason()));

  time_t agora = time(nullptr);
  if (agora > 100000) {
    struct tm* ti = localtime(&agora);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d", ti->tm_mday, ti->tm_mon + 1, ti->tm_year + 1900, ti->tm_hour, ti->tm_min, ti->tm_sec);
    t += linhaInfo("Data e hora (NTP)", buf);
  } else {
    t += linhaInfo("Data e hora (NTP)", "Sem sincronização");
  }

  String chip = String(ESP.getChipId(), HEX);
  chip.toUpperCase();
  String flashId = String(ESP.getFlashChipId(), HEX);
  flashId.toUpperCase();

  t += linhaInfo("Chip ID", chip);
  t += linhaInfo("CPU", String(ESP.getCpuFreqMHz()) + " MHz");
  t += linhaInfo("Flash (chip real)", fmtBytes(ESP.getFlashChipRealSize()));
  t += linhaInfo("Flash (config. da IDE)", fmtBytes(ESP.getFlashChipSize()));
  t += linhaInfo("Velocidade da flash", String(ESP.getFlashChipSpeed() / 1000000UL) + " MHz");
  t += linhaInfo("ID da flash", flashId);
  t += linhaInfo("Programa", fmtBytes(ESP.getSketchSize()) + " usados · " + fmtBytes(ESP.getFreeSketchSpace()) + " livres");
  t += linhaInfo("Memória livre (heap)", String(ESP.getFreeHeap()) + " bytes");
  t += linhaInfo("Maior bloco livre", String(ESP.getMaxFreeBlockSize()) + " bytes");
  t += linhaInfo("Fragmentação do heap", String(ESP.getHeapFragmentation()) + "%");
  t += linhaInfo("Firmware", FIRMWARE_VERSAO);
  t += linhaInfo("Sensor DHT22", sensorOk ? "OK" : "sem leitura válida (confira o dado no pino D5)");
  t += linhaInfo("Core ESP8266", htmlEsc(ESP.getCoreVersion()));
  t += linhaInfo("SDK", htmlEsc(String(ESP.getSdkVersion())));
  t += linhaInfo("Compilado em", String(__DATE__) + " " + String(__TIME__));

  t += "</table>";
  return t;
}

// Página de configuração dividida em abas: Clima, Tela, Bateria, WiFi e Sistema.
// Ações destrutivas ficam em POST + confirmação, sem auto-refresh.
static const char* const ABAS_ID[]   = {"clima", "tela", "bateria", "wifi", "sistema"};
static const char* const ABAS_NOME[] = {"Clima", "Tela", "Bateria", "WiFi", "Sistema"};
static const uint8_t ABAS_COUNT = 5;

String abaValida(const String& a) {
  for (uint8_t k = 0; k < ABAS_COUNT; k++) {
    if (a == ABAS_ID[k]) return a;
  }
  return "clima";
}

void paginaConfig() {
  String aba = abaValida(server.arg("aba"));
  String r = server.arg("r");
  String msg = "";
  if (r == "ok") msg = "✅ Configurações salvas. Atualizando o clima...";
  else if (r == "chave") msg = "⚠️ Chave inválida: use só letras e números (20 a 40 caracteres).";
  else if (r == "local") msg = "⚠️ ID da cidade inválido: informe o número (ou cole o endereço da cidade).";
  else if (r == "idioma") msg = "⚠️ Idioma inválido.";
  else if (r == "disp") msg = "✅ Tela atualizada.";
  else if (r == "agenda") msg = "✅ Horário salvo.";
  else if (r == "hora") msg = "⚠️ Horário inválido.";
  else if (r == "horaigual") msg = "⚠️ Os horários de ligar e de apagar não podem ser iguais.";
  else if (r == "fuso") msg = "⚠️ Fuso horário inválido.";
  else if (r == "bat") msg = "✅ Configuração da bateria salva.";
  else if (r == "api") msg = "✅ Dados do painel salvos.";
  else if (r == "apiurl") msg = "⚠️ URL inválida: use http:// ou https:// e não deixe espaços.";
  else if (r == "apiid") msg = "⚠️ ID do dispositivo inválido: use até 32 caracteres (letras, números, - _ .).";
  else if (r == "apikey") msg = "⚠️ DEVICE_KEY inválida: use de 8 a 64 caracteres, sem espaços.";
  else if (r == "envio_ok") msg = "✅ Dados enviados ao painel.";
  else if (r == "envio_erro") msg = "⚠️ Falha no envio: " + htmlEsc(apiUltimoMsg);

  // Resposta em partes (chunked): evita montar a página inteira na RAM
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String h;
  h.reserve(4500);
  h = "<!DOCTYPE html><html lang=\"pt-BR\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Configurações</title>"
      "<style>body{font-family:system-ui,sans-serif;max-width:480px;margin:1em auto;padding:0 1em;background:#0f2027;color:#fff}"
      "label{display:block;margin-top:1em;font-weight:600}"
      "input,select{width:100%;padding:.7em;margin-top:.3em;border:0;border-radius:10px;font-size:1rem}"
      "button{padding:.7em 1.2em;margin:.6em 0;border:0;border-radius:10px;font-size:1rem}"
      ".mut{color:#a9c1cf;font-size:.85rem;margin-top:.3em}a{color:#81d4fa}"
      "table{width:100%;border-collapse:collapse;margin:.4em 0}"
      "td{padding:.5em .3em;border-bottom:1px solid #ffffff22;vertical-align:middle;font-size:.9rem;word-break:break-word}"
      "td:first-child{color:#a9c1cf;width:42%}input[type=checkbox]{width:auto;margin-right:.6em}"
      ".back{display:inline-flex;align-items:center;gap:6px;padding:.4em .9em .4em .5em;border-radius:12px;background:#ffffff14;text-decoration:none;color:#fff;font-size:.9rem}"
      "h2 svg{vertical-align:-4px;margin-right:8px}"
      "nav{display:flex;flex-wrap:wrap;gap:6px;margin:.6em 0 1em}"
      "nav a{padding:.45em .9em;border-radius:999px;background:#ffffff14;text-decoration:none;color:#fff;font-size:.9rem}"
      "nav a.on{background:#81d4fa;color:#0f2027;font-weight:600}</style></head><body>"
      "<p><a class=\"back\" href=\"/\"><svg width=\"20\" height=\"20\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M15 5l-7 7 7 7\"/></svg>Voltar</a></p>"
      "<h2><svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\"><circle cx=\"12\" cy=\"12\" r=\"8.6\" stroke-width=\"3.4\" stroke-dasharray=\"3.377 3.377\"/><circle cx=\"12\" cy=\"12\" r=\"6.2\" stroke-width=\"2.4\"/><circle cx=\"12\" cy=\"12\" r=\"2.4\" stroke-width=\"2\"/></svg>Configurações</h2><nav>";
  for (uint8_t k = 0; k < ABAS_COUNT; k++) {
    h += "<a href=\"/config?aba=" + String(ABAS_ID[k]) + "\"";
    if (aba == ABAS_ID[k]) h += " class=\"on\"";
    h += ">" + String(ABAS_NOME[k]) + "</a>";
  }
  h += "</nav>";
  if (msg.length()) h += "<p>" + msg + "</p>";

  if (aba == "clima") {
    // ---------- Aba Clima: chave, cidade, idioma e fuso ----------
    String estado;
    if (weatherOk) estado = "✅ Clima recebido de " + htmlEsc(currentWeather.cityName);
    else estado = "⚠️ Sem dados do clima (confira a chave e o ID da cidade)";

    String masked = "cole a chave aqui";
    if (OPEN_WEATHER_MAP_APP_ID.length() >= 4) {
      masked = "•••••••• " + OPEN_WEATHER_MAP_APP_ID.substring(OPEN_WEATHER_MAP_APP_ID.length() - 4) + " (salva)";
    }

    h += "<h3>Clima (OpenWeatherMap)</h3><p class=\"mut\">" + estado + "</p>"
         "<form method=\"POST\" action=\"/salvar\">"
         "<label>Chave da API</label>"
         "<input name=\"apiKey\" type=\"text\" autocomplete=\"off\" autocapitalize=\"off\" placeholder=\"" + htmlEsc(masked) + "\">"
         "<div class=\"mut\">Deixe em branco para manter a atual. Crie a sua em "
         "<a href=\"https://home.openweathermap.org/api_keys\" target=\"_blank\" rel=\"noopener\">home.openweathermap.org/api_keys</a> "
         "(chaves novas podem levar um tempo para ativar).</div>"
         "<label>ID da cidade</label>"
         "<input name=\"locId\" type=\"text\" inputmode=\"numeric\" value=\"" + htmlEsc(OPEN_WEATHER_MAP_LOCATION_ID) + "\">"
         "<div class=\"mut\">Busque a cidade em <a href=\"https://openweathermap.org/find\" target=\"_blank\" rel=\"noopener\">openweathermap.org/find</a> "
         "e copie o número no fim do endereço (.../city/<b>3463011</b>). Também pode colar o endereço inteiro.</div>"
         "<label>Idioma</label><select name=\"lang\">";
    for (uint8_t k = 0; k < LANG_COUNT; k++) {
      h += "<option value=\"" + String(LANG_CODES[k]) + "\"";
      if (OPEN_WEATHER_MAP_LANGUAGE == LANG_CODES[k]) h += " selected";
      h += ">" + String(LANG_NAMES[k]) + "</option>";
    }
    server.sendContent(h);
    h = "";
    h += "</select>"
         "<label>Fuso horário</label><select name=\"fuso\">";
    for (uint8_t k = 0; k < FUSO_COUNT; k++) {
      h += "<option value=\"" + String(FUSOS[k].min) + "\"";
      if (FUSOS[k].min == fusoMin) h += " selected";
      h += ">" + String(FUSOS[k].nome) + "</option>";
    }
    h += "</select>"
         "<label><input type=\"checkbox\" name=\"verao\" value=\"1\"";
    if (horarioVerao) h += " checked";
    h += "> Horário de verão (+1 h)</label>"
         "<button type=\"submit\">Salvar e atualizar</button></form>";

    // ---------- Painel web (API): URL, DEVICE_KEY e envio imediato ----------
    server.sendContent(h);
    h = "";
    String maskedKey = "cole a DEVICE_KEY aqui";
    if (deviceKey.length() >= 4) {
      maskedKey = "•••••••• " + deviceKey.substring(deviceKey.length() - 4) + " (salva)";
    }
    String cidadeEnviada = currentWeather.cityName.length() ? htmlEsc(currentWeather.cityName) : String("(nome ainda não recebido)");
    cidadeEnviada += " · ID " + htmlEsc(OPEN_WEATHER_MAP_LOCATION_ID);
    String ultimo;
    if (!apiTentou) ultimo = "nenhum envio ainda";
    else ultimo = "há " + formataUptime((millis() - apiUltimoMs) / 1000UL) + " · " + (apiUltimoOk ? "✅ " : "⚠️ ") + htmlEsc(apiUltimoMsg);

    h += "<hr><h3>Painel web (API)</h3>"
         "<form method=\"POST\" action=\"/api\">"
         "<label>ID do dispositivo (device_id)</label>"
         "<input name=\"deviceId\" type=\"text\" autocapitalize=\"off\" value=\"" + htmlEsc(idDispositivo()) + "\">"
         "<div class=\"mut\">Precisa ser igual ao ID cadastrado no painel.</div>"
         "<label>URL da API</label>"
         "<input name=\"apiUrl\" type=\"text\" inputmode=\"url\" autocapitalize=\"off\" placeholder=\"https://seu-app.vercel.app/api/...\" value=\"" + htmlEsc(apiUrl) + "\">"
         "<label>DEVICE_KEY</label>"
         "<input name=\"deviceKey\" type=\"text\" autocomplete=\"off\" autocapitalize=\"off\" placeholder=\"" + htmlEsc(maskedKey) + "\">"
         "<div class=\"mut\">Deixe a chave em branco para manter a atual. Use https:// (a Vercel redireciona o http). "
         "Sem URL, o envio fica desligado.</div>"
         "<button type=\"submit\">Salvar dados do painel</button></form>"
         "<p class=\"mut\">Cidade enviada ao painel: " + cidadeEnviada + "</p>"
         "<p class=\"mut\">Envio automático a cada " + String(PAINEL_INTERVAL_MS / 60000UL) + " min. Último envio: " + ultimo + "</p>"
         "<form method=\"POST\" action=\"/enviar\">"
         "<button type=\"submit\">Enviar dados agora</button></form>";

  } else if (aba == "tela") {
    // ---------- Aba Tela: ligar/apagar e horário automático ----------
    h += "<h3>Tela OLED</h3>";
    h += displayLigado ? "<p class=\"mut\">Estado: ligada</p>" : "<p class=\"mut\">Estado: apagada</p>";
    h += "<form method=\"POST\" action=\"/display\">"
         "<input type=\"hidden\" name=\"estado\" value=\"t\"><input type=\"hidden\" name=\"volta\" value=\"1\">"
         "<button type=\"submit\">";
    h += displayLigado ? "Apagar tela" : "Ligar tela";
    h += "</button></form>";
    h += "<h4>Horário automático</h4>"
         "<form method=\"POST\" action=\"/agenda\">"
         "<label><input type=\"checkbox\" name=\"ativa\" value=\"1\"";
    if (agendaAtiva) h += " checked";
    h += "> Ligar e apagar a tela sozinha</label>"
         "<label>Ligar às</label><input type=\"time\" name=\"liga\" value=\"" + formataHora(agendaLiga) + "\">"
         "<label>Apagar às</label><input type=\"time\" name=\"desliga\" value=\"" + formataHora(agendaDesliga) + "\">"
         "<div class=\"mut\">O botão manual vale até o próximo horário programado. "
         "Depende da hora da internet (NTP), então só funciona depois de sincronizar.</div>"
         "<button type=\"submit\">Salvar horário</button></form>";

  } else if (aba == "bateria") {
    // ---------- Aba Bateria: ativar/desativar e estado ----------
    h += "<h3>Bateria</h3>"
         "<form method=\"POST\" action=\"/bateria\">"
         "<label><input type=\"checkbox\" name=\"ativa\" value=\"1\"";
    if (bateriaAtiva) h += " checked";
    h += "> Este ESP tem bateria (leitura no A0)</label>"
         "<div class=\"mut\">Desmarcado, a leitura, o ícone e os avisos de bateria ficam desligados. "
         "Marque só se a bateria estiver ligada ao A0 pelo divisor de tensão.</div>"
         "<button type=\"submit\">Salvar</button></form>";
    if (bateriaAtiva) h += tabelaBateria();

  } else if (aba == "wifi") {
    // ---------- Aba WiFi: dados da conexão e ações ----------
    h += "<h3>WiFi</h3>";
    h += tabelaWiFi();
    h += "<form method=\"POST\" action=\"/portal\" onsubmit=\"return confirm('Abrir o portal de configuração? A estação reinicia ao terminar.')\">"
         "<button type=\"submit\">Abrir portal de configuração</button></form>"
         "<form method=\"POST\" action=\"/reset-wifi\" onsubmit=\"return confirm('Apagar a rede WiFi salva e reiniciar?')\">"
         "<button type=\"submit\">Apagar dados de WiFi</button></form>";

  } else {
    // ---------- Aba Sistema: informações do ESP8266 ----------
    h += "<h3>Informações do ESP8266</h3>";
    h += tabelaSistema();
  }

  h += "</body></html>";
  server.sendContent(h);
  server.sendContent("");               // encerra a resposta (chunked)
}

void handleSalvar() {
  String key = server.arg("apiKey");
  key.trim();
  String lang = server.arg("lang");
  String locRaw = server.arg("locId");

  // ID da cidade: usa a última sequência de dígitos (aceita colar o endereço inteiro)
  String loc = "";
  int e = (int)locRaw.length() - 1;
  while (e >= 0 && !isDigit(locRaw[e])) e--;
  int b = e;
  while (b >= 0 && isDigit(locRaw[b])) b--;
  if (e >= 0) loc = locRaw.substring(b + 1, e + 1);

  if (key.length() > 0) {
    bool okKey = key.length() >= 20 && key.length() <= 40;
    for (size_t k = 0; k < key.length(); k++) {
      if (!isAlphaNumeric(key[k])) okKey = false;
    }
    if (!okKey) { redirecionaConfig("chave"); return; }
  }
  if (loc.length() < 1 || loc.length() > 10) { redirecionaConfig("local"); return; }

  bool okLang = false;
  for (uint8_t k = 0; k < LANG_COUNT; k++) {
    if (lang == LANG_CODES[k]) okLang = true;
  }
  if (!okLang) { redirecionaConfig("idioma"); return; }

  // Fuso horário (se o formulário não trouxer o campo, mantém o atual)
  bool trocaFuso = server.hasArg("fuso");
  int fuso = fusoMin;
  if (trocaFuso) {
    fuso = server.arg("fuso").toInt();
    bool okFuso = false;
    for (uint8_t k = 0; k < FUSO_COUNT; k++) {
      if (FUSOS[k].min == fuso) okFuso = true;
    }
    if (!okFuso) { redirecionaConfig("fuso"); return; }
  }

  if (key.length() > 0) OPEN_WEATHER_MAP_APP_ID = key;   // em branco = mantém a atual
  OPEN_WEATHER_MAP_LOCATION_ID = loc;
  OPEN_WEATHER_MAP_LANGUAGE = lang;
  if (trocaFuso) {
    fusoMin = fuso;
    horarioVerao = server.hasArg("verao");
    aplicaFuso();                        // reinicia o NTP com o novo fuso
    agendaUltimoEstado = -1;             // a agenda da tela reavalia com a nova hora
  }
  currentWeatherClient.setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  forecastClient.setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  salvaConfig();

  nextWeatherUpdate = millis();          // o loop() atualiza o clima em seguida
  redirecionaConfig("ok");
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t k = 0; k < s.length(); k++) {
    char c = s[k];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { o += ' '; }
    else { o += c; }
  }
  return o;
}

void paginaJson() {
  float windKmh = IS_METRIC ? currentWeather.windSpeed * 3.6 : currentWeather.windSpeed * 1.609;
  String j;
  j.reserve(800);
  j += "{\"unid\":\"";
  j += IS_METRIC ? "°C" : "°F";
  j += "\",\"erro\":";
  j += weatherOk ? "false" : "true";
  j += ",\"display\":";
  j += displayLigado ? "true" : "false";
  j += ",\"bateria\":";
  if (bateriaAtiva) j += "{\"v\":" + String(bateriaV, 2) + ",\"pct\":" + String(bateriaPct) + "}";
  else j += "null";
  j += ",\"interno\":{";
  if (sensorOk) {
    j += "\"temp\":" + String(localTemp, 1) + ",\"umid\":" + String(localHum, 1);
  } else {
    j += "\"temp\":null,\"umid\":null";
  }
  j += "},\"externo\":{";
  j += "\"cidade\":\"" + jsonEscape(currentWeather.cityName) + "\",";
  j += "\"desc\":\"" + jsonEscape(currentWeather.description) + "\",";
  j += "\"icone\":\"" + jsonEscape(currentWeather.icon) + "\",";
  j += "\"temp\":" + String(currentWeather.temp, 1) + ",";
  j += "\"min\":" + String(currentWeather.tempMin, 1) + ",";
  j += "\"max\":" + String(currentWeather.tempMax, 1) + ",";
  j += "\"umid\":" + String(currentWeather.humidity) + ",";
  j += "\"pressao\":" + String(currentWeather.pressure) + ",";
  j += "\"vento\":" + String(windKmh, 0) + ",";
  j += "\"ventoDir\":" + String(currentWeather.windDeg, 0) + ",";
  j += "\"visib\":" + String(currentWeather.visibility / 1000.0, 1) + ",";
  j += "\"nuvens\":" + String(currentWeather.clouds) + ",";
  j += "\"nascer\":" + String((unsigned long)currentWeather.sunrise) + ",";
  j += "\"por\":" + String((unsigned long)currentWeather.sunset);
  j += "},\"previsao\":[";
  for (uint8_t k = 0; k < MAX_FORECASTS; k++) {
    if (k) j += ",";
    j += "{\"dia\":" + String((unsigned long)forecasts[k].observationTime) + ",";
    j += "\"icone\":\"" + jsonEscape(forecasts[k].icon) + "\",";
    j += "\"temp\":" + String(forecasts[k].temp, 1) + "}";
  }
  j += "],\"rssi\":" + String(WiFi.RSSI()) + ",\"rssiPct\":" + String(getRSSIasQuality(WiFi.RSSI())) + "}";
  server.send(200, "application/json", j);
}

// Histórico interno (do mais antigo para o mais novo)
void paginaHistorico() {
  String j;
  j.reserve(64 + histCount * 12);
  long prox = (long)(nextHist - millis()) / 1000;
  if (prox < 0) prox = 0;
  j += "{\"ok\":";
  j += sensorOk ? "true" : "false";
  j += ",\"proxima\":" + String(prox);
  j += ",\"intervalo\":" + String(HIST_INTERVAL_MS / 1000UL) + ",\"temp\":[";
  uint8_t start = (histCount < HIST_SIZE) ? 0 : histHead;
  for (uint8_t k = 0; k < histCount; k++) {
    if (k) j += ",";
    j += String(histTemp[(start + k) % HIST_SIZE] / 10.0, 1);
  }
  j += "],\"umid\":[";
  for (uint8_t k = 0; k < histCount; k++) {
    if (k) j += ",";
    j += String(histHum[(start + k) % HIST_SIZE] / 10.0, 1);
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void guardaHistorico() {
  histTemp[histHead] = (int16_t)roundf(localTemp * 10);
  histHum[histHead] = (uint16_t)roundf(localHum * 10);
  histHead = (histHead + 1) % HIST_SIZE;
  if (histCount < HIST_SIZE) histCount++;
}

/***************************************************
* Envio das leituras ao painel web (API REST)
* URL e DEVICE_KEY vêm da /config (salvas na flash)
****************************************************/

// Corpo JSON enviado ao painel. Os nomes dos campos ficam todos aqui, fáceis de ajustar.
// O horário da leitura não vai no corpo: o servidor deve registrar a hora em que recebe.
String idDispositivo() {
  if (apiDeviceId.length() > 0) return apiDeviceId;
  String id = String(ESP.getChipId(), HEX);
  id.toLowerCase();
  return "esp8266-" + id;               // padrão, se nenhum ID foi salvo em /config
}

String montaPayloadApi() {
  String j;
  j.reserve(360);
  j += "{\"device_id\":\"" + jsonEscape(idDispositivo()) + "\"";
  j += ",\"temperature\":" + String(localTemp, 1);
  j += ",\"humidity\":" + String(localHum, 1);
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"rssi\":" + String(WiFi.RSSI());
  String cid = OPEN_WEATHER_MAP_LOCATION_ID;           // código da cidade (OpenWeatherMap), enviado como número
  bool soDigitos = cid.length() > 0;
  for (size_t k = 0; k < cid.length(); k++) {
    if (!isDigit(cid[k])) soDigitos = false;
  }
  j += ",\"city_id\":";
  j += soDigitos ? cid : String("null");
  j += ",\"city_name\":\"" + jsonEscape(currentWeather.cityName) + "\"";
  j += ",\"firmware\":\"" FIRMWARE_VERSAO "\"";
  j += ",\"battery_active\":";
  j += bateriaAtiva ? "true" : "false";
  j += ",\"battery\":" + String(bateriaAtiva ? bateriaPct : 0);
  j += ",\"battery_voltage\":" + String(bateriaAtiva ? bateriaV : 0.0f, 2);
  j += "}";
  return j;
}

// Envia uma leitura. Devolve true se o servidor respondeu 2xx. O resultado fica em apiUltimoMsg.
bool enviaParaApiInterno() {
  apiTentou = true;
  apiUltimoMs = millis();
  apiUltimoOk = false;

  if (apiUrl.length() == 0 || deviceKey.length() == 0) {
    apiUltimoMsg = "Configure a URL da API e a DEVICE_KEY";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    apiUltimoMsg = "Sem WiFi";
    return false;
  }
  if (!sensorOk) {
    apiUltimoMsg = "Sensor DHT22 sem leitura válida";
    return false;
  }

  bool https = apiUrl.startsWith("https://");
  if (https && ESP.getFreeHeap() < 16000) {
    apiUltimoMsg = "Memória insuficiente para HTTPS (" + String(ESP.getFreeHeap()) + " bytes livres)";
    return false;
  }

  String corpo = montaPayloadApi();

  WiFiClient clientPlain;
  WiFiClientSecure clientTls;
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);

  bool iniciou;
  if (https) {
    clientTls.setInsecure();                          // não valida o certificado do servidor
    clientTls.setBufferSizes(API_TLS_RX_BUF, 512);    // economiza RAM no HTTPS
    iniciou = http.begin(clientTls, apiUrl);
  } else {
    iniciou = http.begin(clientPlain, apiUrl);
  }
  if (!iniciou) {
    apiUltimoMsg = "URL inválida";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", deviceKey);          // DEVICE_KEY configurada em /config

  int codigo = http.POST(corpo);                      // > 0: código HTTP, < 0: erro de conexão
  if (codigo > 0) apiUltimoMsg = "HTTP " + String(codigo);
  else apiUltimoMsg = HTTPClient::errorToString(codigo);
  http.end();

  apiUltimoOk = (codigo >= 200 && codigo < 300);
  Serial.println("API -> " + apiUltimoMsg);
  return apiUltimoOk;
}

// Envolve o envio com o LED: pisca rápido enquanto envia; no fim, 1 acendida (ok) ou 3 piscadas (falha)
bool enviaParaApi() {
  ledEnviando = true;
  bool ok = enviaParaApiInterno();
  ledEnviando = false;
  if (ok) ledPulsoOk(); else ledPulsoFalha();
  return ok;
}

// Salva ID, URL e DEVICE_KEY. URL em branco desliga o envio; chave em branco mantém a atual; ID em branco volta ao padrão.
void handleApi() {
  String url = server.arg("apiUrl");
  url.trim();
  String key = server.arg("deviceKey");
  key.trim();
  String id = server.arg("deviceId");
  id.trim();

  if (url.length() > 0) {
    bool ok = (url.startsWith("http://") || url.startsWith("https://")) && url.length() <= 120;
    for (size_t k = 0; k < url.length(); k++) {
      if (url[k] < 33 || url[k] > 126) ok = false;    // sem espaços nem caracteres de controle
    }
    if (!ok) { redirecionaConfig("apiurl"); return; }
  }
  if (key.length() > 0) {
    bool ok = key.length() >= 8 && key.length() <= 64;
    for (size_t k = 0; k < key.length(); k++) {
      char c = key[k];
      if (c < 33 || c > 126 || c == '"' || c == '\\') ok = false;
    }
    if (!ok) { redirecionaConfig("apikey"); return; }
  }

  if (id.length() > 0) {
    bool ok = id.length() <= 32;
    for (size_t k = 0; k < id.length(); k++) {
      char c = id[k];
      if (!(isAlphaNumeric(c) || c == '-' || c == '_' || c == '.')) ok = false;
    }
    if (!ok) { redirecionaConfig("apiid"); return; }
  }

  apiUrl = url;
  apiDeviceId = id;
  if (key.length() > 0) deviceKey = key;
  salvaConfig();
  nextApi = millis() + 3000UL;           // primeiro envio logo depois de salvar
  redirecionaConfig("api");
}

// Botão "Enviar dados agora": não espera o próximo envio automático
void handleEnviar() {
  bool ok = enviaParaApi();
  if (ok) nextApi = millis() + PAINEL_INTERVAL_MS;       // recomeça o ciclo do envio automático
  redirecionaConfig(ok ? "envio_ok" : "envio_erro");
}

// Liga/desliga a leitura da bateria (A0). Salvo na EEPROM.
void handleBateria() {
  bateriaAtiva = server.hasArg("ativa");
  if (bateriaAtiva) {
    bateriaV = 0;                        // recomeça a média
    atualizaBateria();
    nextBateria = millis() + 30000UL;
  }
  salvaConfig();
  redirecionaConfig("bat");
}

// Aplica a agenda: só age quando a janela muda (liga/apaga nos horários).
// Assim o botão manual vale até o próximo horário programado.
void verificaAgenda() {
  if (!agendaAtiva || agendaLiga == agendaDesliga) return;
  time_t agora = time(nullptr);
  if (agora < 100000) return;            // ainda sem hora da internet
  struct tm* ti = localtime(&agora);
  int m = ti->tm_hour * 60 + ti->tm_min;
  bool ligada = (agendaLiga < agendaDesliga) ? (m >= agendaLiga && m < agendaDesliga)
                                             : (m >= agendaLiga || m < agendaDesliga);   // janela que cruza a meia-noite
  int8_t estado = ligada ? 1 : 0;
  if (estado != agendaUltimoEstado) {
    agendaUltimoEstado = estado;
    displayLigado = ligada;
    aplicaDisplay();
  }
}

void handleAgenda() {
  bool ativa = server.hasArg("ativa");
  int liga = parseHora(server.arg("liga"));
  int desliga = parseHora(server.arg("desliga"));
  if (liga < 0 || desliga < 0) { redirecionaConfig("hora"); return; }
  if (ativa && liga == desliga) { redirecionaConfig("horaigual"); return; }

  agendaAtiva = ativa;
  agendaLiga = liga;
  agendaDesliga = desliga;
  agendaUltimoEstado = -1;               // reavalia já: a tela passa a seguir a agenda agora
  salvaConfig();
  nextAgenda = millis();
  redirecionaConfig("agenda");
}

// Liga/apaga a tela OLED. estado = 1 (liga), 0 (apaga) ou t (alterna). Salvo na EEPROM.
void aplicaDisplay() {
  if (displayLigado) display.displayOn();
  else display.displayOff();
}

void handleDisplay() {
  String v = server.arg("estado");
  if (v == "1") displayLigado = true;
  else if (v == "0") displayLigado = false;
  else displayLigado = !displayLigado;
  aplicaDisplay();
  salvaConfig();
  if (server.hasArg("volta")) {
    redirecionaConfig("disp");
  } else {
    server.send(200, "application/json", displayLigado ? "{\"display\":true}" : "{\"display\":false}");
  }
}

void handleResetWiFi() {
  server.send(200, "text/html", "<meta charset=\"utf-8\"><p>Dados de WiFi apagados. Reiniciando...</p>");
  delay(500);
  wifiManager.resetSettings();
  ESP.restart();
}

void handlePortal() {
  server.send(200, "text/html", "<meta charset=\"utf-8\"><p>Conecte-se à rede ESP_WeatherStation para configurar. A estação reinicia ao terminar.</p>");
  delay(500);
  server.stop();                        // o portal usa a porta 80
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.startConfigPortal("ESP_WeatherStation");
  ESP.restart();
}
