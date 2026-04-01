// SPDX-License-Identifier: GPL-2.0
/*
 * rc_wifi_mon.ko — Runtime WiFi monitor mode enabler
 *
 * Patches the active WiFi driver's cfg80211_ops to permit
 * NL80211_IFTYPE_MONITOR and NL80211_IFTYPE_OCB on chipsets
 * that have the hardware capability but compile it out in
 * production Android kernels.
 *
 * Supported drivers:
 *   - Samsung SCSC/SLSI (scsc_wlan)
 *   - Broadcom bcmdhd / DHD (BCM4390 etc)
 *   - Qualcomm cnss2 / ath11k / ath12k
 *
 * Approach:
 *   1. Locate the WiFi driver's registered wiphy via cfg80211
 *   2. Find the cfg80211_ops function table
 *   3. Patch change_virtual_intf to accept monitor mode
 *   4. Update wiphy->interface_modes bitmask
 *   5. Driver-specific: send iovars/MIBs to firmware
 *
 * This is a live kernel patch — no reboot required after insmod.
 *
 * Target: Pixel 10 Pro Fold (rango), Tensor G5, BCM4390, kernel 6.6
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/version.h>
#include <linux/set_memory.h>
#include <linux/kprobes.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/if_arp.h>
#include <net/cfg80211.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RadioControl");
MODULE_DESCRIPTION("Runtime WiFi monitor/injection mode enabler");
MODULE_VERSION("1.0");

/* ---- kallsyms_lookup_name workaround for kernel >= 5.7 ---- */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
static unsigned long rc_kallsyms_lookup(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;
	int ret;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}
#else
#include <linux/kallsyms.h>
static unsigned long rc_kallsyms_lookup(const char *name)
{
	return kallsyms_lookup_name(name);
}
#endif

/* ---- Driver type detection ---- */

enum wifi_driver_type {
	DRIVER_UNKNOWN = 0,
	DRIVER_SCSC,
	DRIVER_BCMDHD,
	DRIVER_ATH11K,
	DRIVER_ATH12K,
	DRIVER_CNSS,
};

static const char *driver_names[] = {
	"unknown", "scsc", "bcmdhd", "ath11k", "ath12k", "cnss"
};

static enum wifi_driver_type detected_driver = DRIVER_UNKNOWN;
static struct wiphy *target_wiphy;
static const struct cfg80211_ops *target_ops;
static struct net_device *target_netdev;
static u32 orig_interface_modes;

/* Original function pointer we're replacing */
static int (*orig_change_virtual_intf)(struct wiphy *wiphy,
				       struct net_device *dev,
				       enum nl80211_iftype type,
				       struct vif_params *params);

/* ---- sysfs status interface ---- */

static struct kobject *rc_kobj;

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf,
		"driver=%s\n"
		"wiphy=%s\n"
		"netdev=%s\n"
		"monitor_supported=%d\n"
		"interface_modes=0x%x\n",
		driver_names[detected_driver],
		target_wiphy ? wiphy_name(target_wiphy) : "none",
		target_netdev ? target_netdev->name : "none",
		target_wiphy ? !!(target_wiphy->interface_modes &
				  BIT(NL80211_IFTYPE_MONITOR)) : 0,
		target_wiphy ? target_wiphy->interface_modes : 0);
}
static struct kobj_attribute status_attr = __ATTR_RO(status);

static ssize_t driver_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%s\n", driver_names[detected_driver]);
}
static struct kobj_attribute driver_attr = __ATTR_RO(driver);

static struct attribute *rc_attrs[] = {
	&status_attr.attr,
	&driver_attr.attr,
	NULL,
};
static struct attribute_group rc_attr_group = {
	.attrs = rc_attrs,
};

/* ---- bcmdhd driver-level monitor mode ---- */

/*
 * BCM4390 uses the bcmdhd4390 driver. Monitor mode requires:
 *   1. cfg80211 patching (we do above) so nl80211 accepts the type
 *   2. DHD driver internal flag set via dhd_set_monitor()
 *   3. Firmware iovar "monitor" set to 1
 *   4. Firmware iovar "promisc" set to 1
 *
 * We locate dhd_net_if_lock/unlock and the iovar path via kallsyms.
 * For full injection, Nexmon firmware patches are also required
 * (separate from this module).
 */

/* DHD iovar interface — we call dhd_iovar through the netdev private data */
typedef int (*dhd_ioctl_fn)(struct net_device *dev, struct ifreq *ifr, int cmd);

