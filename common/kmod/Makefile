# RadioControl out-of-tree kernel modules
# Build: make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KERNEL_DIR=/path/to/kernel

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

obj-$(CONFIG_RC_WIFI_MON)    += rc_wifi_mon.o
obj-$(CONFIG_RC_DIAG_BRIDGE) += rc_diag_bridge.o
obj-$(CONFIG_RC_SHANNON_CMD) += rc_shannon_cmd.o

# Default: build all
CONFIG_RC_WIFI_MON    ?= m
CONFIG_RC_DIAG_BRIDGE ?= m
CONFIG_RC_SHANNON_CMD ?= m

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules_install
