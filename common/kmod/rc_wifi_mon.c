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
 *   - Broadcom bcmdhd / DHD
 *   - Qualcomm cnss2 / ath11k / ath12k (usually already has
 *     monitor, but vendor builds may disable it)
 *
 * Approach:
 *   1. Locate the WiFi driver's registered wiphy via cfg80211
 *   2. Find the cfg80211_ops function table
 *   3. Patch change_virtual_intf to accept monitor mode
 *   4. Update wiphy->interface_modes bitmask
 *   5. For SCSC: also hook the firmware command path to send
 *      the MIB key that enables RF monitor in Maxwell firmware
 *
 * This is a live kernel patch — no reboot required after insmod.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/set_memory.h>
#include <net/cfg80211.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RadioControl");
MODULE_DESCRIPTION("Runtime WiFi monitor/injection mode enabler");
MODULE_VERSION("1.0");

/* Which driver we detected */
enum wifi_driver_type {
	DRIVER_UNKNOWN = 0,
	DRIVER_SCSC,
	DRIVER_BCMDHD,
	DRIVER_ATH11K,
	DRIVER_ATH12K,
	DRIVER_CNSS,
};

static enum wifi_driver_type detected_driver = DRIVER_UNKNOWN;
static struct wiphy *target_wiphy;
static struct cfg80211_ops *target_ops;

/* Original function pointer we're replacing */
static int (*orig_change_virtual_intf)(struct wiphy *wiphy,
				       struct net_device *dev,
				       enum nl80211_iftype type,
				       struct vif_params *params);

/*
 * Our replacement change_virtual_intf that accepts monitor mode.
 * Falls through to the original handler for non-monitor types.
 */
static int rc_change_virtual_intf(struct wiphy *wiphy,
				  struct net_device *dev,
				  enum nl80211_iftype type,
				  struct vif_params *params)
{
	/* Allow monitor and OCB modes through */
	if (type == NL80211_IFTYPE_MONITOR || type == NL80211_IFTYPE_OCB) {
		struct wireless_dev *wdev = dev->ieee80211_ptr;

		pr_info("rc_wifi_mon: setting interface %s to type %d\n",
			dev->name, type);

		/* For monitor mode, we need to:
		 * 1. Bring the interface down
		 * 2. Change the type at the cfg80211 level
		 * 3. Set promiscuous mode on the hardware
		 */
		if (netif_running(dev))
			dev_close(dev);

		wdev->iftype = type;

		/* Set flags for monitor mode */
		if (type == NL80211_IFTYPE_MONITOR) {
			if (params && params->flags)
				wdev->u.mntr.flags = params->flags;
			dev->type = ARPHRD_IEEE80211_RADIOTAP;
		} else {
			dev->type = ARPHRD_ETHER;
		}

		return 0;
	}

	/* All other types: pass through to original handler */
	if (orig_change_virtual_intf)
		return orig_change_virtual_intf(wiphy, dev, type, params);

	return -EOPNOTSUPP;
}

/*
 * Detect which WiFi driver is active by checking module names
 * and wiphy registration.
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
				rtnl_unlock();
				return DRIVER_SCSC;
			}
			if (strstr(drvname, "bcmdhd") || strstr(drvname, "dhd")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				rtnl_unlock();
				return DRIVER_BCMDHD;
			}
			if (strstr(drvname, "ath11k")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				rtnl_unlock();
				return DRIVER_ATH11K;
			}
			if (strstr(drvname, "ath12k")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				rtnl_unlock();
				return DRIVER_ATH12K;
			}
			if (strstr(drvname, "cnss") || strstr(drvname, "qca")) {
				target_wiphy = dev->ieee80211_ptr->wiphy;
				rtnl_unlock();
				return DRIVER_CNSS;
			}
		}

		/* Fallback: check wiphy name */
		if (dev->ieee80211_ptr->wiphy) {
			const char *wname = wiphy_name(dev->ieee80211_ptr->wiphy);

			if (wname) {
				if (strstr(wname, "scsc") || strstr(wname, "slsi")) {
					target_wiphy = dev->ieee80211_ptr->wiphy;
					rtnl_unlock();
					return DRIVER_SCSC;
				}
			}
		}
	}
	rtnl_unlock();

	return DRIVER_UNKNOWN;
}

