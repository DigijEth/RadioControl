// SPDX-License-Identifier: GPL-2.0
/*
 * rc_shannon_cmd.ko — Direct Shannon modem command interface
 *
 * Creates /dev/rc_shannon for userspace to send AT commands and
 * read responses from Samsung Shannon modem, bypassing the RIL
 * lock on /dev/umts_router0.
 *
 * On Tensor G5: Shannon 5400 (S5400BUNUELO), paths include
 *   /dev/umts_router, /dev/umts_atc0, /dev/nr_atc0
 *
 * This module:
 *   1. Opens the underlying modem char device from kernel space
 *   2. Creates /dev/rc_shannon as a proxy with proper queuing
 *   3. Multiplexes between RadioControl userspace and the modem
 *   4. Handles URCs (unsolicited result codes) from the modem
 *   5. Prevents RIL from monopolizing the AT channel
 *
 * Why a kernel module instead of just opening the device from userspace?
 *   - The RIL daemon holds /dev/umts_router0 open exclusively
 *   - Even with root, opening it races with RIL and can crash the modem
 *   - This module uses a secondary AT channel (atc0/atc1) that RIL
 *     doesn't claim, or creates a proper multiplexed path
 *
 * Target: Pixel 10 Pro Fold (rango), Tensor G5, kernel 6.6
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
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ioctl.h>
#include <linux/circ_buf.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RadioControl");
MODULE_DESCRIPTION("Shannon modem direct AT command interface");
MODULE_VERSION("1.0");

#define DEVICE_NAME "rc_shannon"
#define CLASS_NAME  "radiocontrol"
#define CMD_BUF_SIZE    4096
#define RESP_BUF_SIZE   8192
#define URC_BUF_SIZE    16384
#define MAX_CLIENTS     4

/* IOCTL commands */
#define RC_SHANNON_MAGIC        'S'
#define RC_SHANNON_AT_CMD       _IOWR(RC_SHANNON_MAGIC, 1, struct rc_at_cmd)
#define RC_SHANNON_GET_URC      _IOR(RC_SHANNON_MAGIC, 2, struct rc_urc_msg)
#define RC_SHANNON_SET_TIMEOUT  _IOW(RC_SHANNON_MAGIC, 3, int)
#define RC_SHANNON_GET_STATUS   _IOR(RC_SHANNON_MAGIC, 4, struct rc_modem_status)
#define RC_SHANNON_FLUSH        _IO(RC_SHANNON_MAGIC, 5)

/* AT command with explicit timeout */
struct rc_at_cmd {
	char     cmd[CMD_BUF_SIZE];
	uint32_t cmd_len;
	char     resp[RESP_BUF_SIZE];
	uint32_t resp_len;
	uint32_t timeout_ms;      /* 0 = use default */
	int32_t  status;           /* 0=OK, -1=ERROR, -2=TIMEOUT, -3=CME ERROR */
};

/* Unsolicited result code */
struct rc_urc_msg {
	char     data[1024];
	uint32_t data_len;
	int32_t  remaining;       /* URCs still queued */
};

/* Modem status info */
struct rc_modem_status {
	char     device_path[128];
	int32_t  connected;
	int32_t  urc_count;
	uint64_t cmds_sent;
	uint64_t cmds_failed;
	uint64_t bytes_tx;
	uint64_t bytes_rx;
};

/* Modem device paths to try, in order of preference */
static const char *modem_paths[] = {
	"/dev/umts_atc0",      /* Secondary AT channel — not claimed by RIL */
	"/dev/umts_atc1",      /* Tertiary AT channel */
	"/dev/nr_atc0",        /* Tensor NR naming */
	"/dev/umts_router",    /* Tensor primary (no trailing 0) */
	"/dev/umts_router0",   /* Exynos primary — last resort */
	NULL
};

static int major;
static struct class *rc_class;
static struct cdev rc_cdev;
static struct device *rc_device;
static struct file *modem_filp;
static char modem_path_used[128];
static DEFINE_MUTEX(cmd_mutex);
static DECLARE_WAIT_QUEUE_HEAD(resp_waitq);
static DECLARE_WAIT_QUEUE_HEAD(urc_waitq);

/* Response buffer for synchronous command/response */
static char resp_buf[RESP_BUF_SIZE];
static int resp_len;
static bool resp_ready;

