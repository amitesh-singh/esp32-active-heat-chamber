#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

class display
{
    static constexpr uint8_t WIDTH  = 128;
    static constexpr uint8_t HEIGHT = 64;

    Adafruit_SSD1306 display_;
    char ip_buf_[16] = "---";

    // ── Layout constants ──────────────────────────────────────────────────────
    static constexpr uint8_t ROW1 = 0;   // Temp + Humidity + Setpoint
    static constexpr uint8_t ROW2 = 20;  // Divider
    static constexpr uint8_t ROW3 = 24;  // Heater state + Fan speed
    static constexpr uint8_t ROW4 = 36;  // Power bar label  
    static constexpr uint8_t ROW5 = 46;  // Power bar 

public:
    display() : display_(WIDTH, HEIGHT, &Wire, -1) {}

    void begin()
    {
        if (!display_.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
            return;
        }
        display_.clearDisplay();
        display_.setTextColor(SSD1306_WHITE);
        display_.cp437(true);

        display_.setTextSize(1);
        display_.setCursor(20, 24);
        display_.print("Heater Chamber");
        display_.setCursor(20, 32);
        display_.print("v");
        display_.print(FW_VERSION);
        display_.display();
        delay(1500);
    }

    void ota_update()
    {
        display_.clearDisplay();
        display_.setTextSize(1);
        display_.setCursor(5, 24);
        display_.print("checking updates...");
        display_.display();
        delay(1000);
    }

    void ota_update_uptodate()
    {
         display_.clearDisplay();
        display_.setTextSize(1);
        display_.setCursor(2, 24);
        display_.print("firmware is latest");
        display_.display();
        delay(1000);
    }
    
    void ota_update_install()
    {
        display_.clearDisplay();
        display_.setTextSize(1);
        display_.setCursor(5, 24);
        display_.print("updating...");
        display_.display();
        delay(1000);
    }

    void setIP(const char* ip)
    {
        strncpy(ip_buf_, ip, sizeof(ip_buf_) - 1);
    }

    /**
     * @param temp        Chamber temperature °C
     * @param humidity    Relative humidity %
     * @param setpoint    Target temperature °C
     * @param heaterOn    true if heater active
     * @param fanSpeed    Fan PWM 0–255
     * @param pidOutput   PID output 0.0–1.0
     * @param sensorFault true if I2C sensor not responding
     */
    void update(float temp, float humidity, float setpoint,
                bool heaterOn, uint8_t fanSpeed,
                float pidOutput, bool sensorFault = false)
    {
        display_.clearDisplay();

        if (sensorFault) {
            _drawFaultScreen();
        } else {
            _drawTempHumidity(temp, humidity, setpoint);
            _drawDivider(ROW2);
            _drawHeaterFan(heaterOn, fanSpeed);
            _drawPowerBar(pidOutput);             // no divider above
        }

        display_.display();
    }

private:

    void _drawTempHumidity(float temp, float humidity, float setpoint)
    {
        // Left: large temperature
        display_.setTextSize(2);
        display_.setCursor(0, ROW1);
        display_.print(temp, 1);
        display_.setTextSize(1);
        display_.print("\xF8" "C");

        // Top right: humidity
        char humBuf[10];
        snprintf(humBuf, sizeof(humBuf), "%.1f%%RH", humidity);
        int16_t x = WIDTH - (strlen(humBuf) * 6);
        display_.setCursor(x, ROW1);
        display_.print(humBuf);

        // Bottom right: setpoint
        char spBuf[10];
        snprintf(spBuf, sizeof(spBuf), "SP:%.1f\xF8" "C", setpoint);
        x = WIDTH - (strlen(spBuf) * 6);
        display_.setCursor(x, ROW1 + 9);
        display_.print(spBuf);
    }

    void _drawHeaterFan(bool heaterOn, uint8_t fanSpeed)
    {
        display_.setTextSize(1);

        display_.setCursor(0, ROW3);
        display_.print("HTR:");

        if (heaterOn) {
            display_.fillRoundRect(24, ROW3 - 1, 22, 11, 2, SSD1306_WHITE);
            display_.setTextColor(SSD1306_BLACK);
            display_.setCursor(26, ROW3);
            display_.print(" ON");
            display_.setTextColor(SSD1306_WHITE);
        } else {
            display_.drawRoundRect(24, ROW3 - 1, 24, 11, 2, SSD1306_WHITE);
            display_.setCursor(26, ROW3);
            display_.print("OFF");
        }

        uint8_t fanPct = (uint8_t)((fanSpeed / 255.0f) * 100.0f);
        char fanBuf[12];
        snprintf(fanBuf, sizeof(fanBuf), "FAN:%3d%%", fanPct);
        display_.setCursor(WIDTH - (strlen(fanBuf) * 6), ROW3);
        display_.print(fanBuf);
    }

    void _drawPowerBar(float pidOutput)
    {
        _drawDivider(ROW4 - 2);

        display_.setTextSize(1);
        display_.setCursor(0, ROW4 + 6);
        char pwrBuf[10];
        snprintf(pwrBuf, sizeof(pwrBuf), "PWR%3d%%", (int)(pidOutput * 100.0f));
        display_.print(pwrBuf);

        uint8_t barX = 48, barY = ROW5, barW = WIDTH - barX - 2, barH = 8;
        display_.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
        uint8_t fillW = (uint8_t)(pidOutput * (barW - 2));
        if (fillW > 0)
            display_.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
        
        // ── IP address — centered, no divider ────────────────────────────────────
        uint8_t ipLen = strlen(ip_buf_) * 6;   // 6px per char at textSize 1
        display_.setCursor((WIDTH - ipLen) / 2, 56);
        display_.print(ip_buf_);
    }

    void _drawDivider(uint8_t y)
    {
        display_.drawFastHLine(0, y, WIDTH, SSD1306_WHITE);
    }

    void _drawFaultScreen()
    {
        display_.setTextSize(1);
        display_.setCursor(28, 8);
        display_.print("! FAULT !");
        _drawDivider(18);
        display_.setCursor(4, 28);
        display_.print("Sensor offline");
        display_.setCursor(4, 38);
        display_.print("SDA:GPIO8 SCL:GPIO9");
        uint8_t ipLen = strlen(ip_buf_) * 6;
        display_.setCursor((WIDTH - ipLen) / 2, 56);
        display_.print(ip_buf_);
    }
};