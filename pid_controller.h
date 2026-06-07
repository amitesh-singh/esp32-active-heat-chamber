#pragma once

/**
 * PID_Controller.h
 * Simple, clean PID controller for temperature control
 * Anti-windup + output clamping included
 */

#include <Arduino.h>

class pid_controller {
public:
    /**
     * @param kp  Proportional gain
     * @param ki  Integral gain
     * @param kd  Derivative gain
     * @param outMin  Minimum output (default 0.0)
     * @param outMax  Maximum output (default 1.0)
     */
    pid_controller(float kp, float ki, float kd,
                  float outMin = 0.0f, float outMax = 1.0f)
        : _kp(kp), _ki(ki), _kd(kd),
          _outMin(outMin), _outMax(outMax),
          _setpoint(0), _integral(0), _lastError(0),
          _lastTime(0), _initialized(false) {}

    void setSetpoint(float setpoint) { _setpoint = setpoint; }
    float getSetpoint() const { return _setpoint; }

    void setTunings(float kp, float ki, float kd) {
        _kp = kp; _ki = ki; _kd = kd;
    }

    /** Reset integrator — call when setpoint changes significantly */
    void reset()
    {
        _integral = 0;
        _lastError = 0;
        _initialized = false;
    }

    /**
     * Compute PID output. Call at consistent intervals (e.g. every 500ms).
     * @param measured  Current temperature reading
     * @return output   Power level 0.0–1.0
     */
    float compute(float measured)
    {
        uint32_t now = millis();

        if (!_initialized) {
            _lastTime = now;
            _initialized = true;
            return 0.0f;
        }

        float dt = (now - _lastTime) / 1000.0f;  // seconds
        if (dt <= 0.0f) return constrain(_lastOutput, _outMin, _outMax);

        float error = _setpoint - measured;

        // Proportional
        float pTerm = _kp * error;

        // Integral with anti-windup clamping
        _integral += error * dt;
        float iTerm = _ki * _integral;

        // Derivative (on measurement, not error — avoids derivative kick)
        float dTerm = -_kd * (measured - _lastMeasured) / dt;

        float output = constrain(pTerm + iTerm + dTerm, _outMin, _outMax);

        // Anti-windup: clamp integral if output is saturated
        if ((output >= _outMax && error > 0) ||
            (output <= _outMin && error < 0)) {
            _integral -= error * dt;  // undo last integral accumulation
        }

        _lastError    = error;
        _lastMeasured = measured;
        _lastOutput   = output;
        _lastTime     = now;

        return output;
    }

    float getLastOutput() const { return _lastOutput; }
    float getIntegral()   const { return _integral; }

private:
    float _kp, _ki, _kd;
    float _outMin, _outMax;
    float _setpoint;
    float _integral;
    float _lastError;
    float _lastMeasured = 0;
    float _lastOutput   = 0;
    uint32_t _lastTime;
    bool _initialized;
};