/* URC circular buffer */
struct urc_entry {
	char data[1024];
	int len;
};
static struct urc_entry urc_ring[64];
static int urc_head;
static int urc_tail;
static DEFINE_SPINLOCK(urc_lock);

/* Statistics */
static uint64_t stat_cmds_sent;
static uint64_t stat_cmds_failed;
static uint64_t stat_bytes_tx;
static uint64_t stat_bytes_rx;

/* Reader thread */
static struct task_struct *reader_thread;
static int default_timeout_ms = 5000;

/* Common URC prefixes from Shannon modems */
static const char *urc_prefixes[] = {
	"+CRING:",  "+CLIP:",   "+CREG:",  "+CGREG:",
	"+CEREG:",  "+C5GREG:", "+CMTI:",  "+CMT:",
	"+CDS:",    "+CUSD:",   "+CCWA:",  "+CSSI:",
	"+CSSU:",   "+COPS:",   "RING",    "NO CARRIER",
	"+CGEV:",   "+CIEV:",   "+AIMS",   "$",
	NULL
};

static bool is_urc(const char *line)
{
	int i;

	/* Skip leading \r\n */
	while (*line == '\r' || *line == '\n')
		line++;

	for (i = 0; urc_prefixes[i]; i++) {
		if (strncmp(line, urc_prefixes[i],
			    strlen(urc_prefixes[i])) == 0)
			return true;
	}
	return false;
}

static void urc_enqueue(const char *data, int len)
{
	unsigned long flags;
	int next;

	spin_lock_irqsave(&urc_lock, flags);
	next = (urc_head + 1) % ARRAY_SIZE(urc_ring);
	if (next == urc_tail) {
		/* Ring full — drop oldest */
		urc_tail = (urc_tail + 1) % ARRAY_SIZE(urc_ring);
	}
	if (len > sizeof(urc_ring[0].data) - 1)
		len = sizeof(urc_ring[0].data) - 1;
	memcpy(urc_ring[urc_head].data, data, len);
	urc_ring[urc_head].data[len] = '\0';
	urc_ring[urc_head].len = len;
	urc_head = next;
	spin_unlock_irqrestore(&urc_lock, flags);

	wake_up_interruptible(&urc_waitq);
}

static int urc_dequeue(struct rc_urc_msg *msg)
{
	unsigned long flags;
	int count;

	spin_lock_irqsave(&urc_lock, flags);
	if (urc_head == urc_tail) {
		spin_unlock_irqrestore(&urc_lock, flags);
		return -EAGAIN;
	}
	msg->data_len = urc_ring[urc_tail].len;
	memcpy(msg->data, urc_ring[urc_tail].data, msg->data_len);
	msg->data[msg->data_len] = '\0';
	urc_tail = (urc_tail + 1) % ARRAY_SIZE(urc_ring);

	/* Count remaining */
	if (urc_head >= urc_tail)
		count = urc_head - urc_tail;
	else
		count = ARRAY_SIZE(urc_ring) - urc_tail + urc_head;
	msg->remaining = count;
	spin_unlock_irqrestore(&urc_lock, flags);

	return 0;
}

static int urc_count(void)
{
	unsigned long flags;
	int count;

	spin_lock_irqsave(&urc_lock, flags);
	if (urc_head >= urc_tail)
		count = urc_head - urc_tail;
	else
		count = ARRAY_SIZE(urc_ring) - urc_tail + urc_head;
	spin_unlock_irqrestore(&urc_lock, flags);
	return count;
}

/*
 * Open the underlying modem device from kernel context.
 */
static struct file *open_modem_device(void)
{
	struct file *f;
	int i;

	for (i = 0; modem_paths[i]; i++) {
		f = filp_open(modem_paths[i], O_RDWR | O_NONBLOCK, 0);
		if (!IS_ERR(f)) {
			strscpy(modem_path_used, modem_paths[i],
				sizeof(modem_path_used));
			pr_info("rc_shannon: opened modem device: %s\n",
				modem_paths[i]);
			return f;
		}
		pr_debug("rc_shannon: %s not available (%ld)\n",
			 modem_paths[i], PTR_ERR(f));
	}

	return ERR_PTR(-ENODEV);
}

/*
 * Send an AT command to the modem and read the response.
 * Separates URCs from command responses.
 */