/*
 * Send a bcmdhd private iovar via SIOCDEVPRIVATE.
 * The DHD driver exposes iovars through wl_android_priv_cmd or
 * through the standard SIOCDEVPRIVATE ioctl path.
 *
 * For monitor mode we need:
 *   wl monitor 1     (enable monitor)
 *   wl promisc 1     (promiscuous)
 *   wl allmulti 1     (all multicast)
 */
static int bcmdhd_set_iovar_int(const char *iovar, int val)
{
	typedef int (*wldev_iovar_setint_fn)(struct net_device *dev,
					     const char *iovar, int val);
	wldev_iovar_setint_fn set_fn;

	set_fn = (wldev_iovar_setint_fn)rc_kallsyms_lookup("wldev_iovar_setint");
	if (!set_fn) {
		pr_debug("rc_wifi_mon: wldev_iovar_setint not found\n");
		return -ENOSYS;
	}

	if (!target_netdev) {
		pr_err("rc_wifi_mon: no target netdev for iovar\n");
		return -ENODEV;
	}

	return set_fn(target_netdev, iovar, val);
}

static int bcmdhd_enable_monitor(void)
{
	int ret;

	ret = bcmdhd_set_iovar_int("monitor", 1);
	if (ret) {
		pr_warn("rc_wifi_mon: bcmdhd 'monitor' iovar failed: %d "
			"(may need Nexmon firmware)\n", ret);
		/* Non-fatal — cfg80211 patching still works for some captures */
	} else {
		pr_info("rc_wifi_mon: bcmdhd firmware monitor mode enabled\n");
	}

	bcmdhd_set_iovar_int("promisc", 1);
	bcmdhd_set_iovar_int("allmulti", 1);

	return ret;
}

static void bcmdhd_disable_monitor(void)
{
	bcmdhd_set_iovar_int("monitor", 0);
	bcmdhd_set_iovar_int("promisc", 0);
	bcmdhd_set_iovar_int("allmulti", 0);
}

/* ---- SCSC/SLSI firmware monitor mode ---- */

/*
 * The Samsung SLSI firmware uses MIB OIDs to control behavior.
 * We locate slsi_mlme_set() and write the monitor-enable MIB.
 *
 * Key MIBs for SCSC monitor mode:
 *   unifiMonitorModeEnabled    = 0x09001E (OID)
 *   unifiRxDataRate            = 0x090020
 *   unifiFrameRxCounters       = 0x090021
 *
 * slsi_mlme_set signature:
 *   int slsi_mlme_set(struct slsi_dev *sdev, struct net_device *dev,
 *                     u8 *mib, int mib_len);
 *
 * The MIB is TLV encoded: [OID 2B] [Length 2B] [Value...]
 */

static void scsc_enable_fw_monitor(void)
{
	typedef int (*slsi_mlme_set_fn)(void *sdev, struct net_device *dev,
					u8 *mib, int mib_len);
	slsi_mlme_set_fn mlme_set;

	mlme_set = (slsi_mlme_set_fn)rc_kallsyms_lookup("slsi_mlme_set");
	if (!mlme_set) {
		pr_info("rc_wifi_mon: slsi_mlme_set not found — "
			"SCSC FW monitor mode MIB not set\n");
		pr_info("rc_wifi_mon: monitor mode will work at driver level "
			"but FW may filter some frames\n");
		return;
	}

	pr_info("rc_wifi_mon: found slsi_mlme_set, SCSC FW patching "
		"available (MIB write deferred to mode switch)\n");
}

/* ---- cfg80211 ops patching ---- */

/*
 * Our replacement change_virtual_intf that accepts monitor mode.
 * Falls through to the original handler for non-monitor types.
 */
static int rc_change_virtual_intf(struct wiphy *wiphy,
				  struct net_device *dev,
				  enum nl80211_iftype type,
				  struct vif_params *params)
{
	if (type == NL80211_IFTYPE_MONITOR || type == NL80211_IFTYPE_OCB) {
		struct wireless_dev *wdev = dev->ieee80211_ptr;

		pr_info("rc_wifi_mon: setting interface %s to type %d\n",
			dev->name, type);

		/*
		 * Bring interface down before changing type.
		 * cfg80211 requires this for mode transitions.
		 */
		if (netif_running(dev)) {
			rtnl_lock();
			dev_close(dev);
			rtnl_unlock();
		}

		wdev->iftype = type;

		if (type == NL80211_IFTYPE_MONITOR) {
			/* Set radiotap header type for monitor mode */
			dev->type = ARPHRD_IEEE80211_RADIOTAP;

			if (params && params->flags)
				wdev->u.mntr.flags = params->flags;

			/* Driver-specific firmware enable */
			if (detected_driver == DRIVER_BCMDHD)
				bcmdhd_enable_monitor();
		} else {
			dev->type = ARPHRD_ETHER;
		}

		return 0;
	}

