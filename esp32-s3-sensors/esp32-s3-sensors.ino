#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cSht4x.h> 
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_system.h>

Preferences  prefs;
DNSServer    dnsServer;
WebServer    server(80);
const char*  apSSID = "ESP32S3-Setup";
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

/* - - - - - Global vars for settings storage - - - - - */
String s_ssid;  // Wi-Fi AP SSID
String s_pass;  // Wi-Fi AP password
String o_host;  // Name for the OTA host
String o_pass;  // OTA host password
String m_host;  // MQTT server host IP
int    m_port;  // MQTT server host port
String m_id;    // MQTT Client ID
String m_user;  // MQTT user
String m_pass;  // MQTT password
String m_path;  // MQTT path

/* - - - - - HW & pins - - - - - */
// MH-Z19C
#define MHZ_RX 15
#define MHZ_TX 16

// ZH03B
#define ZH_RX  17
#define ZH_TX  18

// SHT-40
#define SDA_PIN 8
#define SCL_PIN 7

HardwareSerial mhz19(1);  // UART1
HardwareSerial zh03b(2);  // UART2
SensirionI2cSht4x sht4x;  // I2C


volatile bool     bootPressed   = false;
volatile uint32_t bootPressTick = 0;
bool              bootPressHandled = false;
bool              otaWindowActive  = false;
uint32_t          otaWindowUntil   = 0;

const uint32_t OTA_TRIGGER_MS     = 1500;
const uint32_t OTA_WINDOW_MS      = 1UL * 60UL * 1000UL;
const uint32_t FACTORY_RESET_MS   = 5000;

const uint32_t PUBLISH_INTERVAL_MS = 15000;
const uint64_t PUBLISH_INTERVAL_US = (uint64_t)PUBLISH_INTERVAL_MS * 1000ULL;


/* - - - - - RGB LED helpers - - - - - */
static void rgbSet(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(LED_BUILTIN, r, g, b);
}

static void rgbOff() {
  rgbSet(0, 0, 0);
}

static void rgbGreenOn() {
  rgbSet(0, 64, 0);
}

static void rgbBlueFlash() {
  rgbSet(0, 0, 64);
  delay(30);
  if (otaWindowActive) {
    rgbGreenOn();
  } else {
    rgbOff();
  }
}

static void rgbRedFlash() {
  rgbSet(64, 0, 0);
  delay(120);
  rgbOff();
}


/* - - - - - AP and Wi-Fi connection stuff - - - - - */
// "Captive" web-page
void handleRoot() {
  String html = F(
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 connection setup</title></head><body>"
    "<h1>ESP32 connection setup</h1><p>Fill settings</p>"
    "<form action='/save' method='POST'>"
    "SSID: <input name='s' required><br><br>"
    "Pass: <input type='password' name='p' required><br><br>"
    "<hr><br>"
    "OTA  Host: <input name='oh' required><br><br>"
    "OTA  Pass: <input type='password' name='op' required><br><br>"
    "<hr><br>"
    "MQTT Host: <input name='mh' required><br><br>"
    "MQTT Port: <input name='mp' required><br><br>"
    "MQTT ID:   <input name='mid' required><br><br>"
    "MQTT User: <input name='musr' required><br><br>"
    "MQTT Pass: <input type='password' name='mpsw' required><br><br>"
    "MQTT Path: <input name='mpath' required><br><br>"
    "<button>Save & Connect</button></form></body></html>");
  server.send(200, "text/html", html);
}

// Save input data
void handleSave() {
  prefs.begin("connections", false);
    prefs.putString("ssid",  server.arg("s"));
    prefs.putString("pass",  server.arg("p"));
    prefs.putString("ohost", server.arg("oh"));
    prefs.putString("opass", server.arg("op"));
    prefs.putString("mhost", server.arg("mh"));
    prefs.putInt   ("mport", server.arg("mp").toInt());
    prefs.putString("mid",   server.arg("mid"));
    prefs.putString("muser", server.arg("musr"));
    prefs.putString("mpass", server.arg("mpsw"));
    prefs.putString("mpath", server.arg("mpath"));
  prefs.end();
  server.send(200, "text/html", "<h1>Saved<br>Rebooting...</h1>");
  delay(2000);
  ESP.restart();
}

// AP stuff
void startSetupAP() {
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1),
                    IPAddress(192,168,4,1),
                    IPAddress(255,255,255,0));
  WiFi.softAP(apSSID);

  dnsServer.start(53, "*", IPAddress(192,168,4,1));

  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/save",      HTTP_POST, handleSave);
  server.onNotFound(handleRoot);

  server.begin();
