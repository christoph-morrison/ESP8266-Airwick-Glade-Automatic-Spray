#pragma once

#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>

namespace Counter {
    unsigned long burstCounterInstance  = 0;
    unsigned long burstCounterOverall   = 0;

    char counterFileName[30]        = "/counter.dat";
    
    void save() {


        DynamicJsonDocument json(512);    
        json["instance"] = burstCounterInstance;
        json["overall"]  = burstCounterOverall;

        File counterFile = LittleFS.open(counterFileName, "w");

        if (!counterFile) {
            return;
        }
 
        serializeJson(json, counterFile);

        counterFile.close();
    }

    bool load() {
        if (LittleFS.begin()) {
            if (LittleFS.exists(counterFileName)) {
                File counterFile = LittleFS.open(counterFileName, "r");

                if (counterFile) {
                    const size_t size = counterFile.size();
                    std::unique_ptr<char[]> buf(new char[size]);
                
                    counterFile.readBytes(buf.get(), size);
                    DynamicJsonDocument json(512);

                    if (DeserializationError::Ok == deserializeJson(json, buf.get())) {
                        burstCounterInstance    = json["instance"];
                        burstCounterOverall     = json["overall"];
                    }
                }

                return true;
            } else {
                burstCounterInstance = 0;
                burstCounterOverall  = 0;
                save();
                Serial.printf("New counter file %s created\n", counterFileName);
            }

            return false;
        }

        return false;
    }

    void raw(String &raw) {
        File counterFile = LittleFS.open(counterFileName, "r");

        if (counterFile) {
            raw = counterFile.readString();
        }
    }

    void resetInstanceCounter() {
        burstCounterInstance = 0;
        save();
    }

    void resetOverallCounter() {
        burstCounterOverall = 0;
        save();
    }

    void resetAllCounter() {
        burstCounterOverall     = 0;
        burstCounterInstance    = 0;
        save();
    }
}
