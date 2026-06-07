#!/bin/bash
# chamber_control.sh
# Called by Klipper gcode_shell_command to control the ESP32 heater chamber
#
# Usage:
#   chamber_control.sh heater on
#   chamber_control.sh heater off
#   chamber_control.sh temp 60
#   chamber_control.sh fan 180
#   chamber_control.sh status
#
# Install:
#   sudo cp chamber_control.sh /usr/local/bin/chamber_control.sh
#   sudo chmod +x /usr/local/bin/chamber_control.sh

# ── Config ────────────────────────────────────────────────────────────────────
ESP32_IP="192.168.1.20"     # Set to your ESP32's IP (assign static via router)
ESP32_PORT="80"
TIMEOUT=5                    # curl timeout in seconds
LOG_FILE="/tmp/chamber.log"

BASE_URL="http://${ESP32_IP}:${ESP32_PORT}"

# ── Logging ───────────────────────────────────────────────────────────────────
log() {
    echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# ── HTTP helpers ──────────────────────────────────────────────────────────────
http_post() {
    local url="$1"
    local response
    response=$(curl -s -X POST \
        --connect-timeout "$TIMEOUT" \
        --max-time "$TIMEOUT" \
        "$url" 2>&1)
    local code=$?
    if [ $code -ne 0 ]; then
        log "ERROR: curl failed (exit $code) → $url"
        echo "CHAMBER_ERROR: connection failed"
        exit 1
    fi
    echo "$response"
}

http_get() {
    local url="$1"
    local response
    response=$(curl -s -X GET \
        --connect-timeout "$TIMEOUT" \
        --max-time "$TIMEOUT" \
        "$url" 2>&1)
    local code=$?
    if [ $code -ne 0 ]; then
        log "ERROR: curl failed (exit $code) → $url"
        echo "CHAMBER_ERROR: connection failed"
        exit 1
    fi
    echo "$response"
}

# ── Parse JSON field (no jq dependency) ───────────────────────────────────────
json_field() {
    local json="$1"
    local field="$2"
    echo "$json" | grep -oP "\"${field}\":\s*\K[^,}]+" | tr -d '"'
}

# ── Commands ──────────────────────────────────────────────────────────────────
CMD="$1"
ARG="$2"

case "$CMD" in

    heater)
        if [[ "$ARG" != "on" && "$ARG" != "off" ]]; then
            log "Usage: $0 heater on|off"
            exit 1
        fi
        log "Heater → $ARG"
        resp=$(http_post "${BASE_URL}/heater?state=${ARG}")
        log "Response: $resp"
        # Echo back in Klipper-readable format
        temp=$(json_field "$resp" "temp")
        echo "CHAMBER: heater=${ARG} temp=${temp}C"
        ;;

    temp)
        if [ -z "$ARG" ]; then
            log "Usage: $0 temp <value>"
            exit 1
        fi
        # Validate numeric
        if ! [[ "$ARG" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            log "ERROR: temp must be numeric, got: $ARG"
            exit 1
        fi
        log "Setpoint → ${ARG}°C"
        resp=$(http_post "${BASE_URL}/temp?target=${ARG}")
        log "Response: $resp"
        setpoint=$(json_field "$resp" "setpoint")
        echo "CHAMBER: setpoint=${setpoint}C"
        ;;

    fan)
        if [ -z "$ARG" ]; then
            log "Usage: $0 fan <0-255>"
            exit 1
        fi
        if ! [[ "$ARG" =~ ^[0-9]+$ ]] || [ "$ARG" -gt 255 ]; then
            log "ERROR: fan speed must be 0-255, got: $ARG"
            exit 1
        fi
        log "Fan → $ARG"
        resp=$(http_post "${BASE_URL}/fan?speed=${ARG}")
        log "Response: $resp"
        fan=$(json_field "$resp" "fan")
        echo "CHAMBER: fan=${fan}"
        ;;

    status)
        resp=$(http_get "${BASE_URL}/status")
        log "Status: $resp"
        temp=$(json_field "$resp"     "temp")
        setpoint=$(json_field "$resp" "setpoint")
        heater=$(json_field "$resp"   "heater")
        fan=$(json_field "$resp"      "fan")
        pid=$(json_field "$resp"      "pid_output")
        safety=$(json_field "$resp"   "safety_tripped")
        echo "CHAMBER: temp=${temp}C setpoint=${setpoint}C heater=${heater} fan=${fan} pid=${pid} safety=${safety}"
        ;;

    *)
        echo "Usage: $0 {heater on|off | temp <C> | fan <0-255> | status}"
        exit 1
        ;;
esac
