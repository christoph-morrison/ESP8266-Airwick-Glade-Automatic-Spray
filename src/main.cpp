#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Config.h>
#include <Counter.h>
#include <Bounce2.h>

uint8_t mqttRetryCounter = 0;

const u_int PIN_MOSFET_POWER_SUPPLY = D5;
const u_int PIN_MANUAL_BUTTON = D6;
const u_int POWER_ON_TIME = 9000;
const u_int MQTT_DELAY = 1500;

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
const uint32_t networkPublishInterval = 60000 * 5;

char identifier[30];
#define FIRMWARE_PREFIX "esp8266-air-fragrancer"
#define AVAILABILITY_ONLINE "online"
#define AVAILABILITY_OFFLINE "offline"
char MQTT_TOPIC_AVAILABILITY[256];
char MQTT_TOPIC_NETWORK[256];
char MQTT_TOPIC_KEEP_ALIVE[256];
char MQTT_TOPIC_BURSTCOUNTER[256];
char MQTT_TOPIC_COMMAND[256];
char MQTT_TOPIC_COMMANDSTATE[256];

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
    Config::reset();
    wifiManager.resetSettings();
    WiFi.disconnect(true);
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
        delay(MQTT_DELAY);
    }
}

bool isMqttConnected() {
    return mqttClient.connected();
}

