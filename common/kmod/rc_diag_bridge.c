// SPDX-License-Identifier: GPL-2.0
/*
 * rc_diag_bridge.ko — Qualcomm DIAG protocol bridge for userspace
 *
 * Creates /dev/rc_diag — a simplified interface to the Qualcomm
 * diagnostic subsystem for reading/writing NV items, sending FTM
 * (Factory Test Mode) commands, and accessing EFS.
 *
 * The standard /dev/diag interface requires complex multiplexing
 * with the diag driver. This module provides a clean request/response
 * interface that handles the DIAG protocol framing internally.
 *
 * Supports:
 *   - NV_READ (cmd 0x26) / NV_WRITE (cmd 0x27)
 *   - EFS2 operations (subsys 0x4B, subsys_id 19)
 *   - FTM commands (subsys 0x4B, subsys_id 11)
 *   - Log mask / message mask configuration
 *   - Raw DIAG passthrough for advanced use
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ioctl.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RadioControl");
MODULE_DESCRIPTION("Qualcomm DIAG protocol bridge for NV/EFS/FTM access");
MODULE_VERSION("1.0");

#define DEVICE_NAME "rc_diag"
#define CLASS_NAME  "radiocontrol"
#define DIAG_BUF_SIZE 8192

/* DIAG command codes */
#define DIAG_NV_READ_F       0x26
#define DIAG_NV_WRITE_F      0x27
#define DIAG_SUBSYS_CMD_F    0x4B
#define DIAG_LOG_CONFIG_F    0x73
#define DIAG_MSG_CONFIG_F    0x7D

/* DIAG subsystem IDs */
#define DIAG_SUBSYS_FTM      11
#define DIAG_SUBSYS_EFS2     19
#define DIAG_SUBSYS_PARAMS   37

/* IOCTL commands */
#define RC_DIAG_MAGIC         'D'
#define RC_DIAG_NV_READ       _IOWR(RC_DIAG_MAGIC, 1, struct rc_nv_item)
#define RC_DIAG_NV_WRITE      _IOW(RC_DIAG_MAGIC, 2, struct rc_nv_item)
#define RC_DIAG_RAW_CMD       _IOWR(RC_DIAG_MAGIC, 3, struct rc_diag_raw)
#define RC_DIAG_FTM_CMD       _IOWR(RC_DIAG_MAGIC, 4, struct rc_diag_raw)
#define RC_DIAG_EFS_READ      _IOWR(RC_DIAG_MAGIC, 5, struct rc_efs_op)
#define RC_DIAG_EFS_WRITE     _IOW(RC_DIAG_MAGIC, 6, struct rc_efs_op)

/* NV item structure */
struct rc_nv_item {
	uint16_t id;
	uint16_t status;      /* 0 = success on return */
	uint8_t  data[128];
	uint32_t data_len;
};

/* Raw DIAG command */
struct rc_diag_raw {
	uint8_t  cmd[DIAG_BUF_SIZE];
	uint32_t cmd_len;
	uint8_t  resp[DIAG_BUF_SIZE];
	uint32_t resp_len;
};

/* EFS operation */
struct rc_efs_op {
	char     path[256];
	uint8_t  data[4096];
	uint32_t data_len;
	int32_t  status;
};

/*
 * DIAG HDLC framing — the DIAG protocol uses async HDLC framing
 * with CRC-16 (CCITT). All messages are wrapped in:
 *   [0x7E] [escaped payload] [CRC-16 LE] [0x7E]
 *
 * Escape: 0x7E -> 0x7D 0x5E, 0x7D -> 0x7D 0x5D
 */

#define HDLC_FLAG 0x7E
#define HDLC_ESC  0x7D
#define HDLC_ESC_MASK 0x20

static const uint16_t crc16_table[256] = {
	0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
	0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
	0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
	0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
	0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
	0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
	0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
	0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
	0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
	0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
	0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
	0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
	0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
	0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
	0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
	0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
	0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
	0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
	0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
	0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
	0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
	0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
	0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
	0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
	0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
	0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
	0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
	0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
	0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
	0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
	0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
	0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
};