#ifdef DEBUG
  Serial.printf("SETUP AP \"%s\", URL: http://192.168.4.1\n", apSSID);
#endif
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(100);
  }
}



/* - - - - - Sensors read & send data - - - - - */
// Send to MQTT server
void publishData(int co2, int pm1, int pm25, int pm10, float t, float h) {
  StaticJsonDocument<256> doc;
  doc["co2"]  = co2;
  doc["pm1"]  = pm1;
  doc["pm25"] = pm25;
  doc["pm10"] = pm10;
  doc["temp"] = t;
  doc["hum"]  = h;
  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish(m_path.c_str(), buf);
}

// Read MH-Z19C CO2 value (ppm)
int readCO2() {
  uint8_t mhzCmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  uint8_t resp[9];
  while (mhz19.available()) mhz19.read();
  mhz19.write(mhzCmd, 9);
  unsigned long t0 = millis();
  while (mhz19.available() < 9) {
    if (millis() - t0 > 1000) return -1;
    delay(2);
  }
  for (int i = 0; i < 9; i++) resp[i] = mhz19.read();
  if (resp[0] != 0xFF || resp[1] != 0x86) return -2;
  return (resp[2] << 8) | resp[3];
}

// Read ZH03B PM1.0, PM2.5, PM10 values (mkg/m3)
bool readZH03B(uint16_t &pm1, uint16_t &pm25, uint16_t &pm10) {
  static uint8_t buf[24];
  while (zh03b.available() >= 24) {
    if (zh03b.read() != 0x42) continue;
    if (zh03b.read() != 0x4D) continue;
    buf[0] = 0x42;
    buf[1] = 0x4D;
    for (int i = 2; i < 24; i++) {
      buf[i] = zh03b.read();
    }
    uint16_t sum = 0;
    for (int i = 0; i < 22; i++) sum += buf[i];
    uint16_t checksum = (buf[22] << 8) | buf[23];
    if (sum != checksum) continue;
    pm1  = (buf[10] << 8) | buf[11];
    pm25 = (buf[12] << 8) | buf[13];
    pm10 = (buf[14] << 8) | buf[15];
    return true;
  }
  return false;
}

// Read SHT40 temperature and humidity values (C, RH%)
bool readSHT40(float &temp, float &hum) {
  uint16_t error = sht4x.measureMediumPrecision(temp, hum);
  if (error) {
    return false;
  }
  return true;
}



/* - - - - - WiFi and MQTT connection helpers - - - - - */
void connectWiFi() {
  static uint32_t lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();
  WiFi.begin(s_ssid.c_str(), s_pass.c_str());
}

void connectMQTT() {
  static uint32_t lastAttempt = 0;
  if (mqtt.connected()) return;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();
  mqtt.connect(m_id.c_str(), m_user.c_str(), m_pass.c_str());
}


/* - - - - - "BOOT" button helpers - - - - - */
void IRAM_ATTR bootButtonISR() {
  if (digitalRead(0) == LOW) {
    bootPressed   = true;
    bootPressTick = xTaskGetTickCountFromISR();
  } else {
    bootPressed = false;
  }
}

void startOtaWindow() {
  if (!otaWindowActive) {
    ArduinoOTA.begin();
    otaWindowActive = true;
    rgbGreenOn();
#ifdef DEBUG
    Serial.println("OTA window started");
#endif
  }
  otaWindowUntil = millis() + OTA_WINDOW_MS;
}

void stopOtaWindow() {
  if (!otaWindowActive) return;
  ArduinoOTA.end();
  otaWindowActive = false;
  rgbOff();
#ifdef DEBUG
  Serial.println("OTA window ended");
#endif
}



/* - - - - - SETUP - - - - - */
void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  rgbLedWrite(LED_BUILTIN, 0, 0, 0);
  attachInterrupt(0, bootButtonISR, CHANGE);
  delay(1000);
  setCpuFrequencyMhz(80);
  btStop();
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    startOtaWindow();
  }


// Load saved config into global variables
  prefs.begin("connections", true);
    s_ssid = prefs.getString("ssid", "");
    s_pass = prefs.getString("pass", "");
    o_host = prefs.getString("ohost", "");
    o_pass = prefs.getString("opass", "");
    m_host = prefs.getString("mhost", "");
    m_port = prefs.getInt   ("mport", 1883);
    m_id   = prefs.getString("mid", "");
    m_user = prefs.getString("muser", "");
    m_pass = prefs.getString("mpass", "");
    m_path = prefs.getString("mpath", "");
  prefs.end();

