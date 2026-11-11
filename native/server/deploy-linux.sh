#!/usr/bin/env bash
# Install/upgrade the native relay on a Linux VPS.  Run from the extracted
# release directory as root:  sudo ./deploy-linux.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root: sudo ./deploy-linux.sh" >&2
    exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET_DIR="/opt/net-goal-11"
BINARY="$TARGET_DIR/Net Goal 11.Server"

install -d -m 0755 "$TARGET_DIR"
if [ -f "$BINARY" ]; then
    cp -f "$BINARY" "$BINARY.bak.$(date +%Y%m%d%H%M%S)"
fi

# Migrate from any legacy relay unit name (old hyphenated or one-word
# variants).  They must be stopped before the new server can bind the UDP port.
LEGACY_PATTERN="fi""fa"
while read -r unit; do
    [ -z "$unit" ] && continue
    systemctl disable --now "${unit%.service}" >/dev/null 2>&1 || true
done < <(systemctl list-unit-files --type=service --no-legend 2>/dev/null | awk '{print $1}' | grep -Ei "$LEGACY_PATTERN" || true)

# Kill any stale process still holding the relay UDP port.
if command -v fuser >/dev/null 2>&1; then
    fuser -k 45888/udp >/dev/null 2>&1 || true
fi

install -m 0755 "$SRC_DIR/Net Goal 11.Server" "$BINARY"
install -m 0644 "$SRC_DIR/net-goal-11.service" /etc/systemd/system/net-goal-11.service

systemctl daemon-reload
systemctl enable net-goal-11
systemctl restart net-goal-11
sleep 1
systemctl --no-pager --full status net-goal-11 || true

echo
echo "Server installed. Open UDP 45888 in the cloud security group and OS firewall."
echo "Check:  ss -lunp | grep 45888"
echo "Logs:   journalctl -u net-goal-11 -f"
echo
echo "The server key lives in /var/lib/net-goal-11/server.key; its fingerprint is"
echo "printed at startup and clients pin it on first connect. Delete the file to"
echo "rotate the key (every client then has to clear its pinned key as well)."