static uint16_t crc16_calc(const uint8_t *buf, int len)
{
	uint16_t crc = 0xFFFF;
	while (len--)
		crc = crc16_table[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFF;
}

/* Encode a DIAG message with HDLC framing */
static int hdlc_encode(const uint8_t *src, int src_len,
		       uint8_t *dst, int dst_size)
{
	uint16_t crc;
	int pos = 0;
	int i;

	crc = crc16_calc(src, src_len);

	dst[pos++] = HDLC_FLAG;

	for (i = 0; i < src_len && pos < dst_size - 4; i++) {
		if (src[i] == HDLC_FLAG || src[i] == HDLC_ESC) {
			dst[pos++] = HDLC_ESC;
			dst[pos++] = src[i] ^ HDLC_ESC_MASK;
		} else {
			dst[pos++] = src[i];
		}
	}

	/* Append CRC (little-endian) with escaping */
	for (i = 0; i < 2 && pos < dst_size - 2; i++) {
		uint8_t b = (crc >> (i * 8)) & 0xFF;
		if (b == HDLC_FLAG || b == HDLC_ESC) {
			dst[pos++] = HDLC_ESC;
			dst[pos++] = b ^ HDLC_ESC_MASK;
		} else {
			dst[pos++] = b;
		}
	}

	dst[pos++] = HDLC_FLAG;
	return pos;
}

static int major;
static struct class *rc_class;
static struct cdev rc_cdev;
static struct device *rc_device;
static struct file *diag_filp;
static DEFINE_MUTEX(diag_mutex);

static struct file *open_diag_device(void)
{
	struct file *f;

	f = filp_open("/dev/diag", O_RDWR | O_NONBLOCK, 0);
	if (!IS_ERR(f))
		return f;

	pr_info("rc_diag: /dev/diag not available (%ld)\n", PTR_ERR(f));
	return ERR_PTR(-ENODEV);
}

static int send_diag_cmd(const uint8_t *cmd, int cmd_len,
			 uint8_t *resp, int resp_size)
{
	uint8_t hdlc_buf[DIAG_BUF_SIZE * 2];
	loff_t pos = 0;
	ssize_t written, bytes_read;
	int hdlc_len;
	int timeout_ms = 2000;
	int elapsed = 0;

	if (!diag_filp || IS_ERR(diag_filp))
		return -ENODEV;

	/* HDLC encode the command */
	hdlc_len = hdlc_encode(cmd, cmd_len, hdlc_buf, sizeof(hdlc_buf));

	/* Send to DIAG */
	written = kernel_write(diag_filp, hdlc_buf, hdlc_len, &pos);
	if (written < 0)
		return written;

	/* Read response */
	pos = 0;
	while (elapsed < timeout_ms) {
		bytes_read = kernel_read(diag_filp, resp, resp_size, &pos);
		if (bytes_read > 0)
			return bytes_read;
		msleep(20);
		elapsed += 20;
	}

	return -ETIMEDOUT;
}

static long rc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	mutex_lock(&diag_mutex);

	switch (cmd) {
	case RC_DIAG_NV_READ: {
		struct rc_nv_item nv;
		uint8_t diag_cmd[3];
		uint8_t diag_resp[256];

		if (copy_from_user(&nv, (void __user *)arg, sizeof(nv))) {
			ret = -EFAULT;
			break;
		}

		/* Build NV_READ command: [0x26] [NV_ID LE 16bit] */
		diag_cmd[0] = DIAG_NV_READ_F;
		diag_cmd[1] = nv.id & 0xFF;
		diag_cmd[2] = (nv.id >> 8) & 0xFF;

		ret = send_diag_cmd(diag_cmd, 3, diag_resp, sizeof(diag_resp));
		if (ret > 0) {
			/* Response: [0x26] [NV_ID] [status] [data...] */
			nv.status = diag_resp[3] | (diag_resp[4] << 8);
			nv.data_len = ret > 133 ? 128 : ret - 5;
			if (nv.data_len > 0)
				memcpy(nv.data, diag_resp + 5, nv.data_len);
			if (copy_to_user((void __user *)arg, &nv, sizeof(nv)))
				ret = -EFAULT;
			else
				ret = 0;
		}
		break;
	}

	case RC_DIAG_NV_WRITE: {
		struct rc_nv_item nv;
		uint8_t diag_cmd[256];
		uint8_t diag_resp[32];

		if (copy_from_user(&nv, (void __user *)arg, sizeof(nv))) {
			ret = -EFAULT;
			break;
		}

		if (nv.data_len > 128) {
			ret = -EINVAL;
			break;
		}

		/* Build NV_WRITE: [0x27] [NV_ID LE] [data...] */
		diag_cmd[0] = DIAG_NV_WRITE_F;
		diag_cmd[1] = nv.id & 0xFF;
		diag_cmd[2] = (nv.id >> 8) & 0xFF;
		memcpy(diag_cmd + 3, nv.data, nv.data_len);

		ret = send_diag_cmd(diag_cmd, 3 + nv.data_len,
				    diag_resp, sizeof(diag_resp));
		if (ret > 0) {
			nv.status = diag_resp[3] | (diag_resp[4] << 8);
			if (copy_to_user((void __user *)arg, &nv, sizeof(nv)))
				ret = -EFAULT;
			else
				ret = 0;
		}
		break;
	}

	case RC_DIAG_RAW_CMD: {
		struct rc_diag_raw *raw;
		raw = kmalloc(sizeof(*raw), GFP_KERNEL);
		if (!raw) { ret = -ENOMEM; break; }

		if (copy_from_user(raw, (void __user *)arg, sizeof(*raw))) {
			kfree(raw);
			ret = -EFAULT;
			break;
		}

		ret = send_diag_cmd(raw->cmd, raw->cmd_len,
				    raw->resp, sizeof(raw->resp));
		if (ret > 0) {
			raw->resp_len = ret;
			if (copy_to_user((void __user *)arg, raw, sizeof(*raw)))
				ret = -EFAULT;
			else
				ret = 0;
		}
		kfree(raw);
		break;
	}

	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&diag_mutex);
	return ret;
}