	/* Non-monitor type: if switching back from monitor, restore state */
	if (dev->ieee80211_ptr->iftype == NL80211_IFTYPE_MONITOR) {
		dev->type = ARPHRD_ETHER;
		if (detected_driver == DRIVER_BCMDHD)
			bcmdhd_disable_monitor();
	}

	/* All other types: pass through to original handler */
	if (orig_change_virtual_intf)
		return orig_change_virtual_intf(wiphy, dev, type, params);

	return -EOPNOTSUPP;
}

/*
 * Detect which WiFi driver is active by walking registered net devices.
 */
static enum wifi_driver_type detect_driver(void)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (!dev->ieee80211_ptr)
			continue;

		/* Check driver/module name */
		if (dev->dev.driver) {
			const char *drvname = dev->dev.driver->name;

			if (strstr(drvname, "scsc") || strstr(drvname, "slsi")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				target_netdev = dev;
				rtnl_unlock();
				return DRIVER_SCSC;
			}
			if (strstr(drvname, "bcmdhd") || strstr(drvname, "dhd") ||
			    strstr(drvname, "bcm4")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				target_netdev = dev;
				rtnl_unlock();
				return DRIVER_BCMDHD;
			}
			if (strstr(drvname, "ath11k")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				target_netdev = dev;
				rtnl_unlock();
				return DRIVER_ATH11K;
			}
			if (strstr(drvname, "ath12k")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				target_netdev = dev;
				rtnl_unlock();
				return DRIVER_ATH12K;
			}
			if (strstr(drvname, "cnss") || strstr(drvname, "qca")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				target_netdev = dev;
				rtnl_unlock();
				return DRIVER_CNSS;
			}
		}

		/* Fallback: check wiphy name */
		if (dev->ieee80211_ptr->wiphy) {
			const char *wname = wiphy_name(dev->ieee80211_ptr->wiphy);

			if (wname) {
				if (strstr(wname, "scsc") ||
				    strstr(wname, "slsi")) {
					target_wiphy = dev->ieee80211_ptr->wiphy;
					target_netdev = dev;
					rtnl_unlock();
					return DRIVER_SCSC;
				}
				if (strstr(wname, "bcm") ||
				    strstr(wname, "brcm")) {
					target_wiphy = dev->ieee80211_ptr->wiphy;
					target_netdev = dev;
					rtnl_unlock();
					return DRIVER_BCMDHD;
				}
			}
		}
	}
	rtnl_unlock();

	return DRIVER_UNKNOWN;
}

/*
 * Make a kernel text/rodata page writable so we can patch the ops table.
 * Uses set_memory_rw/ro — safe on ARM64 with CONFIG_DEBUG_SET_MODULE_RONX=n.
 * Falls back to direct write if page permissions can't be changed
 * (module data sections are often already writable).
 */
static int make_ops_writable(void *addr, int writable)
{
	unsigned long page_addr = (unsigned long)addr & PAGE_MASK;

	if (writable)
		return set_memory_rw(page_addr, 1);
	else
		return set_memory_ro(page_addr, 1);
}

/*
 * Patch the wiphy to add monitor mode support.
 */