// No config -> start AP and configure
  if (s_ssid.length() == 0) {
#ifdef DEBUG
    Serial.println("No saved config, starting setup AP");
#endif
    startSetupAP();
  }

// Config exists -> try to connect to WiFi AP
#ifdef DEBUG
  Serial.printf("Connecting to WiFi: %s\n", s_ssid.c_str());
#endif
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.begin(s_ssid.c_str(), s_pass.c_str());

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
#ifdef DEBUG
    Serial.print(".");
#endif
  }
  Serial.println();

// Connection failed -> wrong config? Setup again
  if (WiFi.status() != WL_CONNECTED) {
#ifdef DEBUG
    Serial.println("WiFi failed, setup AP");
#endif
    startSetupAP();
  }
#ifdef DEBUG
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
#endif

// OTA
  ArduinoOTA.setHostname(o_host.c_str());
  ArduinoOTA.setPassword(o_pass.c_str());
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setRebootOnSuccess(true);

#ifdef DEBUG
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA End");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
  });
#endif

// Sensors
  mhz19.begin(9600, SERIAL_8N1, MHZ_RX, MHZ_TX);
  zh03b.begin(9600, SERIAL_8N1, ZH_RX, ZH_TX);

  Wire.setClock(100000);
  Wire.begin(SDA_PIN, SCL_PIN);
  sht4x.begin(Wire, SHT40_I2C_ADDR_44);

#ifdef DEBUG
  uint32_t serialNo = 0;
  uint16_t error = sht4x.serialNumber(serialNo);
  if (error) {
    Serial.print("SHT40 Serial number error: ");
    Serial.println(error);
  } else {
    Serial.print("SHT40 detected – Serial number: 0x");
    Serial.println(serialNo, HEX);
  }

  Serial.println("MH-Z19C + ZH03B + SHT40 started");
#endif

  mqtt.setServer(m_host.c_str(), m_port);
  mqtt.setKeepAlive(60);
  connectMQTT();
}



/* - - - - - LOOP - - - - - */
void loop() {
  if (otaWindowActive) {
    ArduinoOTA.handle();
    if ((int32_t)(millis() - otaWindowUntil) >= 0) {
      stopOtaWindow();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    connectMQTT();
  } else {
    mqtt.loop();
  }

// Reset device with pressing BOOT button for >5s
  if (bootPressed) {
    uint32_t heldMs = (xTaskGetTickCount() - bootPressTick) * portTICK_PERIOD_MS;
    if (heldMs > FACTORY_RESET_MS) {
#ifdef DEBUG
      rgbRedFlash();
      Serial.println("Reset!");
#endif
      WiFi.disconnect(true, true);
      prefs.begin("connections", false);
        prefs.clear();
      prefs.end();
      delay(200);
      ESP.restart();
    }
    else if (!bootPressHandled && heldMs > OTA_TRIGGER_MS) {
      startOtaWindow();
      bootPressHandled = true;
    }
  } else {
    bootPressHandled = false;
  }

// Read sensors measurements
  if (mqtt.connected()) {
    int co2 = readCO2();
    float temperature=0, humidity=0;
    readSHT40(temperature, humidity);
    uint16_t pm1=0, pm25=0, pm10=0;
    readZH03B(pm1, pm25, pm10);

    StaticJsonDocument<256> doc;
    doc["co2"]  = co2 > 0 ? co2 : 0;
    doc["temp"] = temperature;
    doc["hum"]  = humidity;
    doc["pm1"]  = pm1;
    doc["pm25"] = pm25;
    doc["pm10"] = pm10;

    // Publish
    char buf[256];
    serializeJson(doc, buf);
    bool sent = mqtt.publish(m_path.c_str(), buf);

#ifdef DEBUG
    Serial.print("Published: ");
    Serial.println(buf);
    if (!sent) {
      Serial.println("MQTT publish failed!");
    }
    rgbBlueFlash();
#endif

    if (!otaWindowActive) {
      mqtt.disconnect();
      WiFi.mode(WIFI_OFF);
      esp_wifi_stop();
      delay(50);
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
      esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
      esp_sleep_enable_timer_wakeup(PUBLISH_INTERVAL_US);
      esp_light_sleep_start();
      esp_wifi_start();
      WiFi.mode(WIFI_STA);
      WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    }
  }
}