void init_output_pin() {
  pinMode(PIN_MOSFET_POWER_SUPPLY, OUTPUT);
  digitalWrite(PIN_MOSFET_POWER_SUPPLY, LOW);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void publishBurstCounter() {
    char message_buf[512];
    String raw;
    Counter::raw(raw);
    raw.toCharArray(message_buf, raw.length() + 1);
    mqttClient.publish(&MQTT_TOPIC_BURSTCOUNTER[0], message_buf, true);
}

void publishDeviceState(const String& message) {
    mqttReconnect();
    char message_buf[256];
    message.toCharArray(message_buf, message.length() + 1);
    mqttClient.publish(&MQTT_TOPIC_COMMANDSTATE[0], message_buf, false);
}

void incrementCounter() {
    Counter::burstCounterInstance++;
    Counter::burstCounterOverall++;
    Counter::save();
    Serial.printf("New burst counted, now %li bursts (%li overall)\n", 
        Counter::burstCounterInstance, Counter::burstCounterOverall);
    publishBurstCounter();
}

void power_output() {
    publishDeviceState("on");
    digitalWrite(PIN_MOSFET_POWER_SUPPLY, HIGH);
    digitalWrite(LED_BUILTIN, LOW);
    delay(POWER_ON_TIME);
    digitalWrite(PIN_MOSFET_POWER_SUPPLY, LOW);
    digitalWrite(LED_BUILTIN, HIGH);
    incrementCounter();
    publishDeviceState("off");
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

    wifiJson["ssid"]        = WiFi.SSID();
    wifiJson["ip"]          = WiFi.localIP().toString();
    wifiJson["rssi"]        = WiFi.RSSI();

    stateJson["wifi"]       = wifiJson.as<JsonObject>();

    serializeJson(stateJson, payload);
    mqttClient.publish(&MQTT_TOPIC_NETWORK[0], &payload[0], true);
}

void resetInstanceCounter() {
    Serial.println("Instance burst counter reset received");

    publishDeviceState("reset-instance-counter");
    Counter::resetInstanceCounter();
    publishBurstCounter();
    delay(1000);
    publishDeviceState("off");
    mqttReconnect();
}

void resetAllCounter() {
    Serial.println("All counter reset received");

    publishDeviceState("reset-all-counter");
    Counter::resetAllCounter();
    publishBurstCounter();
    delay(1000);
    publishDeviceState("off");
    mqttReconnect();
}

void resetConfig() {
    publishDeviceState("reset-config");
    delay(1000);
    publishDeviceState("gone");
    Serial.println("Config reset received");
    resetWifiSettingsAndReboot();
}

void resetAll() {
    Serial.println("Complete reset received");
    publishDeviceState("reset-all");
    delay(1000);
    publishDeviceState("goodbye and thank you for the fish");
    delay(1000);
    publishDeviceState("the cake is a lie");
    delay(1000);
    Counter::resetAllCounter();
    resetWifiSettingsAndReboot();
}

void publishUpdate() {
    publishBurstCounter();
    publishNetworkState();
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) { 

    Serial.print("Received message [on topic ");
    Serial.print(topic);
    Serial.print("]: ");

    char msg[length+1];
    
    for (u_int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
        msg[i] = (char)payload[i];
    }
    
    Serial.println();
 
    msg[length] = '\0';
 
    if( strcmp(topic, MQTT_TOPIC_COMMAND) == 0) {
    
        if ( ( strcmp(msg, "on" ) ==  0 || strcmp(msg, "1") == 0) ) {
            Serial.println("Power on received via MQTT command");
            power_output();
        }

        if ( strcmp(msg, "reset-instance-counter") == 0 ) {
            resetInstanceCounter();
        }

        if ( strcmp(msg, "reset-all-counter") == 0 ) {
            resetAllCounter();
        }

        if ( strcmp(msg, "reset-config") == 0 ) {
            resetConfig();
        }

        if ( strcmp(msg, "reset-all") == 0 ) {
            resetAll();
        }

        if ( strcmp(msg, "update") == 0 ) {
            publishUpdate();
        }
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

    delay(MQTT_DELAY);

    snprintf(identifier, sizeof(identifier), "%s-%X", FIRMWARE_PREFIX, ESP.getChipId());
    snprintf(MQTT_TOPIC_AVAILABILITY, 255,   "%s/%X/connection",    mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_NETWORK, 255,        "%s/%X/network",       mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_COMMAND, 255,        "%s/%X/command",       mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_KEEP_ALIVE, 255,     "%s/%X/keep-alive",    mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_BURSTCOUNTER, 255,   "%s/%X/burst-counter", mqttTopicPrefix.c_str(), ESP.getChipId());
    snprintf(MQTT_TOPIC_COMMANDSTATE, 255,   "%s/%X/device-state",  mqttTopicPrefix.c_str(), ESP.getChipId());
    
    WiFi.hostname(identifier);

    Config::load();

    setupWifi();
    setupOTA();
    mqttClient.setServer(Config::mqtt_server, 1883);
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(mqttCallback);

    Serial.println("-- Network configuration --");
    Serial.printf("Hostname: %s\n", identifier);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.println();
    
    Serial.println("-- GPIO configuration --");
    Serial.printf("Button pin: %d\n", PIN_MANUAL_BUTTON);
    Serial.printf("MOSFET control pin: %d\n", PIN_MOSFET_POWER_SUPPLY);
    Serial.println();

    Serial.println("-- Persistent storage --");
    Counter::load();
    Serial.printf("Instance: %li bursts\n", Counter::burstCounterInstance);
    Serial.printf("Overall:  %li bursts\n", Counter::burstCounterOverall);
    Serial.println();

    mqttReconnect();

    publishDeviceState("boot");

    button.attach( PIN_MANUAL_BUTTON , INPUT_PULLUP );
    button.interval(5); 
    button.setPressedState(LOW); 

    init_output_pin();
    publishBurstCounter();

    publishDeviceState("off");
}

void loop() {
    ArduinoOTA.handle();
    mqttClient.loop();
    button.update();

    if (initDone == false) {
        Serial.printf("Process initial config informations:\n");
                
        Serial.printf("  - network,\n");
        publishNetworkState();
        
        Serial.printf("  - keep-alive: ");
        publishKeepAlive();
                
        Serial.println("done!");
        Serial.println("Starting normal operation\n");
        initDone = true;
    }

    if ( button.pressed() ) {
        Serial.println("Manual burst requested.\n");
        power_output();
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

    if (!mqttClient.connected() && currentMillis - lastMqttConnectionAttempt >= mqttConnectionInterval) {
        lastMqttConnectionAttempt = currentMillis;
        printf("Reconnect mqtt\n");

        publishBurstCounter();
        mqttReconnect();
    }
}