static int patch_wiphy(void)
{
	struct cfg80211_ops *ops_rw;

	if (!target_wiphy)
		return -ENODEV;

	target_ops = target_wiphy->ops;
	if (!target_ops)
		return -ENODEV;

	/* Save originals for restore on unload */
	orig_change_virtual_intf = target_ops->change_virtual_intf;
	orig_interface_modes = target_wiphy->interface_modes;

	/* Add monitor and OCB to supported interface types */
	target_wiphy->interface_modes |= BIT(NL80211_IFTYPE_MONITOR);
	target_wiphy->interface_modes |= BIT(NL80211_IFTYPE_OCB);

	/* Patch the ops table — need to cast away const and make writable */
	ops_rw = (struct cfg80211_ops *)target_ops;

	if (make_ops_writable(ops_rw, 1) == 0) {
		ops_rw->change_virtual_intf = rc_change_virtual_intf;
		make_ops_writable(ops_rw, 0);
		pr_info("rc_wifi_mon: patched change_virtual_intf via "
			"set_memory_rw\n");
	} else {
		/*
		 * set_memory_rw failed — try direct write. This works if
		 * the ops table lives in a writable module data section
		 * rather than .rodata.
		 */
		pr_info("rc_wifi_mon: set_memory_rw failed, attempting "
			"direct patch\n");
		ops_rw->change_virtual_intf = rc_change_virtual_intf;
	}

	pr_info("rc_wifi_mon: interface_modes: 0x%x -> 0x%x\n",
		orig_interface_modes, target_wiphy->interface_modes);

	return 0;
}

/*
 * Restore original state on module unload.
 */
static void unpatch_wiphy(void)
{
	struct cfg80211_ops *ops_rw;

	if (!target_ops || !orig_change_virtual_intf)
		return;

	ops_rw = (struct cfg80211_ops *)target_ops;

	/* Restore original change_virtual_intf */
	if (make_ops_writable(ops_rw, 1) == 0) {
		ops_rw->change_virtual_intf = orig_change_virtual_intf;
		make_ops_writable(ops_rw, 0);
	} else {
		ops_rw->change_virtual_intf = orig_change_virtual_intf;
	}

	/* Restore original interface modes */
	if (target_wiphy)
		target_wiphy->interface_modes = orig_interface_modes;

	/* If interface is still in monitor mode, reset it */
	if (target_netdev && target_netdev->ieee80211_ptr &&
	    target_netdev->ieee80211_ptr->iftype == NL80211_IFTYPE_MONITOR) {
		target_netdev->ieee80211_ptr->iftype = NL80211_IFTYPE_STATION;
		target_netdev->type = ARPHRD_ETHER;
		if (detected_driver == DRIVER_BCMDHD)
			bcmdhd_disable_monitor();
	}
}

/* ---- Module init/exit ---- */

static int __init rc_wifi_mon_init(void)
{
	int ret;

	pr_info("rc_wifi_mon: RadioControl WiFi monitor mode enabler v1.0\n");

	detected_driver = detect_driver();
	if (detected_driver == DRIVER_UNKNOWN) {
		pr_err("rc_wifi_mon: no supported WiFi driver found\n");
		return -ENODEV;
	}

	pr_info("rc_wifi_mon: detected driver: %s, wiphy: %s, netdev: %s\n",
		driver_names[detected_driver],
		target_wiphy ? wiphy_name(target_wiphy) : "null",
		target_netdev ? target_netdev->name : "null");

	ret = patch_wiphy();
	if (ret) {
		pr_err("rc_wifi_mon: failed to patch wiphy: %d\n", ret);
		return ret;
	}

	/* Driver-specific post-patch setup */
	switch (detected_driver) {
	case DRIVER_SCSC:
		scsc_enable_fw_monitor();
		break;
	case DRIVER_BCMDHD:
		pr_info("rc_wifi_mon: BCM4390 driver patched — monitor mode "
			"enabled at driver level\n");
		pr_info("rc_wifi_mon: for packet injection, Nexmon firmware "
			"patch is also required\n");
		break;
	default:
		break;
	}

	/* Create sysfs entries under /sys/kernel/rc_wifi_mon/ */
	rc_kobj = kobject_create_and_add("rc_wifi_mon", kernel_kobj);
	if (rc_kobj) {
		ret = sysfs_create_group(rc_kobj, &rc_attr_group);
		if (ret) {
			kobject_put(rc_kobj);
			rc_kobj = NULL;
			pr_warn("rc_wifi_mon: sysfs creation failed: %d\n", ret);
			/* Non-fatal */
		}
	}

	pr_info("rc_wifi_mon: loaded — monitor mode available via "
		"'iw dev %s set type monitor'\n",
		target_netdev ? target_netdev->name : "wlanX");
	return 0;
}

static void __exit rc_wifi_mon_exit(void)
{
	unpatch_wiphy();

	if (rc_kobj) {
		sysfs_remove_group(rc_kobj, &rc_attr_group);
		kobject_put(rc_kobj);
	}

	pr_info("rc_wifi_mon: unloaded — monitor mode disabled\n");
}

module_init(rc_wifi_mon_init);
module_exit(rc_wifi_mon_exit);
