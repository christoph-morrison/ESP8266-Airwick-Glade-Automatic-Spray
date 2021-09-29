#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Config.h>
#include <Bounce2.h>

uint8_t mqttRetryCounter = 0;

const u_int PIN_SPRAY_POWER_SUPPLY = D5;
const u_int PIN_MANUAL_BUTTON = D6;
const u_int POWER_ON_TIME = 7000;

WiFiManager  wifiManager;
WiFiClient   wifiClient;
PubSubClient mqttClient;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", Config::mqtt_server, sizeof(Config::mqtt_server));
WiFiManagerParameter custom_mqtt_user("user", "MQTT username", Config::username, sizeof(Config::username));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", Config::password, sizeof(Config::password));

// 1 minute = 60 seconds = 60000 milliseconds
uint32_t lastMqttConnectionAttempt = 0;
const uint16_t mqttConnectionInterval = 60000 * 1; 

uint32_t keepAlivePreviousMillis = 0;
const uint16_t keepAlivePublishInterval = 60000 * 1;

uint32_t networkPreviousMillis = 0;
const uint32_t networkPublishInterval = 60000 * 30;

char identifier[30];
#define FIRMWARE_PREFIX "esp8266-air-fragrancer"
#define AVAILABILITY_ONLINE "online"
#define AVAILABILITY_OFFLINE "offline"
char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_KEEP_ALIVE[128];
char MQTT_TOPIC_COMMAND[128];

bool shouldSaveConfig = false;
bool initDone = false;

Bounce2::Button button = Bounce2::Button();

// MQTT topic prefix
String mqttTopicPrefix = "hab/devices/luxuries/automatic-air-fragrancer";

void saveConfigCallback() {
    shouldSaveConfig = true;
}

void setupOTA() {
    ArduinoOTA.onStart([]() { Serial.println("Start"); });
    ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        }
    });

    ArduinoOTA.setHostname(identifier);

    // This is less of a security measure and more a accidential flash prevention
    ArduinoOTA.setPassword(identifier);
    ArduinoOTA.begin();
}

void setupWifi() {
    wifiManager.setDebugOutput(false);
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);

    WiFi.hostname(identifier);
    wifiManager.autoConnect(identifier);
    mqttClient.setClient(wifiClient);

    strcpy(Config::mqtt_server, custom_mqtt_server.getValue());
    strcpy(Config::username, custom_mqtt_user.getValue());
    strcpy(Config::password, custom_mqtt_pass.getValue());

    if (shouldSaveConfig) {
        Config::save();
    } else {
        // For some reason, the read values get overwritten in this function
        // To combat this, we just reload the config
        // This is most likely a logic error which could be fixed otherwise
        Config::load();
    }
}

void resetWifiSettingsAndReboot() {
    wifiManager.resetSettings();
    delay(3000);
    ESP.restart();
}

void mqttReconnect() {
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        if (mqttClient.connect(identifier, Config::username, Config::password, MQTT_TOPIC_AVAILABILITY, 1, true, AVAILABILITY_OFFLINE)) {
            mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_ONLINE, true);
            
            // Make sure to subscribe after polling the status so that we never execute commands with the default data
            mqttClient.subscribe(MQTT_TOPIC_COMMAND);
            break;
        }
        delay(5000);
    }
}

bool isMqttConnected() {
    return mqttClient.connected();
}

