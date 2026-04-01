#!/system/bin/sh
# RadioControl — cleanup on uninstall

CONFIG_DIR="/data/adb/radiocontrol"
PID_FILE="$CONFIG_DIR/webui.pid"

# Stop WebUI server
if [ -f "$PID_FILE" ]; then
  kill $(cat "$PID_FILE") 2>/dev/null
fi

# Unload kernel modules
for mod in rc_wifi_mon rc_shannon_cmd rc_diag_bridge; do
  lsmod 2>/dev/null | grep -q "$mod" && rmmod "$mod" 2>/dev/null
done

# Restore WiFi to managed mode
for iface in wlan0 wlan1 wifi0; do
  if [ -d "/sys/class/net/$iface" ]; then
    ip link set "$iface" down 2>/dev/null
    iw dev "$iface" set type managed 2>/dev/null
    ip link set "$iface" up 2>/dev/null
    break
  fi
done

# Remove persistent config
rm -rf "$CONFIG_DIR"