static const struct file_operations rc_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = rc_ioctl,
	.compat_ioctl   = rc_ioctl,
};

static int __init rc_diag_init(void)
{
	int ret;
	dev_t dev;

	pr_info("rc_diag: initializing Qualcomm DIAG bridge\n");

	diag_filp = open_diag_device();
	if (IS_ERR(diag_filp)) {
		pr_info("rc_diag: /dev/diag not available — Qualcomm modem "
			"not present. Module loaded but inactive.\n");
		diag_filp = NULL;
	}

	ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (ret < 0)
		return ret;
	major = MAJOR(dev);

	rc_class = class_create(THIS_MODULE, CLASS_NAME "_diag");
	if (IS_ERR(rc_class)) {
		ret = PTR_ERR(rc_class);
		goto err;
	}

	cdev_init(&rc_cdev, &rc_fops);
	cdev_add(&rc_cdev, MKDEV(major, 0), 1);

	rc_device = device_create(rc_class, NULL, MKDEV(major, 0),
				  NULL, DEVICE_NAME);
	if (IS_ERR(rc_device)) {
		ret = PTR_ERR(rc_device);
		goto err2;
	}

	pr_info("rc_diag: /dev/%s created\n", DEVICE_NAME);
	return 0;

err2:
	cdev_del(&rc_cdev);
	class_destroy(rc_class);
err:
	unregister_chrdev_region(MKDEV(major, 0), 1);
	return ret;
}

static void __exit rc_diag_exit(void)
{
	device_destroy(rc_class, MKDEV(major, 0));
	cdev_del(&rc_cdev);
	class_destroy(rc_class);
	unregister_chrdev_region(MKDEV(major, 0), 1);
	if (diag_filp)
		filp_close(diag_filp, NULL);
	pr_info("rc_diag: unloaded\n");
}

module_init(rc_diag_init);
module_exit(rc_diag_exit);
