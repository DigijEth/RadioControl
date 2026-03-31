#!/system/bin/sh
# RadioControl — early boot property overrides & kernel module loading
# Runs in post-fs-data context (before zygote)

MODDIR=${0%/*}

#############################
# Load saved configuration
#############################

CONFIG_DIR="/data/adb/radiocontrol"
CONFIG_FILE="$CONFIG_DIR/config.sh"

mkdir -p "$CONFIG_DIR"

if [ ! -f "$CONFIG_FILE" ]; then
  cat > "$CONFIG_FILE" << 'DEFAULTS'
# RadioControl configuration — persisted across reboots
# Modified by WebUI, sourced at boot

ENGINEERING_MODE=0
FACTORY_TEST_MODE=0
USB_DIAG_MODE=0
HIDDEN_MENUS=0
MODEM_LOG=0
WIFI_MODE=managed

# Kernel modules to load (space-separated)
# Options: wifi_mon shannon_cmd diag_bridge
LOAD_MODULES=""
DEFAULTS
fi

source "$CONFIG_FILE"

#############################
# Detect chipset
#############################

CHIPSET=$(getprop ro.board.platform 2>/dev/null)
HARDWARE=$(getprop ro.hardware 2>/dev/null)
DETECTED_SOC="unknown"

is_qualcomm() {
  case "$CHIPSET" in msm*|sdm*|sm*|qcom*|lahaina|taro|kalama|pineapple|crow) return 0;; esac
  case "$HARDWARE" in qcom*) return 0;; esac
  return 1
}

is_exynos() {
  case "$CHIPSET" in exynos*|universal*|samsungexynos*) return 0;; esac
  case "$HARDWARE" in exynos*|samsungexynos*) return 0;; esac
  return 1
}

is_tensor() {
  case "$CHIPSET" in gs*|zuma*|ripcurrent*|laguna*) return 0;; esac
  case "$HARDWARE" in gs*|pixel*|oriole|raven|bluejay|panther|cheetah|lynx|tangorpro|felix|shiba|husky|comet|caiman|komodo|tokay|rango|frankel) return 0;; esac
  return 1
}

if is_qualcomm; then DETECTED_SOC="qualcomm"
elif is_exynos; then DETECTED_SOC="exynos"
elif is_tensor; then DETECTED_SOC="tensor"
fi

echo "$DETECTED_SOC" > "$CONFIG_DIR/detected_soc"

#############################
# Apply property overrides
#############################

apply_prop() {
  resetprop "$1" "$2" 2>/dev/null
}

if [ "$ENGINEERING_MODE" = "1" ]; then
  apply_prop ro.build.type eng
  apply_prop ro.debuggable 1
  apply_prop persist.sys.usb.config diag,adb
  apply_prop ro.secure 0
  apply_prop ro.adb.secure 0
  apply_prop service.adb.root 1
fi

if [ "$FACTORY_TEST_MODE" = "1" ]; then
  apply_prop persist.sys.factorytest 1
  apply_prop ro.factorytest 1
  apply_prop persist.radio.fieldtest 1
  apply_prop ro.telephony.hidden_menu 1
  apply_prop persist.radio.apm_sim_not_pwdn 1
  apply_prop persist.radio.sib16_support 1
fi

if [ "$HIDDEN_MENUS" = "1" ]; then
  apply_prop ro.telephony.hidden_menu 1
  apply_prop persist.radio.hidden_menu 1
  apply_prop ro.debuggable 1
fi

if [ "$USB_DIAG_MODE" = "1" ]; then
  apply_prop persist.sys.usb.config diag,serial_cdev,rmnet,adb
  apply_prop sys.usb.configfs 1
fi

if [ "$MODEM_LOG" = "1" ]; then
  apply_prop persist.radio.ramdump 1
  apply_prop persist.vendor.radio.adb_log_on 1
  apply_prop persist.vendor.sys.modem.diag.mdlog on
fi

#############################
# Chipset-specific unlocks
#############################

if [ "$DETECTED_SOC" = "qualcomm" ]; then
  if [ "$HIDDEN_MENUS" = "1" ] || [ "$FACTORY_TEST_MODE" = "1" ]; then
    apply_prop persist.vendor.radio.adb_log_on 1
    apply_prop persist.vendor.radio.enableadvanced 1
    apply_prop persist.radio.field_test 1
    apply_prop persist.radio.secret_code 1
    apply_prop persist.sys.ssr.enable_debug 1
    apply_prop persist.vendor.radio.ca_info 1
    apply_prop persist.vendor.radio.flexmap_type nw_mode
    apply_prop persist.vendor.radio.manual_nw_rej_ct 0
  fi
fi

if [ "$DETECTED_SOC" = "exynos" ]; then
  if [ "$HIDDEN_MENUS" = "1" ] || [ "$FACTORY_TEST_MODE" = "1" ]; then
    apply_prop ro.sec.fle.encryption 0
    apply_prop persist.sys.sysdump_mode active
    apply_prop persist.sys.usb.q_audio_mod 0
    apply_prop ro.factory.sensor 1
    apply_prop persist.sys.factorytest 2
    apply_prop persist.cp.log 1
    apply_prop persist.cp.rat on
  fi
fi

if [ "$DETECTED_SOC" = "tensor" ]; then
  if [ "$HIDDEN_MENUS" = "1" ] || [ "$FACTORY_TEST_MODE" = "1" ]; then
    apply_prop persist.radio.secret_code 1
    apply_prop persist.vendor.radio.enableadvanced 1
    apply_prop persist.radio.field_test 1
    apply_prop persist.vendor.radio.modem_log 1
    apply_prop persist.vendor.sys.modem.logging.enable 1
    apply_prop persist.vendor.radio.nr5g 1
    apply_prop persist.vendor.radio.data_nr_allow 1
  fi
fi

#############################
# Mount debugfs if needed
#############################

if [ ! -d /sys/kernel/debug/clk ]; then
  mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
fi
