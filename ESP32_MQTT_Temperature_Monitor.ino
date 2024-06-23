#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "";
const char* password = "";

// MQTT settings
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
char mqtt_server[50] = "";
char mqtt_username[50] = "";
char mqtt_password[50] = "";
char mqtt_client_id[30] = "esp32"; // Default client ID, change as needed
char mqtt_topic_base[100]; // Base topic for MQTT

// Temperature settings
int oneWireBus = 27; // GPIO pin for DS18B20
OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);
float temperatureOffsets[10]; // Array to store temperature offsets
bool useFahrenheit = false; // Flag to indicate temperature unit

// Preferences instance
Preferences preferences;

// Async web server on port 80
AsyncWebServer server(80);

// Keep-alive interval
unsigned long lastKeepAlive = 0;
const unsigned long keepAliveInterval = 30000; // 30 seconds

void setupWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
}

void reconnectMQTT() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.println("Connecting to MQTT...");
    if (mqttClient.connect(mqtt_client_id, mqtt_username, mqtt_password)) {
      Serial.println("Connected to MQTT");
    } else {
      Serial.print("Failed to connect, state: ");
      Serial.println(mqttClient.state());
      delay(2000);
      attempts++;
    }
  }
}

String buildTemperatureDisplay() {
  String html = "<h2>Temperatures</h2><table>";
  sensors.requestTemperatures();
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    DeviceAddress address;
    sensors.getAddress(address, i);
    float tempC = sensors.getTempC(address);
    float tempF = sensors.getTempF(address);
    
    if (tempC == -127.0 || tempF == -196.6) {
      tempC = 0.0;
      tempF = 0.0;
    } else {
      tempC += temperatureOffsets[i];
      tempF += temperatureOffsets[i];
    }

    html += "<tr><td>Sensor ";
    html += i;
    html += "</td><td>";
    html += useFahrenheit ? String(tempF, 1) : String(tempC, 1); // Format to 1 decimal place
    html += useFahrenheit ? " F" : " C"; // Removed the degree symbol
    html += "</td><td>Address: ";
    for (uint8_t j = 0; j < 8; j++) {
      if (address[j] < 16) html += "0";
      html += String(address[j], HEX);
    }
    html += "</td></tr>";
  }
  html += "</table>";
  return html;
}

String buildConfigForm() {
  String form = "<form action='/save_config' method='GET'>";
  form += "<label for='mqtt_server'>MQTT Server:</label>";
  form += "<input type='text' id='mqtt_server' name='mqtt_server' value='" + String(mqtt_server) + "' required><br>";
  form += "<label for='mqtt_username'>Username:</label>";
  form += "<input type='text' id='mqtt_username' name='mqtt_username' value='" + String(mqtt_username) + "'><br>";
  form += "<label for='mqtt_password'>Password:</label>";
  form += "<input type='password' id='mqtt_password' name='mqtt_password' value='" + String(mqtt_password) + "'><br>";
  form += "<label for='mqtt_client_id'>Client ID:</label>";
  form += "<input type='text' id='mqtt_client_id' name='mqtt_client_id' value='" + String(mqtt_client_id) + "'><br>";
  form += "<label for='temperature_topic'>Temperature Topic:</label>";
  form += "<input type='text' id='temperature_topic' name='temperature_topic' value='" + String(mqtt_topic_base) + "/temperature' required><br>";
  form += "<label for='use_fahrenheit'>Temperature Unit:</label>";
  form += "<select id='use_fahrenheit' name='use_fahrenheit'>";
  form += "<option value='false' " + String(!useFahrenheit ? "selected" : "") + ">Celsius</option>";
  form += "<option value='true' " + String(useFahrenheit ? "selected" : "") + ">Fahrenheit</option>";
  form += "</select><br>";

  // Add temperature offsets for each sensor
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    form += "<label for='offset";
    form += String(i);
    form += "'>Offset for Sensor ";
    form += String(i);
    form += ":</label>";
    form += "<input type='number' step='0.1' id='offset";
    form += String(i);
    form += "' name='offset";
    form += String(i);
    form += "' value='";
    form += temperatureOffsets[i];
    form += "'><br>";
  }

  form += "<label for='gpio'>GPIO Pin for DS18B20:</label>";
  form += "<input type='number' id='gpio' name='gpio' value='" + String(oneWireBus) + "' required><br>";

  form += "<input type='submit' value='Save'>";
  form += "</form>";
  form += "<br><a href='/'>Back</a>";
  return form;
}

