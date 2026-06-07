#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <functional>

////#define OTA_DEBUG

enum class ota_state
{
    CHECKING,
    LATEST,
    UPDATING
};
class https_ota 
{
public:
    https_ota(
        const String& currentVersion,
        const String& jsonUrl,
        std::function<void()> onUptodate = nullptr,
        std::function<void()> onInstalling = nullptr
    );

    bool checkAndUpdate();

private:
    String current_version_;
    String json_url_;

    ota_state state_;

    std::function<void()> on_uptodate_;
    std::function<void()> on_installing_;

    bool fetchJson(String& payload);
    bool parseVersion(const String& json, String& newVersion, String& firmwareUrl);
    bool isNewerVersion(const String& newVersion);
    bool performUpdate(const String& firmwareUrl);

    static int versionCompare(const String& v1, const String& v2);
};


https_ota::https_ota(
    const String& currentVersion,
    const String& jsonUrl,
    std::function<void()> onUptodate,
    std::function<void()> onInstalling
)
    : current_version_(currentVersion),
      json_url_(jsonUrl),
      on_uptodate_(onUptodate),
      on_installing_(onInstalling) {}

bool https_ota::checkAndUpdate() 
{
    String jsonPayload;
    String newVersion;
    String firmwareUrl;

    state_ = ota_state::CHECKING;

    if (!fetchJson(jsonPayload)) {
        #ifdef OTA_DEBUG
        Serial.println("Failed to fetch update JSON");
        #endif
        return false;
    }

    if (!parseVersion(jsonPayload, newVersion, firmwareUrl)) {
        #ifdef OTA_DEBUG
        Serial.println("Invalid update JSON");
        #endif
        return false;
    }

    if (!isNewerVersion(newVersion)) {
        if (on_uptodate_) on_uptodate_();
        #ifdef OTA_DEBUG
        Serial.println("Firmware is up to date");
        #endif
        return false;
    }
    #ifdef OTA_DEBUG
    Serial.printf("Updating from %s to %s\n",
                  current_version_.c_str(),
                  newVersion.c_str());
    #endif
    return performUpdate(firmwareUrl);
}

bool https_ota::fetchJson(String& payload)
{
    WiFiClientSecure client;
    client.setInsecure(); // Use root CA for production

    HTTPClient https;
    if (!https.begin(client, json_url_))
        return false;

    int httpCode = https.GET();
    if (httpCode != HTTP_CODE_OK)
        return false;

    payload = https.getString();
    https.end();
    return true;
}

bool https_ota::parseVersion(
    const String& json,
    String& newVersion,
    String& firmwareUrl
) 
{
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;

    newVersion  = doc["version"].as<String>();
    firmwareUrl = doc["firmware"].as<String>();

    return !newVersion.isEmpty() && !firmwareUrl.isEmpty();
}

bool https_ota::isNewerVersion(const String& newVersion) 
{
    return versionCompare(newVersion, current_version_) > 0;
}

bool https_ota::performUpdate(const String& firmwareUrl)
{
    WiFiClientSecure client;
    client.setInsecure(); // Replace with cert validation in production

    if (on_installing_) on_installing_();

    HTTPClient https;
    if (!https.begin(client, firmwareUrl)) {
        #ifdef OTA_DEBUG
        Serial.printf("failed to get the firmware url: %s\r\n", firmwareUrl.c_str());
        #endif
        return false;
    }

    int httpCode = https.GET();
    if (httpCode != HTTP_CODE_OK)
        return false;

    int contentLength = https.getSize();
    bool canBegin = Update.begin(contentLength);
    if (!canBegin) return false;

    WiFiClient* stream = https.getStreamPtr();
    size_t written = Update.writeStream(*stream);

    if (written != contentLength)
        return false;

    if (!Update.end())
        return false;

    if (!Update.isFinished())
        return false;

    #ifdef OTA_DEBUG
    Serial.println("Update complete, rebooting...");
    #endif
    delay(1000);
    ESP.restart();

    return true;
}

int https_ota::versionCompare(const String& v1, const String& v2)
{
    int p1[3] = {0}, p2[3] = {0};
    sscanf(v1.c_str(), "%d.%d.%d", &p1[0], &p1[1], &p1[2]);
    sscanf(v2.c_str(), "%d.%d.%d", &p2[0], &p2[1], &p2[2]);

    for (int i = 0; i < 3; i++) {
        if (p1[i] > p2[i]) return 1;
        if (p1[i] < p2[i]) return -1;
    }
    return 0;
}