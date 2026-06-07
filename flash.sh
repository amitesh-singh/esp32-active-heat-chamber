#!/bin/bash
#
cp ~/.cache/arduino/sketches/DED7442946156A3C26A9A13D8C50BC41/active_heat_chamber_esp32.ino.bin ota_server/firmware.bin

SOURCE_FILE="config.h"
OUTPUT_FILE="ota_server/firmware.json"

# Extract version string
FW_VERSION=$(grep -oP '#define\s+FW_VERSION\s+"\K[^"]+' "$SOURCE_FILE")

if [[ -z "$FW_VERSION" ]]; then
  echo "❌ Error: FW_VERSION not found in $SOURCE_FILE"
  exit 1
fi

# Write JSON file
cat <<EOF > "$OUTPUT_FILE"
{
  "version": "$FW_VERSION",
  "firmware": "https://192.168.1.13/firmware.bin"
}
EOF

echo "✅ firmware.json generated with version $FW_VERSION"

cat ota_server/firmware.json
