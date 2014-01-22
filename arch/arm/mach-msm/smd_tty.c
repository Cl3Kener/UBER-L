/* arch/arm/mach-msm/smd_tty.c
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 * Author: Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/platform_device.h>
#include <linux/sched.h>

#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

#include <mach/msm_smd.h>
#include <mach/subsystem_restart.h>
#include <mach/socinfo.h>
#include <mach/msm_ipc_logging.h>

#include "smd_private.h"

#define MAX_SMD_TTYS 37
#define MAX_TTY_BUF_SIZE 2048
#define MAX_RA_WAKE_LOCK_NAME_LEN 32
#define SMD_TTY_LOG_PAGES 2

#define SMD_TTY_INFO(buf...) \
do { \
	if (smd_tty_log_ctx) { \
		ipc_log_string(smd_tty_log_ctx, buf); \
	} \
} while (0)

#define SMD_TTY_ERR(buf...) \
do { \
	if (smd_tty_log_ctx) \
		ipc_log_string(smd_tty_log_ctx, buf); \
	pr_err(buf); \
} while (0)

static void *smd_tty_log_ctx;
static DEFINE_MUTEX(smd_tty_lock);

static uint smd_tty_modem_wait;
module_param_named(modem_wait, smd_tty_modem_wait,
			uint, S_IRUGO | S_IWUSR | S_IWGRP);

#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
static uint smd_tty_ds_modem_wait = 20;
module_param_named(ds_modem_wait, smd_tty_ds_modem_wait,
			uint, S_IRUGO | S_IWUSR | S_IWGRP);
#endif

struct smd_tty_info {
	smd_channel_t *ch;
	struct tty_port port;
	struct device *device_ptr;
	struct wake_lock wake_lock;
	struct tasklet_struct tty_tsklt;
	struct timer_list buf_req_timer;
	struct completion ch_allocated;
	struct platform_driver driver;
	void *pil;
	int in_reset;
	int in_reset_updated;
	int is_open;
	unsigned int open_wait;
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
	int is_dsmodem_ready;
#endif
	wait_queue_head_t ch_opened_wait_queue;
	spinlock_t reset_lock;
	spinlock_t ra_lock;		/* Read Available Lock*/
	char ra_wake_lock_name[MAX_RA_WAKE_LOCK_NAME_LEN];
	struct wake_lock ra_wake_lock;	/* Read Available Wakelock */
	struct smd_config *smd;
};

/**
 * SMD port configuration.
 *
 * @tty_dev_index   Index into smd_tty[]
 * @port_name       Name of the SMD port
 * @dev_name        Name of the TTY Device (if NULL, @port_name is used)
 * @edge            SMD edge
 */
struct smd_config {
	uint32_t tty_dev_index;
	const char *port_name;
	const char *dev_name;
	uint32_t edge;
};

static struct smd_config smd_configs[] = {
	{0, "DS", NULL, SMD_APPS_MODEM},
	{1, "APPS_FM", NULL, SMD_APPS_WCNSS},
	{2, "APPS_RIVA_BT_ACL", NULL, SMD_APPS_WCNSS},
	{3, "APPS_RIVA_BT_CMD", NULL, SMD_APPS_WCNSS},
	{4, "MBALBRIDGE", NULL, SMD_APPS_MODEM},
	{5, "APPS_RIVA_ANT_CMD", NULL, SMD_APPS_WCNSS},
	{6, "APPS_RIVA_ANT_DATA", NULL, SMD_APPS_WCNSS},
	{7, "DATA1", NULL, SMD_APPS_MODEM},
	{8, "DATA4", NULL, SMD_APPS_MODEM},
	{11, "DATA11", NULL, SMD_APPS_MODEM},
	{21, "DATA21", NULL, SMD_APPS_MODEM},
	{27, "GPSNMEA", NULL, SMD_APPS_MODEM},
	{36, "LOOPBACK", "LOOPBACK_TTY", SMD_APPS_MODEM},
};
#define DS_IDX 0
#define LOOPBACK_IDX 36

static struct delayed_work loopback_work;
static struct smd_tty_info smd_tty[MAX_SMD_TTYS];

static int is_in_reset(struct smd_tty_info *info)
{
	return info->in_reset;
}