void init_output_pin() {
  pinMode(PIN_SPRAY_POWER_SUPPLY, OUTPUT);
  digitalWrite(PIN_SPRAY_POWER_SUPPLY, LOW);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void power_output() {
  digitalWrite(PIN_SPRAY_POWER_SUPPLY, HIGH);
  digitalWrite(LED_BUILTIN, LOW);
  delay(POWER_ON_TIME);
  digitalWrite(PIN_SPRAY_POWER_SUPPLY, LOW);
  digitalWrite(LED_BUILTIN, HIGH);
}

void publishKeepAlive() {
    char keepAliveMsg[5] = "ping";
    Serial.printf("Send keep alive message: %s\n", keepAliveMsg);

    mqttClient.publish(&MQTT_TOPIC_KEEP_ALIVE[0], keepAliveMsg, true);
}

void publishNetworkState() {
    DynamicJsonDocument wifiJson(192);
    DynamicJsonDocument stateJson(1024);
    char payload[256];

    wifiJson["ssid"]    = WiFi.SSID();
    wifiJson["ip"]      = WiFi.localIP().toString();
    wifiJson["rssi"]    = WiFi.RSSI();

    stateJson["wifi"]   = wifiJson.as<JsonObject>();

    serializeJson(stateJson, payload);
    mqttClient.publish(&MQTT_TOPIC_STATE[0], &payload[0], true);
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) { 

    Serial.print("Received message [");
    Serial.print(topic);
    Serial.print("] ");

    char msg[length+1];
    
    for (u_int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
        msg[i] = (char)payload[i];
    }
    
    Serial.println();
 
    msg[length] = '\0';
    Serial.println(msg);
 
    if(strcmp(msg, "on" ) ==  0 || strcmp(msg, "1") == 0) {
      power_output();
    }
}

void setup() {
    Serial.begin(115200);

    Serial.println("\n");
    Serial.printf("Hello from %s-%X\n", FIRMWARE_PREFIX, ESP.getChipId());
    Serial.printf("Core Version: %s\n", ESP.getCoreVersion().c_str());
    Serial.printf("Boot Version: %u\n", ESP.getBootVersion());
    Serial.printf("Boot Mode: %u\n", ESP.getBootMode());
    Serial.printf("CPU Frequency: %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Reset reason: %s\n", ESP.getResetReason().c_str());

    delay(3000);

    snprintf(identifier, sizeof(identifier), "%s-%X", FIRMWARE_PREFIX, ESP.getChipId());
    snprintf(MQTT_TOPIC_AVAILABILITY, 127,   "%s/%X/connection",  mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_STATE, 127,          "%s/%X/state",       mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_COMMAND, 127,        "%s/%X/command",     mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_KEEP_ALIVE, 127,     "%s/%X/keep-alive",  mqttTopicPrefix.c_str(), ESP.getChipId());

    WiFi.hostname(identifier);

    Config::load();

    setupWifi();
    setupOTA();
    mqttClient.setServer(Config::mqtt_server, 1883);
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(mqttCallback);

    Serial.printf("Hostname: %s\n", identifier);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.println("-- Current GPIO Configuration --");
    Serial.printf("Button pin: %d\n", PIN_MANUAL_BUTTON);
    Serial.printf("MOSFET control pin: %d\n", PIN_SPRAY_POWER_SUPPLY);

    mqttReconnect();

    button.attach( PIN_MANUAL_BUTTON , INPUT_PULLUP );
    button.interval(5); 
    button.setPressedState(LOW); 

    init_output_pin();
}

void loop() {
    ArduinoOTA.handle();
    mqttClient.loop();
    button.update();

    if (initDone == false) {
        Serial.print("Process initial config informations: ");
        
        publishNetworkState();
        Serial.print("network, ");
        
        publishKeepAlive();
        Serial.print("keep-alive, ");
        
        Serial.println("done");
        initDone = true;
    }

    const uint32_t currentMillis = millis();
    if (currentMillis - keepAlivePreviousMillis >= keepAlivePublishInterval) {
        keepAlivePreviousMillis = currentMillis;
        publishKeepAlive();
    }

    if (currentMillis - networkPreviousMillis >= networkPublishInterval) {
        networkPreviousMillis = currentMillis;
        publishNetworkState();
    }

    if ( button.pressed() ) {
        power_output();
    }
 
    if (!mqttClient.connected() && currentMillis - lastMqttConnectionAttempt >= mqttConnectionInterval) {
        lastMqttConnectionAttempt = currentMillis;
        printf("Reconnect mqtt\n");
        mqttReconnect();
    }
}
