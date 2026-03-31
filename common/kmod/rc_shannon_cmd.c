// SPDX-License-Identifier: GPL-2.0
/*
 * rc_shannon_cmd.ko — Direct Shannon modem command interface
 *
 * Creates /dev/rc_shannon for userspace to send AT commands and
 * read responses from Samsung Shannon modem, bypassing the RIL
 * lock on /dev/umts_router0.
 *
 * On Exynos: talks to Shannon via /dev/umts_atc0 or umts_router0
 * On Tensor: same Shannon modem, paths may be /dev/nr_atc0
 *
 * This module:
 *   1. Opens the underlying modem char device from kernel space
 *   2. Creates /dev/rc_shannon as a proxy with proper queuing
 *   3. Multiplexes between RadioControl userspace and the modem
 *   4. Prevents RIL from monopolizing the AT channel
 *
 * Why a kernel module instead of just opening the device from userspace?
 *   - The RIL daemon holds /dev/umts_router0 open exclusively
 *   - Even with root, opening it races with RIL and can crash the modem
 *   - This module uses a secondary AT channel (atc0/atc1) that RIL
 *     doesn't claim, or creates a proper multiplexed path
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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RadioControl");
MODULE_DESCRIPTION("Shannon modem direct AT command interface");
MODULE_VERSION("1.0");

#define DEVICE_NAME "rc_shannon"
#define CLASS_NAME  "radiocontrol"
#define BUF_SIZE    4096

/* Modem device paths to try, in order of preference */
static const char *modem_paths[] = {
	"/dev/umts_atc0",      /* Secondary AT channel — not claimed by RIL */
	"/dev/umts_atc1",      /* Tertiary AT channel */
	"/dev/nr_atc0",        /* Tensor NR naming */
	"/dev/umts_router0",   /* Primary — last resort, RIL conflict risk */
	NULL
};

static int major;
static struct class *rc_class;
static struct cdev rc_cdev;
static struct device *rc_device;
static struct file *modem_filp;
static DEFINE_MUTEX(cmd_mutex);
static DECLARE_WAIT_QUEUE_HEAD(resp_waitq);

/* Response buffer */
static char resp_buf[BUF_SIZE];
static int resp_len;
static bool resp_ready;

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
 */
static int send_at_command(const char *cmd, int cmd_len,
			   char *response, int resp_size)
{
	loff_t pos = 0;
	ssize_t written, bytes_read;
	int timeout_ms = 3000;
	int elapsed = 0;
	int total_read = 0;

	if (!modem_filp || IS_ERR(modem_filp))
		return -ENODEV;

	/* Write command to modem */
	written = kernel_write(modem_filp, cmd, cmd_len, &pos);
	if (written < 0) {
		pr_err("rc_shannon: write failed: %zd\n", written);
		return written;
	}

	/* Read response with timeout */
	memset(response, 0, resp_size);
	pos = 0;

	while (elapsed < timeout_ms && total_read < resp_size - 1) {
		bytes_read = kernel_read(modem_filp, response + total_read,
					 resp_size - 1 - total_read, &pos);
		if (bytes_read > 0) {
			total_read += bytes_read;
			/* Check for final response */
			if (strnstr(response, "\r\nOK\r\n", total_read) ||
			    strnstr(response, "\r\nERROR\r\n", total_read) ||
			    strnstr(response, "\r\n+CME ERROR:", total_read))
				break;
		} else {
			msleep(50);
			elapsed += 50;
		}
	}

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

static ssize_t rc_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	char cmd_buf[BUF_SIZE];
	int ret;

	if (count >= BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(cmd_buf, buf, count))
		return -EFAULT;
	cmd_buf[count] = '\0';

	mutex_lock(&cmd_mutex);

	/* Ensure command ends with \r\n */
	if (count >= 2 && cmd_buf[count-2] == '\r' && cmd_buf[count-1] == '\n') {
		/* Already terminated */
	} else if (count >= 1 && cmd_buf[count-1] == '\r') {
		cmd_buf[count] = '\n';
		count++;
	} else {
		cmd_buf[count] = '\r';
		cmd_buf[count+1] = '\n';
		count += 2;
	}
	cmd_buf[count] = '\0';

	ret = send_at_command(cmd_buf, count, resp_buf, BUF_SIZE);
	if (ret >= 0) {
		resp_len = ret;
		resp_ready = true;
		wake_up_interruptible(&resp_waitq);
	}

	mutex_unlock(&cmd_mutex);

	return ret >= 0 ? count : ret;
}

static ssize_t rc_read(struct file *file, char __user *buf,
		       size_t count, loff_t *ppos)
{
	int to_copy;

	if (!resp_ready) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(resp_waitq, resp_ready);
	}

	if (!resp_ready)
		return -ERESTARTSYS;

	to_copy = min((int)count, resp_len);
	if (copy_to_user(buf, resp_buf, to_copy))
		return -EFAULT;

	resp_ready = false;
	return to_copy;
}

static unsigned int rc_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = POLLOUT | POLLWRNORM;

	poll_wait(file, &resp_waitq, wait);
	if (resp_ready)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static const struct file_operations rc_fops = {
	.owner   = THIS_MODULE,
	.open    = rc_open,
	.release = rc_release,
	.read    = rc_read,
	.write   = rc_write,
	.poll    = rc_poll,
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
		/* Don't fail — we'll create the device node anyway
		 * so userspace gets a clear error on read/write */
	}

	/* Register char device */
	ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (ret < 0)
		goto err_chrdev;
	major = MAJOR(dev);

	rc_class = class_create(THIS_MODULE, CLASS_NAME);
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

	/* Make device world-accessible (root context anyway) */
	pr_info("rc_shannon: /dev/%s created (major %d)\n",
		DEVICE_NAME, major);
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

	pr_info("rc_shannon: unloaded\n");
}

module_init(rc_shannon_init);
module_exit(rc_shannon_exit);
