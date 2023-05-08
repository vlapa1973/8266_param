#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>

const char* ssidAP = "ESP_Relay";
const char* passwordAP = "Pa$$w0rd";

const byte pinBuiltinLed = 2; // Change BLED GPIO according to your board

const char configSign[4] = { '#', 'R', 'E', 'L' };
const byte maxStrParamLength = 32;

const char* ssidArg = "ssid";
const char* passwordArg = "password";
const char* domainArg = "domain";
const char* serverArg = "server";
const char* portArg = "port";
const char* userArg = "user";
const char* mqttpswdArg = "mqttpswd";
const char* clientArg = "client";
const char* topicArg = "topic";
const char* gpioArg = "gpio";
const char* levelArg = "level";
const char* onbootArg = "onboot";
const char* rebootArg = "reboot";

String ssid, password, domain;
String mqttServer, mqttUser, mqttPassword, mqttClient = "ESP_Relay", mqttTopic = "/Relay";
uint16_t mqttPort = 1883;
byte relayPin = 5;
bool relayLevel = LOW;
bool relayOnBoot = false;

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiClient espClient;
PubSubClient pubsubClient(espClient);

/*
 * EEPROM configuration functions
 */

uint16_t readEEPROMString(uint16_t offset, String& str) {
  char buffer[maxStrParamLength + 1];

  buffer[maxStrParamLength] = 0;
  for (byte i = 0; i < maxStrParamLength; i++) {
    if (! (buffer[i] = EEPROM.read(offset + i)))
      break;
  }
  str = String(buffer);

  return offset + maxStrParamLength;
}

uint16_t writeEEPROMString(uint16_t offset, const String& str) {
  for (byte i = 0; i < maxStrParamLength; i++) {
    if (i < str.length())
      EEPROM.write(offset + i, str[i]);
    else
      EEPROM.write(offset + i, 0);
  }

  return offset + maxStrParamLength;
}


bool readConfig() {
  uint16_t offset = 0;

  Serial.println("Reading config from EEPROM");
  for (byte i = 0; i < sizeof(configSign); i++) {
    if (EEPROM.read(offset + i) != configSign[i])
      return false;
  }
  offset += sizeof(configSign);
  offset = readEEPROMString(offset, ssid);
  offset = readEEPROMString(offset, password);
  offset = readEEPROMString(offset, domain);
  offset = readEEPROMString(offset, mqttServer);
  EEPROM.get(offset, mqttPort);
  offset += sizeof(mqttPort);
  offset = readEEPROMString(offset, mqttUser);
  offset = readEEPROMString(offset, mqttPassword);
  offset = readEEPROMString(offset, mqttClient);
  offset = readEEPROMString(offset, mqttTopic);
  EEPROM.get(offset, relayPin);
  offset += sizeof(relayPin);
  EEPROM.get(offset, relayLevel);
  offset += sizeof(relayLevel);
  EEPROM.get(offset, relayOnBoot);

  return true;
}

void writeConfig() {
  uint16_t offset = 0;

  Serial.println("Writing config to EEPROM");
  EEPROM.put(offset, configSign);
  offset += sizeof(configSign);
  offset = writeEEPROMString(offset, ssid);
  offset = writeEEPROMString(offset, password);
  offset = writeEEPROMString(offset, domain);
  offset = writeEEPROMString(offset, mqttServer);
  EEPROM.put(offset, mqttPort);
  offset += sizeof(mqttPort);
  offset = writeEEPROMString(offset, mqttUser);
  offset = writeEEPROMString(offset, mqttPassword);
  offset = writeEEPROMString(offset, mqttClient);
  offset = writeEEPROMString(offset, mqttTopic);
  EEPROM.put(offset, relayPin);
  offset += sizeof(relayPin);
  EEPROM.put(offset, relayLevel);
  offset += sizeof(relayLevel);
  EEPROM.put(offset, relayOnBoot);
  EEPROM.commit();
}

