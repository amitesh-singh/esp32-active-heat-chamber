#pragma once

#include <Arduino.h>

class heater_controller
{
public:

    // ── Constructor ──────────────────────────────────────────────────────────
    /**
     * @param pin       GPIO pin connected to MOSFET gate
     * @param cycleMs   Full ON/OFF cycle period in milliseconds (default 2000)
     */
    heater_controller(uint8_t pin, uint32_t cycleMs = 2000)
        : pin_(pin)
        , cycle_ms_(cycleMs)
        , power_(0.0f)
        , cycle_start_(0)
        , enabled_(false)
        , state_(false)
    {}

    void begin()
    {
        pinMode(pin_, OUTPUT);
        _off(true);
        cycle_start_ = millis();
    }

    void update() 
    {
        if (!enabled_) {
            _off();
            return;
        }

        uint32_t now     = millis();
        uint32_t elapsed = now - cycle_start_;

        // Start a new cycle when the previous one completes
        if (elapsed >= cycle_ms_) {
            cycle_start_ = now;
            elapsed     = 0;
        }

        // ON for the first (power × cycleMs) milliseconds of each cycle
        uint32_t onWindow = (uint32_t)(power_ * cycle_ms_);

        if (elapsed < onWindow) {
            _on();
        } else {
            _off();
        }
    }

    /**
     * Set heater power fraction.
     * @param power  0.0 = fully off, 1.0 = fully on. Clamped automatically.
     */
    void setPower(float power)
    {
        power_ = constrain(power, 0.0f, 1.0f);
    }

    float getPower() const { return power_; }

    void enable()
    {
        enabled_    = true;
        cycle_start_ = millis();   // reset cycle so we start fresh
    }

    void disable() 
    {
        enabled_ = false;
        power_   = 0.0f;
        _off();
    }

    bool isEnabled() const { return enabled_; }

    /**
     * Immediately cuts the SSR and locks the controller off.
     * Call enable() to recover (only after resolving the fault condition).
     */
    void emergencyStop()
    {
        enabled_ = false;
        power_   = 0.0f;
        _off();
    }

    bool isOn() const { return state_; }

    uint32_t onTimeMs() const
    {
        return (uint32_t)(power_ * cycle_ms_);
    }

    uint32_t cycleMs() const { return cycle_ms_; }

private:

    void _on()
    {
        if (!state_) {
            digitalWrite(pin_, HIGH);
            state_ = true;
        }
    }

    void _off(bool forcefully = false) 
    {
        if (state_ || forcefully) {
            digitalWrite(pin_, LOW);
            state_ = false;
        }
    }

    uint8_t  pin_;
    uint32_t cycle_ms_;
    float    power_;
    uint32_t cycle_start_;
    bool     enabled_;
    bool     state_;
};