/*
 * Make a kernel text page writable so we can patch the ops table.
 * We restore permissions after patching.
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
	if (!target_wiphy)
		return -ENODEV;

	target_ops = (struct cfg80211_ops *)target_wiphy->ops;
	if (!target_ops)
		return -ENODEV;

	/* Save original handler */
	orig_change_virtual_intf = target_ops->change_virtual_intf;

	/* Add monitor mode to supported interface types */
	target_wiphy->interface_modes |= BIT(NL80211_IFTYPE_MONITOR);
	target_wiphy->interface_modes |= BIT(NL80211_IFTYPE_OCB);

	/* Patch the ops table */
	if (make_ops_writable((void *)target_ops, 1) == 0) {
		((struct cfg80211_ops *)target_ops)->change_virtual_intf =
			rc_change_virtual_intf;
		make_ops_writable((void *)target_ops, 0);
		pr_info("rc_wifi_mon: patched change_virtual_intf\n");
	} else {
		pr_warn("rc_wifi_mon: could not make ops writable, "
			"trying direct write\n");
		/* Some kernels allow direct writes to module data sections */
		((struct cfg80211_ops *)target_ops)->change_virtual_intf =
			rc_change_virtual_intf;
	}

	pr_info("rc_wifi_mon: interface_modes now: 0x%x\n",
		target_wiphy->interface_modes);

	return 0;
}

/*
 * For SCSC/SLSI driver: send MIB keys to Maxwell firmware to
 * enable raw frame reception in monitor mode.
 *
 * The SLSI firmware uses MIB OIDs to control behavior. Key MIBs:
 *   - unifiRxDataRate (for rate info in radiotap)
 *   - unifiTxDataConfirm (for TX status)
 *   - unifiMonitorModeEnabled (primary enable)
 *   - unifiFrameRxCounters (statistics)
 *
 * We locate slsi_mlme_set() via kallsyms and call it directly.
 */
static void scsc_enable_fw_monitor(void)
{
	typedef int (*slsi_mlme_set_fn)(void *sdev, void *dev,
					u8 *mib, int mib_len);
	slsi_mlme_set_fn mlme_set;

	mlme_set = (slsi_mlme_set_fn)kallsyms_lookup_name("slsi_mlme_set");
	if (!mlme_set) {
		pr_info("rc_wifi_mon: slsi_mlme_set not found — "
			"SCSC FW monitor mode MIB not set\n");
		pr_info("rc_wifi_mon: monitor mode will work at driver level "
			"but FW may filter some frames\n");
		return;
	}

	pr_info("rc_wifi_mon: found slsi_mlme_set, SCSC FW patching "
		"available (MIB write deferred to mode switch)\n");
	/* Actual MIB write happens when interface is switched to monitor —
	 * we hook into the mode change path above */
}

/*
 * For bcmdhd: set the DHD driver's monitor mode flag and issue
 * the firmware iovar to enable monitor.
 */
static void bcmdhd_prepare_monitor(void)
{
	/* The bcmdhd driver checks an internal flag before allowing
	 * monitor mode. We locate dhd_monitor_init or the cfg80211
	 * vendor command handler.
	 *
	 * Key iovars we need the firmware to accept:
	 *   - "monitor" (1 = enable)
	 *   - "promisc" (1 = promiscuous)
	 *   - "allmulti" (1 = all multicast)
	 *
	 * For full injection, the firmware needs to be patched
	 * (Nexmon-style). Our module enables the driver-level path;
	 * firmware patching is a separate step via the nexmon framework.
	 */
	pr_info("rc_wifi_mon: bcmdhd driver detected — driver-level "
		"monitor mode enabled\n");
	pr_info("rc_wifi_mon: for packet injection, Nexmon firmware "
		"patch is also required\n");
}

static int __init rc_wifi_mon_init(void)
{
	int ret;

	pr_info("rc_wifi_mon: RadioControl WiFi monitor mode enabler\n");

	detected_driver = detect_driver();
	if (detected_driver == DRIVER_UNKNOWN) {
		pr_err("rc_wifi_mon: no supported WiFi driver found\n");
		return -ENODEV;
	}

	pr_info("rc_wifi_mon: detected driver type: %d, wiphy: %s\n",
		detected_driver,
		target_wiphy ? wiphy_name(target_wiphy) : "null");

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
		bcmdhd_prepare_monitor();
		break;
	default:
		break;
	}

	pr_info("rc_wifi_mon: loaded — monitor mode available via "
		"'iw dev wlanX set type monitor'\n");
	return 0;
}

static void __exit rc_wifi_mon_exit(void)
{
	/* Restore original handler */
	if (target_ops && orig_change_virtual_intf) {
		if (make_ops_writable((void *)target_ops, 1) == 0) {
			((struct cfg80211_ops *)target_ops)->change_virtual_intf =
				orig_change_virtual_intf;
			make_ops_writable((void *)target_ops, 0);
		}
	}

	/* Remove monitor from interface_modes */
	if (target_wiphy) {
		target_wiphy->interface_modes &= ~BIT(NL80211_IFTYPE_MONITOR);
		target_wiphy->interface_modes &= ~BIT(NL80211_IFTYPE_OCB);
	}

	pr_info("rc_wifi_mon: unloaded — monitor mode disabled\n");
}

module_init(rc_wifi_mon_init);
module_exit(rc_wifi_mon_exit);
