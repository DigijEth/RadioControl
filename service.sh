#!/system/bin/sh
# RadioControl — late service script
# Loads kernel modules, starts WebUI, applies WiFi mode

MODDIR=${0%/*}
CONFIG_DIR="/data/adb/radiocontrol"
CONFIG_FILE="$CONFIG_DIR/config.sh"
LOG_FILE="$CONFIG_DIR/radiocontrol.log"
PID_FILE="$CONFIG_DIR/webui.pid"
KMOD_DIR="$MODDIR/common/kmod"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$LOG_FILE"
}

mkdir -p "$CONFIG_DIR"
source "$CONFIG_FILE" 2>/dev/null
DETECTED_SOC=$(cat "$CONFIG_DIR/detected_soc" 2>/dev/null)

log "RadioControl service starting (SoC: $DETECTED_SOC)"

#############################
# Load kernel modules
#############################

load_kmod() {
  local mod_name="$1"
  local mod_path="$KMOD_DIR/${mod_name}.ko"

  if [ -f "$mod_path" ]; then
    log "Loading kernel module: $mod_name"
    insmod "$mod_path" 2>> "$LOG_FILE"
    if [ $? -eq 0 ]; then
      log "  -> $mod_name loaded successfully"
    else
      log "  -> $mod_name FAILED (may need kernel headers rebuild)"
    fi
  else
    log "  -> $mod_name.ko not found at $mod_path"
  fi
}

# Auto-detect which modules to load based on SoC
if echo "$LOAD_MODULES" | grep -q "wifi_mon"; then
  load_kmod "rc_wifi_mon"
fi

if echo "$LOAD_MODULES" | grep -q "shannon_cmd"; then
  load_kmod "rc_shannon_cmd"
elif [ "$DETECTED_SOC" = "exynos" ] || [ "$DETECTED_SOC" = "tensor" ]; then
  # Auto-load Shannon module for Exynos/Tensor if AT terminal is used
  if [ -c /dev/umts_atc0 ] || [ -c /dev/nr_atc0 ]; then
    load_kmod "rc_shannon_cmd"
  fi
fi

if echo "$LOAD_MODULES" | grep -q "diag_bridge"; then
  load_kmod "rc_diag_bridge"
elif [ "$DETECTED_SOC" = "qualcomm" ] && [ -c /dev/diag ]; then
  load_kmod "rc_diag_bridge"
fi

#############################
# Detect available modem interfaces
#############################

detect_modem_interfaces() {
  local interfaces=""

  # Shannon AT channels (Exynos/Tensor) — umts_router is the primary AT interface
  for dev in /dev/umts_router /dev/umts_atc0 /dev/umts_atc1 /dev/umts_router0 /dev/nr_atc0; do
    [ -c "$dev" ] && interfaces="$interfaces $dev"
  done

  # Shannon IPC / DM / OEM channels
  for dev in /dev/umts_ipc0 /dev/umts_ipc1 /dev/umts_dm0 /dev/umts_rfs0 \
             /dev/umts_boot0 /dev/umts_loopback /dev/umts_rcs0 /dev/umts_rcs1 \
             /dev/umts_wfc0 /dev/umts_wfc1 /dev/umts_toe0 \
             /dev/oem_ipc0 /dev/oem_ipc1 /dev/oem_ipc2 /dev/oem_ipc3 \
             /dev/oem_ipc4 /dev/oem_ipc5 /dev/oem_ipc6 /dev/oem_ipc7 \
             /dev/gnss_ipc /dev/acd-factory_diag; do
    [ -c "$dev" ] && interfaces="$interfaces $dev"
  done

  # Qualcomm DIAG
  [ -c /dev/diag ] && interfaces="$interfaces /dev/diag"

  # Qualcomm AT
  for dev in /dev/smd7 /dev/ttyHS0 /dev/ttyMSM0 /dev/at_mdm0; do
    [ -c "$dev" ] && interfaces="$interfaces $dev"
  done

  # Our kernel module devices
  [ -c /dev/rc_shannon ] && interfaces="$interfaces /dev/rc_shannon"
  [ -c /dev/rc_diag ] && interfaces="$interfaces /dev/rc_diag"

  echo "$interfaces" > "$CONFIG_DIR/modem_interfaces"
  log "Detected modem interfaces:$interfaces"
}

detect_modem_interfaces

#############################
# Detect WiFi driver & capabilities
#############################

detect_wifi_info() {
  local info_file="$CONFIG_DIR/wifi_info"
  echo "" > "$info_file"

  for iface in /sys/class/net/*; do
    local name=$(basename "$iface")
    if [ -d "$iface/wireless" ] || [ -d "$iface/phy80211" ] || echo "$name" | grep -qE '^(wlan|wifi|wlp)'; then
      echo "IFACE=$name" >> "$info_file"

      # Driver name
      local driver=$(readlink "$iface/device/driver" 2>/dev/null | xargs basename 2>/dev/null)
      echo "DRIVER=$driver" >> "$info_file"

      # Module name
      local module=$(basename "$(readlink "$iface/device/driver/module" 2>/dev/null)" 2>/dev/null)
      echo "MODULE=$module" >> "$info_file"

      # Firmware path
      if [ -f /sys/module/bcmdhd/parameters/firmware_path ]; then
        echo "FW_PATH=$(cat /sys/module/bcmdhd/parameters/firmware_path)" >> "$info_file"
        echo "WIFI_CHIP=broadcom" >> "$info_file"
      elif [ -d /sys/module/scsc_wlan ]; then
        echo "WIFI_CHIP=samsung_scsc" >> "$info_file"
        [ -f /d/scsc/mx/mxman ] && echo "SCSC_STATE=$(cat /d/scsc/mx/mxman 2>/dev/null)" >> "$info_file"
      elif [ -d /sys/module/ath11k ] || [ -d /sys/module/ath12k ]; then
        echo "WIFI_CHIP=qualcomm_ath" >> "$info_file"
      elif [ -d /sys/module/cnss2 ]; then
        echo "WIFI_CHIP=qualcomm_cnss" >> "$info_file"
      fi

      # Current mode
      local mode=$(iw dev "$name" info 2>/dev/null | grep type | awk '{print $2}')
      echo "CURRENT_MODE=$mode" >> "$info_file"

      # PHY capabilities
      local phy=$(iw dev "$name" info 2>/dev/null | grep wiphy | awk '{print $2}')
      if [ -n "$phy" ]; then
        echo "PHY=phy${phy}" >> "$info_file"
        iw phy "phy${phy}" info 2>/dev/null | sed -n '/Supported interface modes/,/^[^\t]/p' | grep '^\s*\*' | sed 's/.*\* /SUPPORTED_MODE=/' >> "$info_file"
        # Bands
        iw phy "phy${phy}" info 2>/dev/null | grep -E "Band [0-9]" | sed 's/.*Band /BAND=/' >> "$info_file"
      fi

      # Check monitor mode via rc_wifi_mon
      if lsmod 2>/dev/null | grep -q rc_wifi_mon; then
        echo "MONITOR_PATCH=loaded" >> "$info_file"
      else
        echo "MONITOR_PATCH=not_loaded" >> "$info_file"
      fi
    fi
  done

  log "WiFi info written to $info_file"
}

# Wait for WiFi to be up
sleep 3
detect_wifi_info

#############################
# Apply WiFi mode if non-default
#############################

if [ -n "$WIFI_MODE" ] && [ "$WIFI_MODE" != "managed" ]; then
  sleep 2
  local iface=""
  for candidate in wlan0 wlan1 wifi0; do
    [ -d "/sys/class/net/$candidate" ] && iface="$candidate" && break
  done

  if [ -n "$iface" ]; then
    log "Applying WiFi mode: $WIFI_MODE on $iface"
    ip link set "$iface" down 2>/dev/null
    case "$WIFI_MODE" in
      monitor)   iw dev "$iface" set type monitor 2>/dev/null ;;
      injection) iw dev "$iface" set type monitor 2>/dev/null
                 iw dev "$iface" set monitor fcsfail otherbss 2>/dev/null ;;
      mesh)      iw dev "$iface" set type mesh 2>/dev/null ;;
      ap)        iw dev "$iface" set type __ap 2>/dev/null ;;
    esac
    ip link set "$iface" up 2>/dev/null
  fi
fi

#############################
# Enumerate debugfs paths
#############################

enumerate_debugfs() {
  local dbg_file="$CONFIG_DIR/debugfs_paths"
  echo "" > "$dbg_file"

  # Modem/radio related debugfs
  for path in \
    /sys/kernel/debug/modem_diag \
    /sys/kernel/debug/diag \
    /sys/kernel/debug/msm_subsys \
    /sys/kernel/debug/ipc_logging \
    /sys/kernel/debug/cnss \
    /sys/kernel/debug/wlan \
    /sys/kernel/debug/ath11k \
    /sys/kernel/debug/ath12k \
    /sys/kernel/debug/ipa \
    /sys/kernel/debug/scsc \
    /sys/kernel/debug/clk \
    /sys/kernel/debug/regulator \
    /sys/kernel/debug/remoteproc \
    /sys/kernel/debug/aoc \
    /sys/kernel/debug/trusty \
    /sys/kernel/debug/gsa \
    /sys/kernel/debug/mali \
    /sys/kernel/debug/asv; do
    if [ -d "$path" ]; then
      echo "$path" >> "$dbg_file"
    fi
  done

  log "debugfs enumeration done ($(wc -l < "$dbg_file") paths found)"
}

enumerate_debugfs

#############################
# Start WebUI server
#############################

if [ -f "$PID_FILE" ]; then
  kill $(cat "$PID_FILE") 2>/dev/null
  rm -f "$PID_FILE"
fi

log "Starting WebUI on port 8088"
nohup /system/bin/radiocontrol >> "$LOG_FILE" 2>&1 &
echo $! > "$PID_FILE"

log "RadioControl service started (PID: $(cat $PID_FILE))"
