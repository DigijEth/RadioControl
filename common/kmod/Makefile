# RadioControl out-of-tree kernel modules
#
# Build all:
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KERNEL_DIR=/path/to/kernel
#
# Build specific module:
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KERNEL_DIR=/path/to/kernel MOD=rc_wifi_mon
#
# Target: Pixel 10 Pro Fold (rango), Tensor G5, kernel 6.6.102

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

obj-$(CONFIG_RC_WIFI_MON)    += rc_wifi_mon.o
obj-$(CONFIG_RC_SHANNON_CMD) += rc_shannon_cmd.o
obj-$(CONFIG_RC_DIAG_BRIDGE) += rc_diag_bridge.o

# Default: build all
CONFIG_RC_WIFI_MON    ?= m
CONFIG_RC_SHANNON_CMD ?= m
CONFIG_RC_DIAG_BRIDGE ?= m

# Allow building a single module via MOD=name
ifdef MOD
  CONFIG_RC_WIFI_MON    = n
  CONFIG_RC_SHANNON_CMD = n
  CONFIG_RC_DIAG_BRIDGE = n
  CONFIG_$(shell echo $(MOD) | tr a-z A-Z) = m
endif

# Extra compiler flags for Android/ARM64
ccflags-y += -Wno-unused-function
ccflags-y += -DCONFIG_RADIOCONTROL

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules_install

# Copy built modules to the module package directory
package:
	@mkdir -p ../../common/kmod_out
	@for ko in *.ko; do \
		if [ -f "$$ko" ]; then \
			cp "$$ko" ../../common/kmod_out/; \
			echo "Packaged: $$ko"; \
		fi; \
	done

.PHONY: all clean install package