static void buf_req_retry(unsigned long param)
{
	struct smd_tty_info *info = (struct smd_tty_info *)param;
	unsigned long flags;

	spin_lock_irqsave(&info->reset_lock, flags);
	if (info->is_open) {
		spin_unlock_irqrestore(&info->reset_lock, flags);
		tasklet_hi_schedule(&info->tty_tsklt);
		return;
	}
	spin_unlock_irqrestore(&info->reset_lock, flags);
}

static ssize_t open_timeout_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t n)
{
	unsigned int num_dev;
	unsigned long wait;
	if (dev == NULL) {
		SMD_TTY_INFO("%s: Invalid Device passed", __func__);
		return -EINVAL;
	}
	for (num_dev = 0; num_dev < MAX_SMD_TTYS; num_dev++) {
		if (dev == smd_tty[num_dev].device_ptr)
			break;
	}
	if (num_dev >= MAX_SMD_TTYS) {
		SMD_TTY_ERR("[%s]: Device Not found", __func__);
		return -EINVAL;
	}
	if (!kstrtoul(buf, 10, &wait)) {
		smd_tty[num_dev].open_wait = wait;
		return n;
	} else {
		SMD_TTY_INFO("[%s]: Unable to convert %s to an int",
			__func__, buf);
		return -EINVAL;
	}
}

static ssize_t open_timeout_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	unsigned int num_dev;

	if (dev == NULL) {
		SMD_TTY_INFO("%s: Invalid Device passed", __func__);
		return -EINVAL;
	}
	for (num_dev = 0; num_dev < MAX_SMD_TTYS; num_dev++) {
		if (dev == smd_tty[num_dev].device_ptr)
			break;
	}
	if (num_dev >= MAX_SMD_TTYS) {
		SMD_TTY_ERR("[%s]: Device Not Found", __func__);
		return -EINVAL;
	}

	return snprintf(buf, PAGE_SIZE, "%d\n",
			smd_tty[num_dev].open_wait);
}

static DEVICE_ATTR
	(open_timeout, 0664, open_timeout_show, open_timeout_store);

