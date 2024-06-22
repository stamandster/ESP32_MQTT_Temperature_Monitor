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
int oneWireBus = 13; // GPIO pin for DS18B20
OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);
float temperatureOffsets[10]; // Array to store temperature offsets
bool useFahrenheit = false; // Flag to indicate temperature unit

// Preferences instance
Preferences preferences;

// Async web server on port 80
AsyncWebServer server(80);

// Function to scan for OneWire devices on a range of GPIO pins
int scanForOneWireBus() {
  for (int pin = 0; pin < 40; pin++) {
    OneWire testOneWire(pin);
    DallasTemperature testSensors(&testOneWire);
    testSensors.begin();
    if (testSensors.getDeviceCount() > 0) {
      return pin;
    }
  }
  return -1; // Return -1 if no sensors are found on any pin
}

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
  String html = "<h2>Temperatures</h2>";
  sensors.requestTemperatures();
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    DeviceAddress address;
    sensors.getAddress(address, i);
    float tempC = sensors.getTempC(address) + temperatureOffsets[i];
    float tempF = sensors.getTempF(address) + temperatureOffsets[i];
    html += "Sensor ";
    html += i;
    html += ": ";
    html += useFahrenheit ? String(tempF, 1) : String(tempC, 1); // Format to 1 decimal place
    html += useFahrenheit ? " F" : " C";
    html += " (Address: ";
    for (uint8_t j = 0; j < 8; j++) {
      if (address[j] < 16) html += "0";
      html += String(address[j], HEX);
    }
    html += ")<br>";
  }
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
  sensors.requestTemperatures();
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    float currentTemp = useFahrenheit ? sensors.getTempFByIndex(i) : sensors.getTempCByIndex(i);
    if (abs(currentTemp - lastTemperatures[i]) >= 0.2) { // Check if temperature has changed by at least 0.2 degree
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

  // Construct MQTT topic base
  snprintf(mqtt_topic_base, sizeof(mqtt_topic_base), "%s", mqtt_client_id);

  // Load temperature unit preference
  useFahrenheit = preferences.getBool("use_fahrenheit", false);

  // Automatically scan for the OneWire bus if no GPIO pin is saved in preferences
  oneWireBus = preferences.getInt("gpio_pin", -1);
  if (oneWireBus == -1) {
    oneWireBus = scanForOneWireBus();
    preferences.putInt("gpio_pin", oneWireBus);
  }
  oneWire.begin(oneWireBus);
  sensors.setOneWire(&oneWire);

  // Setup WiFi
  setupWiFi();

  // Setup MQTT
  mqttClient.setServer(mqtt_server, 1883);
  reconnectMQTT();

  // Setup DS18B20 temperature sensors
  sensors.begin();
  int numDevices = sensors.getDeviceCount();
  Serial.print("Found ");
  Serial.print(numDevices, DEC);
  Serial.println(" devices.");
  for (int i = 0; i < numDevices; i++) {
    temperatureOffsets[i] = preferences.getFloat(("offset" + String(i)).c_str(), 0.0);
  }

  // Setup web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String htmlPage = "<html><head><title>ESP32 Temperature Monitor</title>";
    htmlPage += "<meta http-equiv='refresh' content='30'>";
    htmlPage += "</head><body><h1>ESP32 Temperature Monitor</h1>";
    htmlPage += buildTemperatureDisplay();
    htmlPage += "<br><a href='/config'>Configuration</a>";
    htmlPage += "</body></html>";
    request->send(200, "text/html", htmlPage);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    String htmlPage = "<html><head><title>Configuration</title></head><body>";
    htmlPage += "<h1>Configuration</h1>";
    htmlPage += buildConfigForm();
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
      useFahrenheit = request->getParam("use_fahrenheit")->value() == "true";
      preferences.putBool("use_fahrenheit", useFahrenheit);
    } else {
      useFahrenheit = false;
      preferences.putBool("use_fahrenheit", false);
    }

    // Save temperature offsets
    for (int i = 0; i < sensors.getDeviceCount(); i++) {
      if (request->hasParam("offset" + String(i))) {
        float offset = request->getParam("offset" + String(i))->value().toFloat();
        temperatureOffsets[i] = offset;
        preferences.putFloat(("offset" + String(i)).c_str(), offset);
      }
    }

    // Save GPIO pin for DS18B20
    if (request->hasParam("gpio")) {
      oneWireBus = request->getParam("gpio")->value().toInt();
      preferences.putInt("gpio_pin", oneWireBus);
      oneWire.begin(oneWireBus); // Reinitialize OneWire with the new pin
      sensors.setOneWire(&oneWire);
      sensors.begin();
    }

    request->send(200, "text/html", "<html><body><h1>Configuration Saved!</h1><br><a href='/config'>Back to Configuration</a></body></html>");
    delay(2000); // Delay to ensure preferences are saved before rebooting
    ESP.restart();
  });

  server.begin();
}

void loop() {
  mqttClient.loop();
  publishChangedTemperatures();
}