/*
 * MQTT functions
 */

bool mqtt_subscribe(PubSubClient& client, const String& topic) {
  Serial.print("Subscribing to ");
  Serial.println(topic);

  return client.subscribe(topic.c_str());
}

bool mqtt_publish(PubSubClient& client, const String& topic, const String& value) {
  Serial.print("Publishing topic ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(value);

  return client.publish(topic.c_str(), value.c_str());
}

void switchRelay(bool on) {
  bool relay = digitalRead(relayPin);

  if (! relayLevel)
    relay = ! relay;
  if (relay != on) {
    digitalWrite(relayPin, relayLevel == on);

    if (mqttServer.length() && pubsubClient.connected()) {
      String topic('/');
      topic += mqttClient;
      topic += mqttTopic;
      mqtt_publish(pubsubClient, topic, String(on));
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (uint8_t i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  char* topicBody = topic + mqttClient.length() + 1; // Skip "/ClientName" from topic
  if (! strncmp(topicBody, mqttTopic.c_str(), mqttTopic.length())) {
    switch ((char)payload[0]) {
      case '0':
        switchRelay(false);
        break;
      case '1':
        switchRelay(true);
        break;
      default:
        bool relay = digitalRead(relayPin);
        if (! relayLevel)
          relay = ! relay;
        mqtt_publish(pubsubClient, String(topic), String(relay));
    }
  } else {
    Serial.println("Unexpected topic!");
  }
}

bool mqttReconnect() {
  const uint32_t timeout = 30000;
  static uint32_t lastTime;
  bool result = false;

  if (millis() > lastTime + timeout) {
    Serial.print("Attempting MQTT connection...");
    digitalWrite(pinBuiltinLed, LOW);
    if (mqttUser.length())
      result = pubsubClient.connect(mqttClient.c_str(), mqttUser.c_str(), mqttPassword.c_str());
    else
      result = pubsubClient.connect(mqttClient.c_str());
    digitalWrite(pinBuiltinLed, HIGH);
    if (result) {
      Serial.println(" connected");
      // Resubscribe
      String topic('/');
      topic += mqttClient;
      topic += mqttTopic;
      result = mqtt_subscribe(pubsubClient, topic);
    } else {
      Serial.print(" failed, rc=");
      Serial.println(pubsubClient.state());
    }
    lastTime = millis();
  }

  return result;
}

/*
 * WiFi setup functions
 */

bool setupWiFiAsStation() {
  const uint32_t timeout = 60000;
  uint32_t maxtime = millis() + timeout;

  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(pinBuiltinLed, LOW);
    delay(500);
    digitalWrite(pinBuiltinLed, HIGH);
    Serial.print(".");
    if (millis() >= maxtime) {
      Serial.println(" fail!");

      return false;
    }
  }
  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  return true;
}

void setupWiFiAsAP() {
  Serial.print("Configuring access point ");
  Serial.println(ssidAP);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssidAP, passwordAP);

  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());
}

void setupWiFi() {
  if ((! ssid.length()) || (! setupWiFiAsStation()))
    setupWiFiAsAP();

  if (domain.length()) {
    if (MDNS.begin(domain.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS responder started");
    } else {
      Serial.println("Error setting up mDNS responder!");
    }
  }

  httpServer.begin();
  Serial.println("HTTP server started (use '/update' url to OTA update)");
}

/*
 * HTTP server functions
 */

String quoteEscape(const String& str) {
  String result = "";
  uint8_t start = 0, pos;

  while (start < str.length()) {
    pos = str.indexOf('"', start);
    if (pos != -1) {
      result += str.substring(start, pos) + "&quot;";
      start = pos + 1;
    } else {
      result += str.substring(start);
      break;
    }
  }
/*
  for (uint16_t i = 0; i < str.length(); i++) {
    if (str[i] == '"')
      result += "&quot;";
    else
      result += str[i];
  }
*/

  return result;
}

void handleRoot() {
  String message =
"<!DOCTYPE html>\
<html>\
<head>\
  <title>ESP Relay</title>\
  <meta http-equiv=\"refresh\" content=\"2\">\
  <style type=\"text/css\">\
    .checkbox {\
      vertical-align:top;\
      margin:0 3px 0 0;\
      width:17px;\
      height:17px;\
    }\
    .checkbox + label {\
      cursor:pointer;\
    }\
    .checkbox:not(checked) {\
      position:absolute;\
      opacity:0;\
    }\
    .checkbox:not(checked) + label {\
      position:relative;\
      padding:0 0 0 60px;\
    }\
    .checkbox:not(checked) + label:before {\
      content:'';\
      position:absolute;\
      top:-4px;\
      left:0;\
      width:50px;\
      height:26px;\
      border-radius:13px;\
      background:#CDD1DA;\
      box-shadow:inset 0 2px 3px rgba(0,0,0,.2);\
    }\
    .checkbox:not(checked) + label:after {\
      content:'';\
      position:absolute;\
      top:-2px;\
      left:2px;\
      width:22px;\
      height:22px;\
      border-radius:10px;\
      background:#FFF;\
      box-shadow:0 2px 5px rgba(0,0,0,.3);\
      transition:all .2s;\
    }\
    .checkbox:checked + label:before {\
      background:#9FD468;\
    }\
    .checkbox:checked + label:after {\
      left:26px;\
    }\
  </style>\
  <script type=\"text/javascript\">\
    function openUrl(url) {\
      var request = new XMLHttpRequest();\
      request.open('GET', url, true);\
      request.send(null);\
    }\
  </script>\
</head>\
<body>\
  <form>\
    <h3>ESP Relay</h3>\
    <p>\
    WiFi mode: ";

  switch (WiFi.getMode()) {
    case WIFI_OFF:
      message += "OFF";
      break;
    case WIFI_STA:
      message += "Station";
      break;
    case WIFI_AP:
      message += "Access Point";
      break;
    case WIFI_AP_STA:
      message += "Hybrid (AP+STA)";
      break;
    default:
      message += "Unknown!";
  }
  message +=
    "<br/>\
    MQTT broker: ";

  if (! pubsubClient.connected())
    message += "not ";
  message += "connected<br/>\
    Heap free size: " + String(ESP.getFreeHeap()) + " bytes<br/>\
    Uptime: " + String(millis() / 1000) + " seconds</p>\
    <input type=\"checkbox\" class=\"checkbox\" id=\"relay\" onchange=\"openUrl('/switch?on=' + this.checked);\" ";

  if (digitalRead(relayPin) == relayLevel)
    message += "checked ";
  message +=
    "/>\
    <label for=\"relay\">Relay</label>\
    <p>\
    <input type=\"button\" value=\"WiFi Setup\" onclick=\"location.href='/wifi';\" />\
    <input type=\"button\" value=\"MQTT Setup\" onclick=\"location.href='/mqtt';\" />\
    <input type=\"button\" value=\"Relay Setup\" onclick=\"location.href='/relay';\" />\
    <input type=\"button\" value=\"Reboot!\" onclick=\"if (confirm('Are you sure to reboot?')) location.href='/reboot';\" />\
  </form>\
</body>\
</html>";

  httpServer.send(200, "text/html", message);
}

void handleWiFiConfig() {
  String message =
"<!DOCTYPE html>\
<html>\
<head>\
  <title>WiFi Setup</title>\
</head>\
<body>\
  <form name=\"wifi\" method=\"get\" action=\"/store\">\
    <h3>WiFi Setup</h3>\
    SSID:<br/>\
    <input type=\"text\" name=\"";

  message += String(ssidArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(ssid) + "\" />\
    <br/>\
    Password:<br/>\
    <input type=\"password\" name=\"";

  message += String(passwordArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(password) + "\" />\
    <br/>\
    mDNS domain:<br/>\
    <input type=\"text\" name=\"";

  message += String(domainArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(domain) + "\" />\
    .local (leave blank to ignore mDNS)\
    <p>\
    <input type=\"submit\" value=\"Save\" />\
    <input type=\"hidden\" name=\"";

  message += String(rebootArg) + "\" value=\"1\" />\
  </form>\
</body>\
</html>";

  httpServer.send(200, "text/html", message);
}

void handleMQTTConfig() {
  String message =
"<!DOCTYPE html>\
<html>\
<head>\
  <title>MQTT Setup</title>\
</head>\
<body>\
  <form name=\"mqtt\" method=\"get\" action=\"/store\">\
    <h3>MQTT Setup</h3>\
    Server:<br/>\
    <input type=\"text\" name=\"";

  message += String(serverArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(mqttServer) + "\" onchange=\"document.mqtt.reboot.value=1;\" />\
    (leave blank to ignore MQTT)\
    <br/>\
    Port:<br/>\
    <input type=\"text\" name=\"";

  message += String(portArg) + "\" maxlength=5 value=\"" + String(mqttPort) + "\" onchange=\"document.mqtt.reboot.value=1;\" />\
    <br/>\
    User (if authorization is required on MQTT server):<br/>\
    <input type=\"text\" name=\"";

  message += String(userArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(mqttUser) + "\" />\
    (leave blank to ignore MQTT authorization)\
    <br/>\
    Password:<br/>\
    <input type=\"password\" name=\"";

  message += String(mqttpswdArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(mqttPassword) + "\" />\
    <br/>\
    Client:<br/>\
    <input type=\"text\" name=\"";

  message += String(clientArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(mqttClient) + "\" />\
    <br/>\
    Topic:<br/>\
    <input type=\"text\" name=\"";

  message += String(topicArg) + "\" maxlength=" + String(maxStrParamLength) + " value=\"" + quoteEscape(mqttTopic) + "\" />\
    <p>\
    <input type=\"submit\" value=\"Save\" />\
    <input type=\"hidden\" name=\"";

  message += String(rebootArg) + "\" value=\"0\" />\
  </form>\
</body>\
</html>";

  httpServer.send(200, "text/html", message);
}

void handleRelayConfig() {
  const byte gpios[] = { 0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16 };

  String message =
"<!DOCTYPE html>\
<html>\
<head>\
  <title>Relay Setup</title>\
</head>\
<body>\
  <form name=\"relay\" method=\"get\" action=\"/store\">\
    <h3>Relay Setup</h3>\
    GPIO:<br/>\
    <select name=\"" + String(gpioArg) + "\" size=1>";

  for (byte i = 0; i < sizeof(gpios); i++) {
    message += "<option value=\"" + String(gpios[i]) + "\"";
    if (relayPin == gpios[i])
      message += " selected";
    message += ">" + String(gpios[i]) + "</option>";
  }
  message += "</select>\
    <br/>\
    Logical level to switch:<br/>\
    <input type=\"radio\" name=\"" + String(levelArg) + "\" value=\"1\" ";

  if (relayLevel)
    message += "checked ";
  message +=
    "/>HIGH\
    <input type=\"radio\" name=\"" + String(levelArg) + "\" value=\"0\" ";

  if (! relayLevel)
    message += "checked ";
  message +=
    "/>LOW\
    <br/>\
    State on boot:<br/>\
    <input type=\"radio\" name=\"" + String(onbootArg) + "\" value=\"1\" ";

  if (relayOnBoot)
    message += "checked ";
  message +=
    "/>On\
    <input type=\"radio\" name=\"" + String(onbootArg) + "\" value=\"0\" ";

  if (! relayOnBoot)
    message += "checked ";
  message +=
    "/>Off\
    <p>\
    <input type=\"submit\" value=\"Save\" />\
    <input type=\"hidden\" name=\"";

  message += String(rebootArg) + "\" value=\"1\" />\
  </form>\
</body>\
</html>";

  httpServer.send(200, "text/html", message);
}

void handleStoreConfig() {
  String argName, argValue;

  Serial.print("/store(");
  for (byte i = 0; i < httpServer.args(); i++) {
    if (i)
      Serial.print(", ");
    argName = httpServer.argName(i);
    Serial.print(argName);
    Serial.print("=\"");
    argValue = httpServer.arg(i);
    Serial.print(argValue);
    Serial.print("\"");

    if (argName == ssidArg) {
      ssid = argValue;
    } else if (argName == passwordArg) {
      password = argValue;
    } else if (argName == domainArg) {
      domain = argValue;
    } else if (argName == serverArg) {
      mqttServer = argValue;
    } else if (argName == portArg) {
      mqttPort = argValue.toInt();
    } else if (argName == userArg) {
      mqttUser = argValue;
    } else if (argName == mqttpswdArg) {
      mqttPassword = argValue;
    } else if (argName == clientArg) {
      mqttClient = argValue;
    } else if (argName == topicArg) {
      mqttTopic = argValue;
    } else if (argName == gpioArg) {
      relayPin = argValue.toInt();
    } else if (argName == levelArg) {
      relayLevel = argValue.toInt();
    } else if (argName == onbootArg) {
      relayOnBoot = argValue.toInt();
    }
  }
  Serial.println(")");

  writeConfig();

  String message =
"<!DOCTYPE html>\
<html>\
<head>\
  <title>Store Setup</title>\
  <meta http-equiv=\"refresh\" content=\"5; /index.html\">\
</head>\
<body>\
  Configuration stored successfully.";

  if (httpServer.arg(rebootArg) == "1")
    message +=
"  <br/>\
  <i>You must reboot module to apply new configuration!</i>";

  message +=
"  <p>\
  Wait for 5 sec. or click <a href=\"/index.html\">this</a> to return to main page.\
</body>\
</html>";

  httpServer.send(200, "text/html", message);
}

void handleRelaySwitch() {
  String on = httpServer.arg("on");

  Serial.print("/switch(");
  Serial.print(on);
  Serial.println(")");

  switchRelay(on == "true");

  String message = "OK";
  httpServer.send(200, "text/html", message);
}

void handleReboot() {
  Serial.println("/reboot()");

  ESP.restart();
}

/*
 * Main setup
 */

void setup() {
  Serial.begin(115200);
  Serial.println();
  pinMode(pinBuiltinLed, OUTPUT);

  EEPROM.begin(1024);
  if (! readConfig())
    Serial.println("EEPROM is empty!");

  digitalWrite(relayPin, relayLevel == relayOnBoot);
  pinMode(relayPin, OUTPUT);

  setupWiFi();

  httpUpdater.setup(&httpServer);
  httpServer.onNotFound([]() {
    httpServer.send(404, "text/plain", "FileNotFound");
  });
  httpServer.on("/", handleRoot);
  httpServer.on("/index.html", handleRoot);
  httpServer.on("/wifi", handleWiFiConfig);
  httpServer.on("/mqtt", handleMQTTConfig);
  httpServer.on("/relay", handleRelayConfig);
  httpServer.on("/store", handleStoreConfig);
  httpServer.on("/switch", handleRelaySwitch);
  httpServer.on("/reboot", handleReboot);

  if (mqttServer.length()) {
    pubsubClient.setServer(mqttServer.c_str(), mqttPort);
    pubsubClient.setCallback(mqttCallback);
  }
}

/*
 * Main loop
 */

void loop() {
  if ((WiFi.getMode() == WIFI_STA) && (WiFi.status() != WL_CONNECTED)) {
    setupWiFi();
  }

  httpServer.handleClient();

  if (mqttServer.length() && (WiFi.getMode() == WIFI_STA)) {
    if (! pubsubClient.connected())
      mqttReconnect();
    if (pubsubClient.connected())
      pubsubClient.loop();
  }

  delay(1); // For WiFi maintenance
}
