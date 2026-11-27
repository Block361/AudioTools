#!/bin/sh
set -eu
# Run from the extracted OpenAoIP IoT package; no compiler or GUI is needed on the Pi.
if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo: sudo sh ./deploy/install.sh" >&2
  exit 1
fi
package=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary="$package/bin/openaoip-iot"
if [ ! -f "$binary" ]; then echo "Missing package binary: $binary" >&2; exit 1; fi
# Reject the wrong architecture or missing C++ runtime before stopping an existing service.
"$binary" --version
if ! getent group audio >/dev/null; then echo "Install ALSA/audio support first (audio group missing)." >&2; exit 1; fi
if ! id openaoip-iot >/dev/null 2>&1; then
  useradd --system --user-group --home-dir /var/lib/openaoip-iot --create-home --shell /usr/sbin/nologin openaoip-iot
fi
usermod --append --groups audio openaoip-iot
# Stop only this product during an explicit update. Audio configurations and keys are retained.
if systemctl is-active --quiet openaoip-iot.service; then systemctl stop openaoip-iot.service; fi
install -m 0755 "$binary" /usr/local/bin/openaoip-iot
install -d -o openaoip-iot -g openaoip-iot -m 0750 /var/lib/openaoip-iot
if [ ! -f /var/lib/openaoip-iot/iot.json ]; then
  runuser -u openaoip-iot -- /usr/local/bin/openaoip-iot --init --config /var/lib/openaoip-iot/iot.json
fi
install -m 0644 "$package/deploy/openaoip-iot.service" /etc/systemd/system/openaoip-iot.service
systemctl daemon-reload
echo "OpenAoIP IoT installed. Configuration: /var/lib/openaoip-iot/iot.json"
echo "Set the controller address and copy the private management key into Controller > Manage IoT."
echo "Then start explicitly: sudo systemctl enable --now openaoip-iot.service"
