# RadioControl Kernel Modules

Out-of-tree kernel modules for enabling hardware features that are
compiled out of production Android kernels.

Target: Pixel 10 Pro Fold (rango), Tensor G5, kernel 6.6.102

## Build Requirements

- Matching kernel headers for target device (kernel 6.6.x)
- ARM64 cross-compiler (aarch64-linux-gnu-gcc)
- Device-specific kernel config (CONFIG_KPROBES=y, CONFIG_MODULES=y)

## Modules

### rc_wifi_mon.ko
Patches the WiFi driver's nl80211 ops table at runtime to allow
monitor mode and packet injection on chipsets that have the capability
but disable it in their cfg80211 change_virtual_intf handler.

Features:
- Uses kprobes-based kallsyms lookup (works on kernel 5.7+)
- Patches wiphy->interface_modes bitmask for monitor + OCB
- Driver-specific firmware iovars for BCM4390 (monitor, promisc, allmulti)
- SCSC/SLSI MIB patching for Maxwell firmware monitor enable
- sysfs status at /sys/kernel/rc_wifi_mon/status
- Clean restore on module unload

Supports:
- Broadcom bcmdhd4390 (BCM4390, primary target)
- Samsung SCSC/SLSI (Exynos WiFi)
- Qualcomm ath11k/ath12k/cnss

### rc_shannon_cmd.ko
Creates /dev/rc_shannon — direct command interface to Samsung Shannon
modem bypassing RIL. Allows raw AT command passthrough and IPC message
injection for band locking, NR mode control, and diagnostic readout.

Features:
- Auto-detects modem path (umts_atc0, nr_atc0, umts_router)
- URC (unsolicited result code) buffering with ring buffer (64 entries)
- Structured ioctl interface (RC_SHANNON_AT_CMD) with configurable timeout
- Simple read/write interface for basic use
- Statistics tracking (cmds sent, bytes tx/rx)
- Modem connectivity test on load
- Kernel 6.4+ class_create compatibility

### rc_diag_bridge.ko
Creates /dev/rc_diag — a simplified userspace interface to the Qualcomm
DIAG subsystem. Handles HDLC framing internally.

Features:
- NV item read/write (DIAG_NV_READ_F / DIAG_NV_WRITE_F)
- FTM commands (Factory Test Mode) via subsystem dispatch
- EFS2 file operations (open, read, write, stat, unlink)
- Full HDLC encode/decode with CRC-16 CCITT validation
- Modem version query
- Raw DIAG passthrough for advanced use
- Graceful inactive mode when no Qualcomm modem present

Note: This module is for Qualcomm-baseband devices. On Tensor G5 with
Shannon 5400, use rc_shannon_cmd instead. rc_diag_bridge will load but
remain inactive.

## Shared Header

`rc_ioctl.h` contains all ioctl definitions for both modules. Include
this from userspace C code to use the structured command interfaces.

## Building

```bash
# Set up cross-compilation
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export KERNEL_DIR=/path/to/kernel/source

# Build all modules
make

# Build a single module
make MOD=rc_wifi_mon

# Package .ko files for the module zip
make package

# Clean
make clean
```

## Runtime Loading

Modules are loaded by RadioControl's service.sh based on detected chipset.
The service automatically:
1. Detects SoC type (Tensor/Exynos/Qualcomm)
2. Loads the appropriate modules (rc_wifi_mon + rc_shannon_cmd for Tensor)
3. Skips rc_diag_bridge on non-Qualcomm devices
4. Verifies module load via /proc/modules
