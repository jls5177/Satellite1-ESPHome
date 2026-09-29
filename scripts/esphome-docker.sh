#!/usr/bin/env bash
# Run ESPHome in Docker with the firmware version stamped from git, so Home
# Assistant's device page shows e.g. "Firmware: v0.1.5-147-g808c0c9 (ESPHome ...)"
# instead of "dev". A "-dirty" suffix marks builds with uncommitted changes.
#
# Usage: scripts/esphome-docker.sh <command> <config.yaml> [esphome args...]
#   scripts/esphome-docker.sh compile config/satellite1-504ff8-noweb.yaml
#   scripts/esphome-docker.sh upload  config/satellite1-504ff8-noweb.yaml --device 192.168.104.172
#
# Environment: ESPHOME_IMAGE (default ghcr.io/esphome/esphome:2026.8.1),
#              FW_VERSION (overrides the git-derived version).
set -euo pipefail

if [ $# -lt 2 ]; then
  sed -n '2,12p' "$0"
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
image=${ESPHOME_IMAGE:-ghcr.io/esphome/esphome:2026.8.1}
version=${FW_VERSION:-$(git -C "$repo_root" describe --always --dirty --abbrev=7 --match 'v*' 2>/dev/null || echo dev)}
command=$1
shift

echo "esphome ${command} with esp32_fw_version=${version}" >&2
exec docker run --rm -v "${repo_root}":/config "${image}" \
  -s esp32_fw_version "${version}" "${command}" "$@"