static int send_at_command(const char *cmd, int cmd_len,
			   char *response, int resp_size, int timeout_ms)
{
	loff_t pos = 0;
	ssize_t written, bytes_read;
	int elapsed = 0;
	int total_read = 0;
	char line_buf[1024];
	int line_pos = 0;
	bool in_response = false;

	if (!modem_filp || IS_ERR(modem_filp))
		return -ENODEV;

	if (timeout_ms <= 0)
		timeout_ms = default_timeout_ms;

	/* Write command to modem */
	written = kernel_write(modem_filp, cmd, cmd_len, &pos);
	if (written < 0) {
		pr_err("rc_shannon: write failed: %zd\n", written);
		stat_cmds_failed++;
		return written;
	}
	stat_bytes_tx += written;
	stat_cmds_sent++;

	/* Read response with timeout, filtering URCs */
	memset(response, 0, resp_size);
	pos = 0;

	while (elapsed < timeout_ms && total_read < resp_size - 1) {
		char tmp[512];

		bytes_read = kernel_read(modem_filp, tmp, sizeof(tmp), &pos);
		if (bytes_read > 0) {
			int i;

			stat_bytes_rx += bytes_read;

			for (i = 0; i < bytes_read; i++) {
				char c = tmp[i];

				/* Build lines to check for URCs */
				if (c == '\n' || line_pos >= sizeof(line_buf) - 1) {
					line_buf[line_pos] = '\0';

					if (line_pos > 0 && is_urc(line_buf)) {
						/* It's a URC — queue it, don't add to response */
						urc_enqueue(line_buf, line_pos);
					} else {
						/* Part of command response */
						if (total_read + line_pos + 1 < resp_size) {
							memcpy(response + total_read,
							       line_buf, line_pos);
							total_read += line_pos;
							response[total_read++] = '\n';
							in_response = true;
						}
					}
					line_pos = 0;
				} else if (c != '\r') {
					line_buf[line_pos++] = c;
				}
			}

			/* Check for final response in accumulated data */
			if (in_response) {
				if (strnstr(response, "OK", total_read) ||
				    strnstr(response, "ERROR", total_read) ||
				    strnstr(response, "+CME ERROR:", total_read) ||
				    strnstr(response, "+CMS ERROR:", total_read))
					break;
			}
		} else {
			msleep(20);
			elapsed += 20;
		}
	}

	/* Flush any remaining partial line */
	if (line_pos > 0) {
		line_buf[line_pos] = '\0';
		if (is_urc(line_buf)) {
			urc_enqueue(line_buf, line_pos);
		} else if (total_read + line_pos < resp_size) {
			memcpy(response + total_read, line_buf, line_pos);
			total_read += line_pos;
		}
	}

	response[total_read] = '\0';

	if (elapsed >= timeout_ms && total_read == 0)
		return -ETIMEDOUT;

	return total_read;
}

/*
 * /dev/rc_shannon file operations
 */
static int rc_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int rc_release(struct inode *inode, struct file *file)
{
	return 0;
}

/*
 * write() — send AT command, response available via read()
 */
static ssize_t rc_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	char cmd_buf[CMD_BUF_SIZE];
	int ret;

	if (count >= CMD_BUF_SIZE - 2)
		return -EINVAL;

	if (copy_from_user(cmd_buf, buf, count))
		return -EFAULT;
	cmd_buf[count] = '\0';

	mutex_lock(&cmd_mutex);

	/* Ensure command ends with \r\n for the Shannon modem */
	if (count >= 2 && cmd_buf[count-2] == '\r' && cmd_buf[count-1] == '\n') {
		/* Already terminated */
	} else if (count >= 1 && cmd_buf[count-1] == '\r') {
		cmd_buf[count] = '\n';
		count++;
	} else if (count >= 1 && cmd_buf[count-1] == '\n') {
		/* Shift to insert \r before \n */
		cmd_buf[count] = cmd_buf[count-1];
		cmd_buf[count-1] = '\r';
		count++;
	} else {
		cmd_buf[count] = '\r';
		cmd_buf[count+1] = '\n';
		count += 2;
	}
	cmd_buf[count] = '\0';

	ret = send_at_command(cmd_buf, count, resp_buf, RESP_BUF_SIZE,
			      default_timeout_ms);
	if (ret >= 0) {
		resp_len = ret;
		resp_ready = true;
		wake_up_interruptible(&resp_waitq);
	} else {
		resp_len = 0;
		resp_ready = false;
	}

	mutex_unlock(&cmd_mutex);

	return ret >= 0 ? (ssize_t)count : ret;
}

