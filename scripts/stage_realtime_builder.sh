#!/usr/bin/env bash
# Stage the realtime variant for Home Assistant's ESPHome Device Builder
# without publishing a fork. Copy the contents of OUT_DIR into the Builder's
# config directory (/config/esphome on the HA host, e.g. via Samba or SSH).
set -euo pipefail

usage() {
  echo "usage: $0 OUT_DIR DEVICE_NAME [VA_URL]" >&2
  echo "  DEVICE_NAME  existing device name, e.g. satellite1-a1b2c3" >&2
  echo "  VA_URL       add-on WebSocket URL (default ws://homeassistant.local:8080/)" >&2
  exit 2
}

[[ $# -ge 2 && $# -le 3 ]] || usage
out=$1
device=$2
va_url=${3:-ws://homeassistant.local:8080/}
[[ $device =~ ^[a-z0-9-]+$ ]] || { echo "DEVICE_NAME must be lowercase letters, digits and '-'" >&2; exit 2; }

repo=$(cd "$(dirname "$0")/.." && pwd)
wrapper="$out/$device.yaml"
[[ ! -e $wrapper ]] || { echo "$wrapper already exists" >&2; exit 1; }

mkdir -p "$out/components" "$out/common"
cp "$repo/config/satellite1.realtime.yaml" "$repo/config/satellite1.realtime.base.yaml" "$out/"
cp -R "$repo/config/common/." "$out/common/"
# satellite1.realtime.yaml loads ../esphome/components, which resolves to
# /config/esphome/components when the YAML lives in /config/esphome.
cp -R "$repo/esphome/components/." "$out/components/"
find "$out" -name __pycache__ -type d -prune -exec rm -rf {} +

cat > "$wrapper" <<EOF
# Satellite1 realtime voice variant, staged from a local checkout.
# If your previous YAML for this device had api/ota/wifi keys, copy them here.
substitutions:
  va_url: "$va_url"
  va_mic_channel: "0"

packages:
  realtime: !include satellite1.realtime.yaml

esphome:
  name: $device
  name_add_mac_suffix: false
EOF

echo "Staged into $out; device YAML: $wrapper"