static void smd_tty_read(unsigned long param)
{
	unsigned char *ptr;
	int avail;
	struct smd_tty_info *info = (struct smd_tty_info *)param;
	struct tty_struct *tty = tty_port_tty_get(&info->port);
	unsigned long flags;

	if (!tty)
		return;

	for (;;) {
		if (is_in_reset(info)) {
			/* signal TTY clients using TTY_BREAK */
			tty_insert_flip_char(tty, 0x00, TTY_BREAK);
			tty_flip_buffer_push(tty);
			break;
		}

		if (test_bit(TTY_THROTTLED, &tty->flags)) break;
		spin_lock_irqsave(&info->ra_lock, flags);
		avail = smd_read_avail(info->ch);
		if (avail == 0) {
			wake_unlock(&info->ra_wake_lock);
			spin_unlock_irqrestore(&info->ra_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&info->ra_lock, flags);

		if (avail > MAX_TTY_BUF_SIZE)
			avail = MAX_TTY_BUF_SIZE;

		avail = tty_prepare_flip_string(tty, &ptr, avail);
		if (avail <= 0) {
			mod_timer(&info->buf_req_timer,
					jiffies + msecs_to_jiffies(30));
			tty_kref_put(tty);
			return;
		}

		if (smd_read(info->ch, ptr, avail) != avail) {
			/* shouldn't be possible since we're in interrupt
			** context here and nobody else could 'steal' our
			** characters.
			*/
			SMD_TTY_ERR(
				"%s - Possible smd_tty_buffer mismatch for %s",
				__func__, info->ch->name);
		}

		wake_lock_timeout(&info->wake_lock, HZ / 2);
		tty_flip_buffer_push(tty);
	}

	/* XXX only when writable and necessary */
	tty_wakeup(tty);
	tty_kref_put(tty);
}

static void smd_tty_notify(void *priv, unsigned event)
{
	struct smd_tty_info *info = priv;
	struct tty_struct *tty;
	unsigned long flags;

	switch (event) {
	case SMD_EVENT_DATA:
		spin_lock_irqsave(&info->reset_lock, flags);
		if (!info->is_open) {
			spin_unlock_irqrestore(&info->reset_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&info->reset_lock, flags);
		/* There may be clients (tty framework) that are blocked
		 * waiting for space to write data, so if a possible read
		 * interrupt came in wake anyone waiting and disable the
		 * interrupts
		 */
		if (smd_write_avail(info->ch)) {
			smd_disable_read_intr(info->ch);
			tty = tty_port_tty_get(&info->port);
			if (tty)
				wake_up_interruptible(&tty->write_wait);
			tty_kref_put(tty);
		}
		spin_lock_irqsave(&info->ra_lock, flags);
		if (smd_read_avail(info->ch)) {
			wake_lock(&info->ra_wake_lock);
			tasklet_hi_schedule(&info->tty_tsklt);
		}
		spin_unlock_irqrestore(&info->ra_lock, flags);
		break;

	case SMD_EVENT_OPEN:
		spin_lock_irqsave(&info->reset_lock, flags);
		info->in_reset = 0;
		info->in_reset_updated = 1;
		info->is_open = 1;
		wake_up_interruptible(&info->ch_opened_wait_queue);
		spin_unlock_irqrestore(&info->reset_lock, flags);
		break;

	case SMD_EVENT_CLOSE:
		spin_lock_irqsave(&info->reset_lock, flags);
		info->in_reset = 1;
		info->in_reset_updated = 1;
		info->is_open = 0;
		wake_up_interruptible(&info->ch_opened_wait_queue);
		spin_unlock_irqrestore(&info->reset_lock, flags);
		/* schedule task to send TTY_BREAK */
		tasklet_hi_schedule(&info->tty_tsklt);

		tty = tty_port_tty_get(&info->port);
		if (tty->index == LOOPBACK_IDX)
			schedule_delayed_work(&loopback_work,
					msecs_to_jiffies(1000));
		tty_kref_put(tty);
		break;
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
		/*
		 * At current smd_tty framework, if smd_tty_open()
		 * is invoked by process before smd_tty_close() is
		 * completely finished, smd_tty_open() may fail
		 * because smd_tty_close() does not wait to close smd
		 * channel from modem. To fix this situation, new SMD
		 * notify status, SMD_EVENT_REOPEN_READY is used.
		 * Until smd_tty receive this status, smd_tty_close()
		 * will be wait(in fact, process will be wait).
		 */
	case SMD_EVENT_REOPEN_READY:
		/* smd channel is closed completely */
		spin_lock_irqsave(&info->reset_lock, flags);
		info->in_reset = 1;
		info->in_reset_updated = 1;
		info->is_open = 0;
		wake_up_interruptible(&info->ch_opened_wait_queue);
		spin_unlock_irqrestore(&info->reset_lock, flags);
		break;
#endif
	}
}

static uint32_t is_modem_smsm_inited(void)
{
	uint32_t modem_state;
	uint32_t ready_state = (SMSM_INIT | SMSM_SMDINIT);

	modem_state = smsm_get_state(SMSM_MODEM_STATE);
	return (modem_state & ready_state) == ready_state;
}

static int smd_tty_port_activate(struct tty_port *tport,
				 struct tty_struct *tty)
{
	int res = 0;
	unsigned int n = tty->index;
	struct smd_tty_info *info;
	const char *peripheral = NULL;


	if (n >= MAX_SMD_TTYS || !smd_tty[n].smd)
		return -ENODEV;

	info = smd_tty + n;

	mutex_lock(&smd_tty_lock);
	tty->driver_data = info;

	peripheral = smd_edge_to_subsystem(smd_tty[n].smd->edge);
	if (peripheral) {
		info->pil = subsystem_get(peripheral);
		if (IS_ERR(info->pil)) {
			SMD_TTY_INFO(
				"%s failed on smd_tty device :%s subsystem_get failed for %s",
				__func__, smd_tty[n].smd->port_name,
				peripheral);

			/*
			 * Sleep, inorder to reduce the frequency of
			 * retry by user-space modules and to avoid
			 * possible watchdog bite.
			 */
			msleep((smd_tty[n].open_wait * 1000));
			res = PTR_ERR(info->pil);
			goto out;
		}

		/* Wait for the modem SMSM to be inited for the SMD
		 * Loopback channel to be allocated at the modem. Since
		 * the wait need to be done atmost once, using msleep
		 * doesn't degrade the performance.
		 */
		if (n == LOOPBACK_IDX) {
			if (!is_modem_smsm_inited())
				msleep(5000);
			smsm_change_state(SMSM_APPS_STATE,
					  0, SMSM_SMD_LOOPBACK);
			msleep(100);
		}

		/*
		 * Wait for a channel to be allocated so we know
		 * the modem is ready enough.
		 */
		if (smd_tty[n].open_wait) {
			res = wait_for_completion_interruptible_timeout(
					&info->ch_allocated,
					msecs_to_jiffies(smd_tty[n].open_wait *
									1000));

			if (res == 0) {
				SMD_TTY_INFO(
					"Timed out waiting for SMD channel %s",
					smd_tty[n].smd->port_name);
				res = -ETIMEDOUT;
				goto release_pil;
			} else if (res < 0) {
				SMD_TTY_INFO(
					"Error waiting for SMD channel %s : %d\n",
					smd_tty[n].smd->port_name, res);
				goto release_pil;
			}
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
			/*
			 * on boot, process tried to open smd0 sleeps until
			 * modem is ready or timeout.
			 */
			if (n == DS_IDX) {
				/* wait for open ready status in seconds */
				pr_info("%s: checking DS modem status\n", __func__);
				res = wait_event_interruptible_timeout(
						info->ch_opened_wait_queue,
						info->is_dsmodem_ready,
						(smd_tty_ds_modem_wait * HZ));
				if (!res) {
					res = -ETIMEDOUT;
					pr_err("%s: timeout to wait for %s modem: %d\n",
							__func__,
							smd_tty[n].smd->port_name,
							res);
					goto release_pil;
				} else if (res < 0) {
					pr_err("%s: Error waiting for %s modem: %d\n",
							__func__,
							smd_tty[n].smd->port_name,
							res);
					goto release_pil;
				}
				pr_info("%s: DS modem is OK, open smd0..\n",
						__func__);
			}
#endif
		}
	}

	tasklet_init(&info->tty_tsklt, smd_tty_read, (unsigned long)info);
	wake_lock_init(&info->wake_lock, WAKE_LOCK_SUSPEND,
			smd_tty[n].smd->port_name);
	scnprintf(info->ra_wake_lock_name, MAX_RA_WAKE_LOCK_NAME_LEN,
		  "SMD_TTY_%s_RA", smd_tty[n].smd->port_name);
	wake_lock_init(&info->ra_wake_lock, WAKE_LOCK_SUSPEND,
			info->ra_wake_lock_name);

	res = smd_named_open_on_edge(smd_tty[n].smd->port_name,
				     smd_tty[n].smd->edge, &info->ch, info,
				     smd_tty_notify);
	if (res < 0) {
		SMD_TTY_INFO("%s: %s open failed %d\n",
			      __func__, smd_tty[n].smd->port_name, res);
		goto release_wl_tl;
	}

	res = wait_event_interruptible_timeout(info->ch_opened_wait_queue,
					       info->is_open, (2 * HZ));
	if (res == 0)
		res = -ETIMEDOUT;
	if (res < 0) {
		SMD_TTY_INFO("%s: wait for %s smd_open failed %d\n",
			      __func__, smd_tty[n].smd->port_name, res);
		goto close_ch;
	}
	SMD_TTY_INFO("%s with PID %u opened port %s",
		      current->comm, current->pid, smd_tty[n].smd->port_name);
	smd_disable_read_intr(info->ch);
	mutex_unlock(&smd_tty_lock);
	return 0;

close_ch:
	smd_close(info->ch);
	info->ch = NULL;

release_wl_tl:
	tasklet_kill(&info->tty_tsklt);
	wake_lock_destroy(&info->wake_lock);
	wake_lock_destroy(&info->ra_wake_lock);

release_pil:
	subsystem_put(info->pil);
out:
	mutex_unlock(&smd_tty_lock);

	return res;
}

static void smd_tty_port_shutdown(struct tty_port *tport)
{
	struct smd_tty_info *info;
	struct tty_struct *tty = tty_port_tty_get(tport);
	unsigned long flags;
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
	int res = 0;
	int n = tty->index;
#endif

	info = tty->driver_data;
	if (info == 0) {
		tty_kref_put(tty);
		return;
	}

	mutex_lock(&smd_tty_lock);

	spin_lock_irqsave(&info->reset_lock, flags);
	info->is_open = 0;
	spin_unlock_irqrestore(&info->reset_lock, flags);

	tasklet_kill(&info->tty_tsklt);
	wake_lock_destroy(&info->wake_lock);
	wake_lock_destroy(&info->ra_wake_lock);

	SMD_TTY_INFO("%s with PID %u closed port %s",
			current->comm, current->pid,
			info->smd->port_name);
	tty->driver_data = NULL;
	del_timer(&info->buf_req_timer);

	smd_close(info->ch);
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
	/*
	 * At current smd_tty framework, if smd_tty_open()
	 * is invoked by process before smd_tty_close() is
	 * completely finished, smd_tty_open() may fail
	 * because smd_tty_close() does not wait to close smd
	 * channel from modem. To fix this situation, new SMD
	 * notify status, SMD_EVENT_REOPEN_READY is used.
	 * Until smd_tty receive this status, smd_tty_close()
	 * will be wait(in fact, process will be wait).
	 */
	pr_info("%s: waiting to close smd %s completely\n",
			__func__, smd_tty[n].smd->port_name);
	/* wait for reopen ready status in seconds */
	res = wait_event_interruptible_timeout(
		info->ch_opened_wait_queue,
		!info->is_open, (smd_tty_ds_modem_wait * HZ));
	if (res <= 0) {
		/* just in case, remain result value */
		pr_err("%s: timeout to wait for %s smd_close. "
				"next smd_open may fail\n",
				__func__, smd_tty[n].smd->port_name);
	}
#endif
	info->ch = NULL;
	subsystem_put(info->pil);

	mutex_unlock(&smd_tty_lock);
	tty_kref_put(tty);
}

static int smd_tty_open(struct tty_struct *tty, struct file *f)
{
	struct smd_tty_info *info = smd_tty + tty->index;

	return tty_port_open(&info->port, tty, f);
}

static void smd_tty_close(struct tty_struct *tty, struct file *f)
{
	struct smd_tty_info *info = tty->driver_data;

	tty_port_close(&info->port, tty, f);
}

static int smd_tty_write(struct tty_struct *tty, const unsigned char *buf, int len)
{
	struct smd_tty_info *info = tty->driver_data;
	int avail;

	/* if we're writing to a packet channel we will
	** never be able to write more data than there
	** is currently space for
	*/
	if (is_in_reset(info))
		return -ENETRESET;

	avail = smd_write_avail(info->ch);
	/* if no space, we'll have to setup a notification later to wake up the
	 * tty framework when space becomes avaliable
	 */
	if (!avail) {
		smd_enable_read_intr(info->ch);
		return 0;
	}
	if (len > avail)
		len = avail;
	SMD_TTY_INFO("[WRITE]: PID %u -> port %s %x bytes",
			current->pid, info->smd->port_name, len);

	return smd_write(info->ch, buf, len);
}

static int smd_tty_write_room(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	return smd_write_avail(info->ch);
}

static int smd_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	return smd_read_avail(info->ch);
}

static void smd_tty_unthrottle(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	unsigned long flags;

	spin_lock_irqsave(&info->reset_lock, flags);
	if (info->is_open) {
		spin_unlock_irqrestore(&info->reset_lock, flags);
		tasklet_hi_schedule(&info->tty_tsklt);
		return;
	}
	spin_unlock_irqrestore(&info->reset_lock, flags);
}

/*
 * Returns the current TIOCM status bits including:
 *      SMD Signals (DTR/DSR, CTS/RTS, CD, RI)
 *      TIOCM_OUT1 - reset state (1=in reset)
 *      TIOCM_OUT2 - reset state updated (1=updated)
 */
static int smd_tty_tiocmget(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	unsigned long flags;
	int tiocm;

	tiocm = smd_tiocmget(info->ch);

	spin_lock_irqsave(&info->reset_lock, flags);
	tiocm |= (info->in_reset ? TIOCM_OUT1 : 0);
	if (info->in_reset_updated) {
		tiocm |= TIOCM_OUT2;
		info->in_reset_updated = 0;
	}
	SMD_TTY_INFO("PID %u --> %s TIOCM is %x ",
			current->pid, __func__, tiocm);
	spin_unlock_irqrestore(&info->reset_lock, flags);

	return tiocm;
}

static int smd_tty_tiocmset(struct tty_struct *tty,
				unsigned int set, unsigned int clear)
{
	struct smd_tty_info *info = tty->driver_data;

	if (info->in_reset)
		return -ENETRESET;

	SMD_TTY_INFO("PID %u --> %s Set: %x Clear: %x",
			current->pid, __func__, set, clear);
	return smd_tiocmset(info->ch, set, clear);
}

static void loopback_probe_worker(struct work_struct *work)
{
	/* wait for modem to restart before requesting loopback server */
	if (!is_modem_smsm_inited())
		schedule_delayed_work(&loopback_work, msecs_to_jiffies(1000));
	else
		smsm_change_state(SMSM_APPS_STATE,
			  0, SMSM_SMD_LOOPBACK);
}

static const struct tty_port_operations smd_tty_port_ops = {
	.shutdown = smd_tty_port_shutdown,
	.activate = smd_tty_port_activate,
};

static struct tty_operations smd_tty_ops = {
	.open = smd_tty_open,
	.close = smd_tty_close,
	.write = smd_tty_write,
	.write_room = smd_tty_write_room,
	.chars_in_buffer = smd_tty_chars_in_buffer,
	.unthrottle = smd_tty_unthrottle,
	.tiocmget = smd_tty_tiocmget,
	.tiocmset = smd_tty_tiocmset,
};

static int smd_tty_dummy_probe(struct platform_device *pdev)
{
	int n;
	int idx;

	for (n = 0; n < ARRAY_SIZE(smd_configs); ++n) {
		idx = smd_configs[n].tty_dev_index;

		if (!smd_configs[n].dev_name)
			continue;

		if (pdev->id == smd_configs[n].edge &&
			!strncmp(pdev->name, smd_configs[n].dev_name,
					SMD_MAX_CH_NAME_LEN)) {
			complete_all(&smd_tty[idx].ch_allocated);
			return 0;
		}
	}
	SMD_TTY_ERR("[ERR]%s: unknown device '%s'\n", __func__, pdev->name);

	return -ENODEV;
}

/**
 * smd_tty_log_init()- Init function for IPC logging
 *
 * Initialize the buffer that is used to provide the log information
 * pertaining to the smd_tty module.
 */
static void smd_tty_log_init(void)
{
	smd_tty_log_ctx = ipc_log_context_create(SMD_TTY_LOG_PAGES,
						"smd_tty");
	if (!smd_tty_log_ctx)
		pr_err("%s: Unable to create IPC log", __func__);
}

#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
static int smd_tty_ds_probe(struct platform_device *pdev)
{
	if (!smd_configs[DS_IDX].dev_name) {
		pr_err("%s: no device for DS\n", __func__);
		return -EINVAL;
	}

	if (strncmp(pdev->name, smd_configs[DS_IDX].dev_name,
				SMD_MAX_CH_NAME_LEN)) {
		pr_err("%s: smd device is not DS %s %s\n", __func__,
				pdev->name, smd_configs[DS_IDX].dev_name);
		return -EINVAL;
	}

	complete_all(&smd_tty[DS_IDX].ch_allocated);
	smd_tty[DS_IDX].is_dsmodem_ready = 1;
	wake_up_interruptible(&smd_tty[DS_IDX].ch_opened_wait_queue);
	pr_info("%s: probing of smd tty for DS modem is done\n", __func__);
	return 0;
}
#endif

static struct tty_driver *smd_tty_driver;

static int __init smd_tty_init(void)
{
	int ret;
	int n;
	int idx;
	struct tty_port *port;

	smd_tty_log_init();
	smd_tty_driver = alloc_tty_driver(MAX_SMD_TTYS);
	if (smd_tty_driver == 0) {
		SMD_TTY_ERR("%s - Driver allocation failed", __func__);
		return -ENOMEM;
	}

	smd_tty_driver->owner = THIS_MODULE;
	smd_tty_driver->driver_name = "smd_tty_driver";
	smd_tty_driver->name = "smd";
	smd_tty_driver->major = 0;
	smd_tty_driver->minor_start = 0;
	smd_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	smd_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	smd_tty_driver->init_termios = tty_std_termios;
	smd_tty_driver->init_termios.c_iflag = 0;
	smd_tty_driver->init_termios.c_oflag = 0;
	smd_tty_driver->init_termios.c_cflag = B38400 | CS8 | CREAD;
	smd_tty_driver->init_termios.c_lflag = 0;
	smd_tty_driver->flags = TTY_DRIVER_RESET_TERMIOS |
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	tty_set_operations(smd_tty_driver, &smd_tty_ops);

	ret = tty_register_driver(smd_tty_driver);
	if (ret) {
		put_tty_driver(smd_tty_driver);
		SMD_TTY_ERR("%s: driver registration failed %d", __func__, ret);
		return ret;
	}

	for (n = 0; n < ARRAY_SIZE(smd_configs); ++n) {
		idx = smd_configs[n].tty_dev_index;

		if (smd_configs[n].dev_name == NULL)
			smd_configs[n].dev_name = smd_configs[n].port_name;

		if (idx == DS_IDX) {
			/*
			 * DS port uses the kernel API starting with
			 * 8660 Fusion.  Only register the userspace
			 * platform device for older targets.
			 */
			int legacy_ds = 0;

			legacy_ds |= cpu_is_msm7x01() || cpu_is_msm7x25();
			legacy_ds |= cpu_is_msm7x27() || cpu_is_msm7x30();
			legacy_ds |= cpu_is_qsd8x50() || cpu_is_msm8x55();
			/*
			 * use legacy mode for 8660 Standalone (subtype 0)
			 */
			legacy_ds |= cpu_is_msm8x60() &&
					(socinfo_get_platform_subtype() == 0x0);
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
			legacy_ds |= cpu_is_msm8974();
#endif

			if (!legacy_ds)
				continue;
		}

		port = &smd_tty[idx].port;
		tty_port_init(port);
		port->ops = &smd_tty_port_ops;
		/* TODO: For kernel >= 3.7 use tty_port_register_device */
		smd_tty[idx].device_ptr =
			tty_register_device(smd_tty_driver, idx, 0);
		if (device_create_file(smd_tty[idx].device_ptr,
					&dev_attr_open_timeout))
			SMD_TTY_ERR(
				"%s: Unable to create device attributes for %s",
				__func__, smd_configs[n].port_name);

		init_completion(&smd_tty[idx].ch_allocated);

		/* register platform device */
		smd_tty[idx].driver.probe = smd_tty_dummy_probe;
#ifdef CONFIG_MSM_SMD_TTY_DS_LEGACY
		if (idx == DS_IDX) {
			/* register platform device for DS */
			smd_tty[idx].driver.probe = smd_tty_ds_probe;
			smd_tty[idx].is_dsmodem_ready = 0;
		}
#endif
		smd_tty[idx].driver.driver.name = smd_configs[n].dev_name;
		smd_tty[idx].driver.driver.owner = THIS_MODULE;
		spin_lock_init(&smd_tty[idx].reset_lock);
		spin_lock_init(&smd_tty[idx].ra_lock);
		smd_tty[idx].is_open = 0;
		setup_timer(&smd_tty[idx].buf_req_timer, buf_req_retry,
				(unsigned long)&smd_tty[idx]);
		init_waitqueue_head(&smd_tty[idx].ch_opened_wait_queue);
		ret = platform_driver_register(&smd_tty[idx].driver);

		if (ret) {
			SMD_TTY_ERR(
				"%s: init failed %d (%d)", __func__, idx, ret);
			smd_tty[idx].driver.probe = NULL;
			goto out;
		}
		smd_tty[idx].smd = &smd_configs[n];
	}
	INIT_DELAYED_WORK(&loopback_work, loopback_probe_worker);
	return 0;

out:
	/* unregister platform devices */
	for (n = 0; n < ARRAY_SIZE(smd_configs); ++n) {
		idx = smd_configs[n].tty_dev_index;

		if (smd_tty[idx].driver.probe) {
			platform_driver_unregister(&smd_tty[idx].driver);
			tty_unregister_device(smd_tty_driver, idx);
		}
	}

	tty_unregister_driver(smd_tty_driver);
	put_tty_driver(smd_tty_driver);
	return ret;
}

module_init(smd_tty_init);
