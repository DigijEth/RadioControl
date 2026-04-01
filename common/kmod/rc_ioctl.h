/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * rc_ioctl.h — Shared ioctl definitions for RadioControl kernel modules
 *
 * Include from userspace to use the ioctl interfaces on:
 *   /dev/rc_shannon  (Shannon modem AT commands)
 *   /dev/rc_diag     (Qualcomm DIAG protocol)
 */

#ifndef _RC_IOCTL_H
#define _RC_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* ---- /dev/rc_shannon ioctls ---- */

#define RC_SHANNON_MAGIC        'S'

struct rc_at_cmd {
	char     cmd[4096];
	__u32    cmd_len;
	char     resp[8192];
	__u32    resp_len;
	__u32    timeout_ms;      /* 0 = use default (5000ms) */
	__s32    status;           /* 0=OK, -1=ERROR, -2=TIMEOUT, -3=CME ERROR */
};

struct rc_urc_msg {
	char     data[1024];
	__u32    data_len;
	__s32    remaining;       /* URCs still queued */
};

struct rc_modem_status {
	char     device_path[128];
	__s32    connected;
	__s32    urc_count;
	__u64    cmds_sent;
	__u64    cmds_failed;
	__u64    bytes_tx;
	__u64    bytes_rx;
};

#define RC_SHANNON_AT_CMD       _IOWR(RC_SHANNON_MAGIC, 1, struct rc_at_cmd)
#define RC_SHANNON_GET_URC      _IOR(RC_SHANNON_MAGIC, 2, struct rc_urc_msg)
#define RC_SHANNON_SET_TIMEOUT  _IOW(RC_SHANNON_MAGIC, 3, int)
#define RC_SHANNON_GET_STATUS   _IOR(RC_SHANNON_MAGIC, 4, struct rc_modem_status)
#define RC_SHANNON_FLUSH        _IO(RC_SHANNON_MAGIC, 5)

/* ---- /dev/rc_diag ioctls ---- */

#define RC_DIAG_MAGIC           'D'

struct rc_nv_item {
	__u16    id;
	__u16    status;          /* 0 = success */
	__u8     data[128];
	__u32    data_len;
};

struct rc_diag_raw {
	__u8     cmd[8192];
	__u32    cmd_len;
	__u8     resp[8192];
	__u32    resp_len;
};

struct rc_ftm_cmd {
	__u16    cmd_id;
	__u16    data_len;
	__u8     data[512];
	__u16    status;
	__u8     resp[512];
	__u16    resp_len;
};

struct rc_efs_op {
	char     path[256];
	__u8     data[4096];
	__u32    data_len;
	__s32    status;
	__u32    mode;            /* file mode for open/mkdir */
	__u32    offset;          /* read/write offset */
};

struct rc_diag_version {
	char     comp_date[12];
	char     comp_time[8];
	char     rel_date[12];
	char     rel_time[8];
	char     model[32];
	__u8     mob_sw_rev;
};

#define RC_DIAG_NV_READ       _IOWR(RC_DIAG_MAGIC, 1, struct rc_nv_item)
#define RC_DIAG_NV_WRITE      _IOW(RC_DIAG_MAGIC, 2, struct rc_nv_item)
#define RC_DIAG_RAW_CMD       _IOWR(RC_DIAG_MAGIC, 3, struct rc_diag_raw)
#define RC_DIAG_FTM_CMD       _IOWR(RC_DIAG_MAGIC, 4, struct rc_ftm_cmd)
#define RC_DIAG_EFS_READ      _IOWR(RC_DIAG_MAGIC, 5, struct rc_efs_op)
#define RC_DIAG_EFS_WRITE     _IOW(RC_DIAG_MAGIC, 6, struct rc_efs_op)
#define RC_DIAG_EFS_STAT      _IOWR(RC_DIAG_MAGIC, 7, struct rc_efs_op)
#define RC_DIAG_EFS_UNLINK    _IOW(RC_DIAG_MAGIC, 8, struct rc_efs_op)
#define RC_DIAG_GET_VERSION   _IOR(RC_DIAG_MAGIC, 9, struct rc_diag_version)

#endif /* _RC_IOCTL_H */
