# RadioControl Kernel Modules

Out-of-tree kernel modules for enabling hardware features that are
compiled out of production Android kernels.

## Build Requirements

- Matching kernel headers for target device
- ARM64 cross-compiler (aarch64-linux-gnu-gcc)
- Device-specific kernel config

## Modules

### rc_wifi_mon.ko
Patches the WiFi driver's nl80211 ops table at runtime to allow
monitor mode and packet injection on chipsets that have the capability
but disable it in their cfg80211 change_virtual_intf handler.

Supports:
- Samsung SCSC/SLSI (slsi_set_monitor_mode — re-enables compiled-out path)
- Broadcom bcmdhd (patches cfg80211_ops to allow NL80211_IFTYPE_MONITOR)
- Qualcomm ath11k/ath12k/cnss (typically already supports monitor, but
  this bypasses vendor restrictions)

### rc_diag_bridge.ko
Creates /dev/rc_diag — a simplified userspace interface to the Qualcomm
DIAG subsystem that bypasses the standard diag driver's filtering.
Allows reading/writing NV items and sending FTM commands from userspace.

### rc_shannon_cmd.ko
Creates /dev/rc_shannon — direct command interface to Samsung Shannon
modem bypassing RIL. Allows raw AT command passthrough and IPC message
injection for band locking, NR mode control, and diagnostic readout.

## Building

```bash
# Set up cross-compilation
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export KERNEL_DIR=/path/to/kernel/source

# Build all modules
make -C $KERNEL_DIR M=$(pwd) modules

# Or build individually
make -C $KERNEL_DIR M=$(pwd) CONFIG_RC_WIFI_MON=m modules
```

## Runtime Loading

Modules are loaded by the RadioControl service.sh based on detected chipset.
KernelSU module overlay places them in /vendor/lib/modules/ or loads
directly via insmod.
