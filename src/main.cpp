#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <OneButton.h>

#include "Config.h"

const char* SSID        = "MorrisonNetIoT";
const char* PSK         = "3WPu8WK6M9yU3e438j39";
const char* MQTT_BROKER = "message-broker";
const char* MQTT_TOPIC  = "test/airmatic";

const u_int WAIT = 10000;
const u_int AIRMATIC_ACTIVATION_PIN = D5;


WiFiClient wifiClient;
PubSubClient pubSubClient(wifiClient);
long lastMsg = 0;
char msg[50];
int value = 0;

const u_int BUTTON_PIN = D6;

OneButton button = OneButton(
  BUTTON_PIN,   // Input pin for the button
  false,        // Button is active high
  true          // Disable internal pull-up resistor
);

void setPowerOn() {
  pinMode(AIRMATIC_ACTIVATION_PIN, OUTPUT);
  digitalWrite(AIRMATIC_ACTIVATION_PIN, LOW);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void pushVirtButton() {
  digitalWrite(AIRMATIC_ACTIVATION_PIN, HIGH);
  digitalWrite(LED_BUILTIN, LOW);
  delay(WAIT);
  digitalWrite(AIRMATIC_ACTIVATION_PIN, LOW);
  digitalWrite(LED_BUILTIN, HIGH);
}

void pushButton() {
    Serial.println("Manual button pressed");
    pushVirtButton();
}

void initManualButton() {
    button.attachClick(pushButton);
    delay(500);
    Serial.println("Push button initialized");
}

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(SSID);
 
    WiFi.begin(SSID, PSK);
 
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
 
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}
 
void callback(char* topic, byte* payload, unsigned int length) {
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
 
    if(strcmp(msg,"on") == 0){
      pushVirtButton();
    }
}

void reconnect() {
    while (!pubSubClient.connected()) {
        Serial.println("Reconnecting MQTT...");
        if (!pubSubClient.connect("ESP8266Client")) {
            Serial.print("failed, rc=");
            Serial.print(pubSubClient.state());
            Serial.println(" retrying in 5 seconds");
            delay(5000);
        }
    }
    pubSubClient.subscribe(MQTT_TOPIC);
    Serial.println("MQTT Connected...");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  setPowerOn();
  initManualButton();

  setup_wifi();
  
  pubSubClient.setServer(MQTT_BROKER, 1883);
  pubSubClient.setCallback(callback);
}

void loop() {
    button.tick();

    if (!pubSubClient.connected()) {
        reconnect();
    }

    pubSubClient.loop();
}