/*
 * read() — get the response from the last AT command
 */
static ssize_t rc_read(struct file *file, char __user *buf,
		       size_t count, loff_t *ppos)
{
	int to_copy;

	if (!resp_ready) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(resp_waitq, resp_ready))
			return -ERESTARTSYS;
	}

	if (!resp_ready)
		return -ERESTARTSYS;

	to_copy = min((int)count, resp_len);
	if (copy_to_user(buf, resp_buf, to_copy))
		return -EFAULT;

	resp_ready = false;
	return to_copy;
}

static __poll_t rc_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &resp_waitq, wait);
	poll_wait(file, &urc_waitq, wait);

	if (resp_ready)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (urc_count() > 0)
		mask |= EPOLLPRI;  /* URCs available via ioctl */

	return mask;
}

/*
 * ioctl — structured AT command interface
 */
static long rc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case RC_SHANNON_AT_CMD: {
		struct rc_at_cmd *at;

		at = kmalloc(sizeof(*at), GFP_KERNEL);
		if (!at)
			return -ENOMEM;

		if (copy_from_user(at, (void __user *)arg, sizeof(*at))) {
			kfree(at);
			return -EFAULT;
		}

		/* Sanity checks */
		if (at->cmd_len == 0 || at->cmd_len >= CMD_BUF_SIZE) {
			kfree(at);
			return -EINVAL;
		}
		at->cmd[at->cmd_len] = '\0';

		/* Ensure \r\n termination */
		if (at->cmd_len < 2 ||
		    at->cmd[at->cmd_len-2] != '\r' ||
		    at->cmd[at->cmd_len-1] != '\n') {
			at->cmd[at->cmd_len++] = '\r';
			at->cmd[at->cmd_len++] = '\n';
			at->cmd[at->cmd_len] = '\0';
		}

		mutex_lock(&cmd_mutex);
		ret = send_at_command(at->cmd, at->cmd_len,
				      at->resp, sizeof(at->resp),
				      at->timeout_ms ? at->timeout_ms :
						       default_timeout_ms);
		mutex_unlock(&cmd_mutex);

		if (ret >= 0) {
			at->resp_len = ret;
			/* Determine status from response */
			if (strnstr(at->resp, "OK", ret))
				at->status = 0;
			else if (strnstr(at->resp, "+CME ERROR:", ret))
				at->status = -3;
			else if (strnstr(at->resp, "+CMS ERROR:", ret))
				at->status = -3;
			else if (strnstr(at->resp, "ERROR", ret))
				at->status = -1;
			else
				at->status = 0;  /* Got data but no final result */
		} else if (ret == -ETIMEDOUT) {
			at->resp_len = 0;
			at->status = -2;
			at->resp[0] = '\0';
			ret = 0;  /* ioctl succeeded, timeout is in status */
		} else {
			at->status = ret;
			at->resp_len = 0;
		}

		if (copy_to_user((void __user *)arg, at, sizeof(*at)))
			ret = -EFAULT;
		else
			ret = 0;

		kfree(at);
		break;
	}

	case RC_SHANNON_GET_URC: {
		struct rc_urc_msg msg;

		ret = urc_dequeue(&msg);
		if (ret == -EAGAIN) {
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			if (wait_event_interruptible(urc_waitq,
						     urc_count() > 0))
				return -ERESTARTSYS;
			ret = urc_dequeue(&msg);
			if (ret)
				return ret;
		}

		if (copy_to_user((void __user *)arg, &msg, sizeof(msg)))
			return -EFAULT;
		ret = 0;
		break;
	}

	case RC_SHANNON_SET_TIMEOUT: {
		int timeout;

		if (get_user(timeout, (int __user *)arg))
			return -EFAULT;
		if (timeout < 100 || timeout > 60000)
			return -EINVAL;
		default_timeout_ms = timeout;
		ret = 0;
		break;
	}

	case RC_SHANNON_GET_STATUS: {
		struct rc_modem_status st;

		memset(&st, 0, sizeof(st));
		strscpy(st.device_path, modem_path_used,
			sizeof(st.device_path));
		st.connected = (modem_filp && !IS_ERR(modem_filp)) ? 1 : 0;
		st.urc_count = urc_count();
		st.cmds_sent = stat_cmds_sent;
		st.cmds_failed = stat_cmds_failed;
		st.bytes_tx = stat_bytes_tx;
		st.bytes_rx = stat_bytes_rx;

		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		ret = 0;
		break;
	}

	case RC_SHANNON_FLUSH: {
		unsigned long flags;

		spin_lock_irqsave(&urc_lock, flags);
		urc_head = 0;
		urc_tail = 0;
		spin_unlock_irqrestore(&urc_lock, flags);

		resp_ready = false;
		resp_len = 0;
		ret = 0;
		break;
	}

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations rc_fops = {
	.owner          = THIS_MODULE,
	.open           = rc_open,
	.release        = rc_release,
	.read           = rc_read,
	.write          = rc_write,
	.poll           = rc_poll,
	.unlocked_ioctl = rc_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static int __init rc_shannon_init(void)
{
	int ret;
	dev_t dev;

	pr_info("rc_shannon: initializing Shannon modem command interface\n");

	/* Open modem device */
	modem_filp = open_modem_device();
	if (IS_ERR(modem_filp)) {
		pr_err("rc_shannon: no modem device found — Shannon modem "
		       "not present or not accessible\n");
		modem_filp = NULL;
		/*
		 * Don't fail — create the device node anyway so userspace
		 * gets a clear -ENODEV on read/write rather than ENOENT
		 * on open. The modem may come up later (e.g., after SIM
		 * unlock or airplane mode toggle).
		 */
	}

	/* Register char device */
	ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (ret < 0)
		goto err_chrdev;
	major = MAJOR(dev);

	/*
	 * class_create() signature changed in kernel 6.4:
	 *   6.4+:  class_create(name)
	 *   <6.4:  class_create(THIS_MODULE, name)
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	rc_class = class_create(CLASS_NAME);
#else
	rc_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
	if (IS_ERR(rc_class)) {
		ret = PTR_ERR(rc_class);
		goto err_class;
	}

	cdev_init(&rc_cdev, &rc_fops);
	rc_cdev.owner = THIS_MODULE;

	ret = cdev_add(&rc_cdev, MKDEV(major, 0), 1);
	if (ret < 0)
		goto err_cdev;

	rc_device = device_create(rc_class, NULL, MKDEV(major, 0),
				  NULL, DEVICE_NAME);
	if (IS_ERR(rc_device)) {
		ret = PTR_ERR(rc_device);
		goto err_device;
	}

	pr_info("rc_shannon: /dev/%s created (major %d)\n",
		DEVICE_NAME, major);

	if (modem_filp) {
		/* Verify modem is responsive */
		char test_resp[256];
		int test_ret;

		mutex_lock(&cmd_mutex);
		test_ret = send_at_command("AT\r\n", 4, test_resp,
					  sizeof(test_resp), 2000);
		mutex_unlock(&cmd_mutex);

		if (test_ret > 0 && strnstr(test_resp, "OK", test_ret))
			pr_info("rc_shannon: modem responsive (AT -> OK)\n");
		else
			pr_warn("rc_shannon: modem opened but AT test "
				"failed (ret=%d) — may need SELinux permissive\n",
				test_ret);
	}

	return 0;

err_device:
	cdev_del(&rc_cdev);
err_cdev:
	class_destroy(rc_class);
err_class:
	unregister_chrdev_region(MKDEV(major, 0), 1);
err_chrdev:
	if (modem_filp)
		filp_close(modem_filp, NULL);
	return ret;
}

static void __exit rc_shannon_exit(void)
{
	device_destroy(rc_class, MKDEV(major, 0));
	cdev_del(&rc_cdev);
	class_destroy(rc_class);
	unregister_chrdev_region(MKDEV(major, 0), 1);

	if (modem_filp)
		filp_close(modem_filp, NULL);

	pr_info("rc_shannon: unloaded — sent %llu commands, "
		"%llu bytes tx, %llu bytes rx\n",
		stat_cmds_sent, stat_bytes_tx, stat_bytes_rx);
}

module_init(rc_shannon_init);
module_exit(rc_shannon_exit);