void publishChangedTemperatures() {
  static float lastTemperatures[10] = {0}; // Array to store last published temperatures
  static unsigned long lastPublishTimes[10] = {0}; // Array to store the last publish times

  sensors.requestTemperatures();
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    float currentTemp = useFahrenheit ? sensors.getTempFByIndex(i) : sensors.getTempCByIndex(i);
    if (currentTemp == -127.0 || currentTemp == -196.6) {
      currentTemp = 0.0;
    } else {
      currentTemp += temperatureOffsets[i];
    }
    unsigned long currentTime = millis();

    if (abs(currentTemp - lastTemperatures[i]) >= 0.2 || (currentTime - lastPublishTimes[i] >= 10000)) { // Check if temperature has changed by at least 0.2 degree or 10 seconds have passed
      DeviceAddress address;
      sensors.getAddress(address, i);

      // Convert the device address to a string representation
      String addressStr = "";
      for (uint8_t j = 0; j < 8; j++) {
        if (address[j] < 16) addressStr += "0"; // Ensure byte values are two-digit hex
        addressStr += String(address[j], HEX);
      }

      char topic[100];
      snprintf(topic, sizeof(topic), "%s/%s/temperature", mqtt_topic_base, addressStr.c_str());
      mqttClient.publish(topic, String(currentTemp).c_str(), true);
      Serial.print("Published to ");
      Serial.print(topic);
      Serial.print(": ");
      Serial.println(currentTemp);
      lastTemperatures[i] = currentTemp; // Update last published temperature
      lastPublishTimes[i] = currentTime; // Update last publish time

      // Update the web page with the new temperature
      String htmlPage = "<html><head><title>ESP32 Temperature Monitor</title>";
      htmlPage += "<meta http-equiv='refresh' content='30'>";
      htmlPage += "</head><body><h1>ESP32 Temperature Monitor</h1>";
      htmlPage += buildTemperatureDisplay();
      htmlPage += "<br><a href='/config'>Configuration</a>";
      htmlPage += "</body></html>";
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Initialize preferences
  preferences.begin("settings", false);

  // Load MQTT settings from Preferences
  preferences.getString("mqtt_server", mqtt_server, sizeof(mqtt_server));
  preferences.getString("mqtt_username", mqtt_username, sizeof(mqtt_username));
  preferences.getString("mqtt_password", mqtt_password, sizeof(mqtt_password));
  preferences.getString("mqtt_client_id", mqtt_client_id, sizeof(mqtt_client_id));
  snprintf(mqtt_topic_base, sizeof(mqtt_topic_base), "%s", mqtt_client_id);

  // Load temperature unit from Preferences
  useFahrenheit = preferences.getBool("use_fahrenheit", false);

  // Load GPIO pin from Preferences, or scan for it if not set
  oneWireBus = preferences.getInt("gpio_pin", 27);
  if (oneWireBus == -1) {
    Serial.println("No OneWire sensors found!");
    return; // Stop setup if no sensors are found
  }

  // Initialize OneWire and DallasTemperature libraries
  oneWire.begin(oneWireBus);
  sensors.setOneWire(&oneWire);
  sensors.begin();

  // Load temperature offsets from Preferences
  int numDevices = sensors.getDeviceCount();
  Serial.print("Found ");
  Serial.print(numDevices);
  Serial.println(" devices.");
  for (int i = 0; i < numDevices; i++) {
    temperatureOffsets[i] = preferences.getFloat(("offset" + String(i)).c_str(), 0.0);
  }

  // Setup WiFi
  setupWiFi();

  // Setup MQTT
  mqttClient.setServer(mqtt_server, 1883);
  reconnectMQTT();

  // Setup web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String htmlPage = "<html><head><title>ESP32 Temperature Monitor</title>";
    htmlPage += "<style>body{font-family:Arial,Helvetica,sans-serif;margin:0;padding:0;background-color:#f4f4f9;color:#333;}h1,h2{text-align:center;}table{width:100%;border-collapse:collapse;margin:20px auto;}td,th{border:1px solid #ddd;padding:8px;}th{background-color:#f2f2f2;text-align:left;}tr:nth-child(even){background-color:#f9f9f9;}a{display:inline-block;padding:10px 15px;margin:20px;text-decoration:none;color:#fff;background-color:#007bff;border-radius:5px;}a:hover{background-color:#0056b3;}</style>";
    htmlPage += "<meta http-equiv='refresh' content='30'>";
    htmlPage += "</head><body><h1>ESP32 Temperature Monitor</h1>";
    htmlPage += buildTemperatureDisplay();
    htmlPage += "<br><a href='/config'>Configuration</a>";
    htmlPage += "</body></html>";
    request->send(200, "text/html", htmlPage);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    String htmlPage = "<html><head><title>Configuration</title><style>body{font-family:Arial,Helvetica,sans-serif;margin:0;padding:0;background-color:#f4f4f9;color:#333;}h1,h2{text-align:center;}form{width:90%;max-width:600px;margin:20px auto;padding:20px;border:1px solid #ddd;background-color:#fff;border-radius:5px;}label{display:block;margin-bottom:8px;}input[type='text'],input[type='password'],input[type='number'],select{width:100%;padding:10px;margin-bottom:10px;border:1px solid #ccc;border-radius:4px;}input[type='submit']{display:block;width:100%;padding:10px;background-color:#007bff;color:#fff;border:none;border-radius:4px;cursor:pointer;}input[type='submit']:hover{background-color:#0056b3;}a{display:inline-block;padding:10px 15px;margin:20px;text-decoration:none;color:#fff;background-color:#007bff;border-radius:5px;}a:hover{background-color:#0056b3;}</style></head><body>";
    htmlPage += "<h1>Configuration</h1>";
    htmlPage += buildConfigForm();
    htmlPage += "</body></html>";
    request->send(200, "text/html", htmlPage);
  });

  server.on("/save_config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("mqtt_server")) {
      strcpy(mqtt_server, request->getParam("mqtt_server")->value().c_str());
      preferences.putString("mqtt_server", mqtt_server);
    }
    if (request->hasParam("mqtt_username")) {
      strcpy(mqtt_username, request->getParam("mqtt_username")->value().c_str());
      preferences.putString("mqtt_username", mqtt_username);
    }
    if (request->hasParam("mqtt_password")) {
      strcpy(mqtt_password, request->getParam("mqtt_password")->value().c_str());
      preferences.putString("mqtt_password", mqtt_password);
    }
    if (request->hasParam("mqtt_client_id")) {
      strcpy(mqtt_client_id, request->getParam("mqtt_client_id")->value().c_str());
      preferences.putString("mqtt_client_id", mqtt_client_id);
      snprintf(mqtt_topic_base, sizeof(mqtt_topic_base), "%s", mqtt_client_id);
    }
    if (request->hasParam("use_fahrenheit")) {
      useFahrenheit = request->getParam("use_fahrenheit")->value().equalsIgnoreCase("true");
      preferences.putBool("use_fahrenheit", useFahrenheit);
    }
    if (request->hasParam("gpio")) {
      oneWireBus = request->getParam("gpio")->value().toInt();
      preferences.putInt("gpio_pin", oneWireBus);
      oneWire.begin(oneWireBus);
      sensors.setOneWire(&oneWire);
      sensors.begin();
    }
    for (int i = 0; i < sensors.getDeviceCount(); i++) {
      if (request->hasParam("offset" + String(i))) {
        temperatureOffsets[i] = request->getParam("offset" + String(i))->value().toFloat();
        preferences.putFloat(("offset" + String(i)).c_str(), temperatureOffsets[i]);
      }
    }
    request->send(200, "text/html", "<html><body><h1>Configuration Saved</h1><a href='/'>Back</a></body></html>");
    reconnectMQTT();
  });

  // Start server
  server.begin();
}

void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }

  mqttClient.loop();
  publishChangedTemperatures();

  unsigned long currentTime = millis();
  if (currentTime - lastKeepAlive > keepAliveInterval) {
    mqttClient.publish((String(mqtt_topic_base) + "/keepalive").c_str(), "keepalive");
    lastKeepAlive = currentTime;
  }

  delay(1000); // Add delay to reduce WDT resets
}
