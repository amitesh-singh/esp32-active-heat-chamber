#pragma once 

#define FW_VERSION "1.0.0"

// Pins (ESP32-C3 Super Mini) ────────────────────────────────────────────────
#define HEATER_PIN         3 // On/Off -> Heater
#define FAN_PWM_PIN     10   // PWM out -> MOSFET gate (LEDC)

// Fan LEDC
#define FAN_FREQ_HZ     25000
#define FAN_RESOLUTION  8       // 0–255

// Safety
#define MAX_TEMP_C      65.0f
#define DEFAULT_TARGET  50.0f
#define HEATER_CYCLE_MS    2000

// PID tuning; tune this based on heater and fan.
#define PID_KP  0.08f
#define PID_KI  0.015f
#define PID_KD  0.005f

// SHT3x
#define I2C_SDA     8
#define I2C_SCL     9
