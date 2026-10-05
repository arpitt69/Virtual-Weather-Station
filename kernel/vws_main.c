// SPDX-License-Identifier: GPL-2.0
/*
 * vws - Virtual Weather Station sensor emulator.
 *
 * Presents a bank of emulated weather sensor chips as a single character
 * device. An hrtimer stands in for the sampling clock / ADC end-of-conversion
 * interrupt: on every tick it runs one conversion per enabled sensor, pushes
 * the results into a per-device FIFO and wakes anyone blocked in poll() or
 * read(). Faults (stuck value, drift, spikes, dropouts, excess noise) can be
 * injected per sensor through ioctl or sysfs, giving the user-space fusion
 * engine broken hardware to cope with on a machine that has none attached.
 *
 * Locking
 *   fifo_lock (spinlock, irqsave) guards everything the sampling timer
 *     touches: the FIFO, the per-sensor runtime state, the sample period.
 *   cfg_lock (mutex) serialises process-context configuration writers so
 *     their read-modify-write sequences do not interleave, and covers the
 *     open/release transitions that start and stop the timer.
 *   A writer takes cfg_lock, then fifo_lock around the actual store.
 */
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "vws.h"

#define VWS_READ_BATCH 32

struct vws_device *vws_dev;

static dev_t vws_devt;

static unsigned int sample_rate = 20;
module_param(sample_rate, uint, 0444);
MODULE_PARM_DESC(sample_rate, "initial sampling clock in Hz (1-1000)");

static unsigned int fifo_depth = 1024;
module_param(fifo_depth, uint, 0444);
MODULE_PARM_DESC(fifo_depth, "sample FIFO depth, rounded up to a power of two");

static unsigned int day_seconds = 300;
module_param(day_seconds, uint, 0444);
MODULE_PARM_DESC(day_seconds, "wall-clock length of one simulated day");

/* --------------------------------------------------------------- sampling */

/**
 * vws_set_rate_locked - change the sampling clock.
 *
 * Caller holds cfg_lock. The timer re-arms itself from vd->period on every
 * tick, so there is no need to cancel and restart it; the new period simply
 * takes effect on the next expiry.
 */
void vws_set_rate_locked(struct vws_device *vd, u32 hz)
{
	unsigned long flags;

	spin_lock_irqsave(&vd->fifo_lock, flags);
	vd->sample_rate_hz = hz;
	vd->period = ns_to_ktime(NSEC_PER_SEC / hz);
	spin_unlock_irqrestore(&vd->fifo_lock, flags);
}

static u32 vws_used(struct vws_device *vd)
{
	unsigned long flags;
	u32 used;

	spin_lock_irqsave(&vd->fifo_lock, flags);
	used = vws_fifo_used(&vd->fifo);
	spin_unlock_irqrestore(&vd->fifo_lock, flags);
	return used;
}

/*
 * The emulated ADC interrupt. Runs in softirq context: no sleeping, no
 * floating point, no copy_to_user - integer work on preallocated memory only.
 */
static enum hrtimer_restart vws_timer_fn(struct hrtimer *t)
{
	struct vws_device *vd = container_of(t, struct vws_device, timer);
	struct vws_sample smp;
	unsigned long flags;
	unsigned int i;
	bool produced = false;
	u64 elapsed;

	spin_lock_irqsave(&vd->fifo_lock, flags);

	vd->timer_ticks++;
	elapsed = ktime_to_ns(ktime_sub(ktime_get(), vd->t0));

	for (i = 0; i < vd->n_sensors; i++) {
		if (!vws_sensor_convert(vd, &vd->sensors[i], elapsed, &smp))
			continue;	/* disabled, or an injected dropout */
		vd->samples_generated++;
		vws_fifo_push(&vd->fifo, &smp);
		produced = true;
	}

	hrtimer_forward_now(t, vd->period);
	spin_unlock_irqrestore(&vd->fifo_lock, flags);

	if (produced)
		wake_up_interruptible(&vd->readq);

	return HRTIMER_RESTART;
}

/* ------------------------------------------------------- file_operations */

static int vws_open(struct inode *inode, struct file *file)
{
	struct vws_device *vd = vws_dev;

	file->private_data = vd;

	mutex_lock(&vd->cfg_lock);
	if (atomic_inc_return(&vd->readers) == 1) {
		unsigned long flags;

		spin_lock_irqsave(&vd->fifo_lock, flags);
		vws_fifo_reset(&vd->fifo);
		spin_unlock_irqrestore(&vd->fifo_lock, flags);

		vd->t0 = ktime_get();
		vd->running = true;
		hrtimer_start(&vd->timer, vd->period, HRTIMER_MODE_REL);
		dev_dbg(vd->dev, "streaming started at %u Hz\n",
			vd->sample_rate_hz);
	}
	mutex_unlock(&vd->cfg_lock);
	return 0;
}

static int vws_release(struct inode *inode, struct file *file)
{
	struct vws_device *vd = file->private_data;

	mutex_lock(&vd->cfg_lock);
	if (atomic_dec_and_test(&vd->readers)) {
		vd->running = false;
		/*
		 * Cancel under cfg_lock, so a concurrent open() cannot start the
		 * timer between the decrement and the cancel and have its stream
		 * killed off. hrtimer_cancel() may sleep waiting for a running
		 * callback, which is safe here: that callback only ever takes
		 * fifo_lock, never this mutex.
		 */
		hrtimer_cancel(&vd->timer);
		dev_dbg(vd->dev, "streaming stopped\n");
	}
	mutex_unlock(&vd->cfg_lock);
	return 0;
}

/*
 * Whole samples only: a short count is never a partial record, so user space
 * can treat the device as a stream of fixed-size structs.
 */
static ssize_t vws_read(struct file *file, char __user *ubuf, size_t len,
			loff_t *off)
{
	struct vws_device *vd = file->private_data;
	struct vws_sample batch[VWS_READ_BATCH];
	unsigned long flags;
	size_t max, n = 0;
	int ret;

	max = len / sizeof(struct vws_sample);
	if (!max)
		return -EINVAL;
	max = min(max, (size_t)VWS_READ_BATCH);

	for (;;) {
		spin_lock_irqsave(&vd->fifo_lock, flags);
		while (n < max && vws_fifo_pop(&vd->fifo, &batch[n]))
			n++;
		if (n)
			vd->samples_read += n;
		spin_unlock_irqrestore(&vd->fifo_lock, flags);

		if (n)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(vd->readq, vws_used(vd) > 0);
		if (ret)
			return ret;	/* -ERESTARTSYS: let the signal run */
	}

	if (copy_to_user(ubuf, batch, n * sizeof(batch[0])))
		return -EFAULT;

	return n * sizeof(batch[0]);
}

static __poll_t vws_poll(struct file *file, struct poll_table_struct *wait)
{
	struct vws_device *vd = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &vd->readq, wait);
	if (vws_used(vd))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static long vws_ioctl_get_sensor(struct vws_device *vd, unsigned long arg)
{
	struct vws_sensor_info info;
	struct vws_sensor *s;
	unsigned long flags;

	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;
	if (info.id >= vd->n_sensors)
		return -EINVAL;

	s = &vd->sensors[info.id];

	memset(&info, 0, sizeof(info));
	spin_lock_irqsave(&vd->fifo_lock, flags);
	info.id          = s->id;
	info.type        = s->type;
	info.enabled     = s->enabled ? 1 : 0;
	info.fault_mode  = s->fault_mode;
	info.fault_param = s->fault_param;
	info.range_min   = s->range_min;
	info.range_max   = s->range_max;
	info.noise_amp   = s->noise_amp;
	strscpy(info.name, s->name, sizeof(info.name));
	spin_unlock_irqrestore(&vd->fifo_lock, flags);

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static long vws_ioctl_inject(struct vws_device *vd, unsigned long arg)
{
	struct vws_fault_req req;
	unsigned long flags;
	unsigned int i;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.mode >= VWS_FAULT_COUNT)
		return -EINVAL;
	if (req.sensor_id != VWS_ALL_SENSORS && req.sensor_id >= vd->n_sensors)
		return -EINVAL;

	mutex_lock(&vd->cfg_lock);
	spin_lock_irqsave(&vd->fifo_lock, flags);
	for (i = 0; i < vd->n_sensors; i++) {
		if (req.sensor_id != VWS_ALL_SENSORS && req.sensor_id != i)
			continue;
		vd->sensors[i].fault_mode  = req.mode;
		vd->sensors[i].fault_param = req.param;
		vd->sensors[i].drift_accum = 0;
	}
	spin_unlock_irqrestore(&vd->fifo_lock, flags);
	mutex_unlock(&vd->cfg_lock);

	dev_info(vd->dev, "fault '%s' param %d injected into sensor %u\n",
		 vws_fault_name(req.mode), req.param, req.sensor_id);
	return 0;
}

static long vws_ioctl_set_enable(struct vws_device *vd, unsigned long arg)
{
	struct vws_enable_req req;
	unsigned long flags;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.sensor_id >= vd->n_sensors)
		return -EINVAL;

	mutex_lock(&vd->cfg_lock);
	spin_lock_irqsave(&vd->fifo_lock, flags);
	vd->sensors[req.sensor_id].enabled = !!req.enable;
	spin_unlock_irqrestore(&vd->fifo_lock, flags);
	mutex_unlock(&vd->cfg_lock);
	return 0;
}

static long vws_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vws_device *vd = file->private_data;
	unsigned long flags;
	unsigned int i;

	if (_IOC_TYPE(cmd) != VWS_IOC_MAGIC || _IOC_NR(cmd) > VWS_IOC_MAXNR)
		return -ENOTTY;

	/* Mutating commands need the fd opened for writing. */
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && !(file->f_mode & FMODE_WRITE))
		return -EACCES;

	switch (cmd) {
	case VWS_IOC_GET_INFO: {
		struct vws_info info = {
			.abi_version = VWS_ABI_VERSION,
			.n_sensors   = vd->n_sensors,
			.fifo_depth  = vd->fifo.depth,
		};

		/* sample_rate_hz and day_seconds are written under fifo_lock. */
		spin_lock_irqsave(&vd->fifo_lock, flags);
		info.sample_rate_hz = vd->sample_rate_hz;
		info.day_seconds = vd->day_seconds;
		spin_unlock_irqrestore(&vd->fifo_lock, flags);

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case VWS_IOC_GET_SENSOR:
		return vws_ioctl_get_sensor(vd, arg);

	case VWS_IOC_SET_RATE: {
		u32 hz;

		if (copy_from_user(&hz, (void __user *)arg, sizeof(hz)))
			return -EFAULT;
		if (hz < VWS_RATE_MIN_HZ || hz > VWS_RATE_MAX_HZ)
			return -ERANGE;

		mutex_lock(&vd->cfg_lock);
		vws_set_rate_locked(vd, hz);
		mutex_unlock(&vd->cfg_lock);
		return 0;
	}
	case VWS_IOC_INJECT_FAULT:
		return vws_ioctl_inject(vd, arg);

	case VWS_IOC_CLEAR_FAULT: {
		u8 id;

		if (copy_from_user(&id, (void __user *)arg, sizeof(id)))
			return -EFAULT;
		if (id != VWS_ALL_SENSORS && id >= vd->n_sensors)
			return -EINVAL;

		mutex_lock(&vd->cfg_lock);
		spin_lock_irqsave(&vd->fifo_lock, flags);
		for (i = 0; i < vd->n_sensors; i++) {
			if (id != VWS_ALL_SENSORS && id != i)
				continue;
			vd->sensors[i].fault_mode  = VWS_FAULT_NONE;
			vd->sensors[i].fault_param = 0;
			vd->sensors[i].drift_accum = 0;
		}
		spin_unlock_irqrestore(&vd->fifo_lock, flags);
		mutex_unlock(&vd->cfg_lock);
		return 0;
	}
	case VWS_IOC_SET_ENABLE:
		return vws_ioctl_set_enable(vd, arg);

	case VWS_IOC_FLUSH:
		spin_lock_irqsave(&vd->fifo_lock, flags);
		vws_fifo_reset(&vd->fifo);
		spin_unlock_irqrestore(&vd->fifo_lock, flags);
		return 0;

	case VWS_IOC_GET_STATS: {
		struct vws_stats st;

		memset(&st, 0, sizeof(st));
		spin_lock_irqsave(&vd->fifo_lock, flags);
		st.timer_ticks       = vd->timer_ticks;
		st.samples_generated = vd->samples_generated;
		st.samples_read      = vd->samples_read;
		st.samples_dropped   = vd->fifo.dropped;
		st.fifo_overflows    = vd->fifo.overflows;
		st.dropouts_injected = vd->dropouts_injected;
		st.fifo_used         = vws_fifo_used(&vd->fifo);
		spin_unlock_irqrestore(&vd->fifo_lock, flags);
		st.readers = atomic_read(&vd->readers);

		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vws_fops = {
	.owner          = THIS_MODULE,
	.open           = vws_open,
	.release        = vws_release,
	.read           = vws_read,
	.poll           = vws_poll,
	.unlocked_ioctl = vws_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ----------------------------------------------------- module life cycle */

static int __init vws_init(void)
{
	struct vws_device *vd;
	int ret;

	BUILD_BUG_ON(sizeof(struct vws_sample) != VWS_SAMPLE_SIZE);

	if (sample_rate < VWS_RATE_MIN_HZ || sample_rate > VWS_RATE_MAX_HZ)
		return -EINVAL;
	if (day_seconds < 10 || day_seconds > 86400)
		return -EINVAL;

	vd = kzalloc(sizeof(*vd), GFP_KERNEL);
	if (!vd)
		return -ENOMEM;

	spin_lock_init(&vd->fifo_lock);
	mutex_init(&vd->cfg_lock);
	init_waitqueue_head(&vd->readq);
	atomic_set(&vd->readers, 0);
	vd->sample_rate_hz = sample_rate;
	vd->day_seconds = day_seconds;
	vd->period = ns_to_ktime(NSEC_PER_SEC / sample_rate);
	vd->t0 = ktime_get();

	vws_sensor_bank_init(vd);

	ret = vws_fifo_alloc(&vd->fifo, fifo_depth);
	if (ret)
		goto err_free;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	hrtimer_setup(&vd->timer, vws_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
#else
	hrtimer_init(&vd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vd->timer.function = vws_timer_fn;
#endif

	ret = alloc_chrdev_region(&vws_devt, 0, 1, VWS_DEVICE_NAME);
	if (ret)
		goto err_fifo;
	vd->devt = vws_devt;

	cdev_init(&vd->cdev, &vws_fops);
	vd->cdev.owner = THIS_MODULE;
	ret = cdev_add(&vd->cdev, vws_devt, 1);
	if (ret)
		goto err_region;

	ret = class_register(&vws_class);
	if (ret)
		goto err_cdev;

	/* vws_dev must be live before device_create: the sysfs attributes
	 * published by that call may be read the instant they appear. */
	vws_dev = vd;

	vd->dev = device_create(&vws_class, NULL, vws_devt, vd,
				VWS_DEVICE_NAME);
	if (IS_ERR(vd->dev)) {
		ret = PTR_ERR(vd->dev);
		goto err_class;
	}

	ret = vws_sysfs_init(vd);
	if (ret)
		goto err_device;

	ret = vws_proc_init();
	if (ret)
		goto err_sysfs;

	dev_info(vd->dev,
		 "loaded: %u sensors, %u Hz, FIFO %u samples, %u s/day, /dev/%s (%d:%d)\n",
		 vd->n_sensors, vd->sample_rate_hz, vd->fifo.depth,
		 vd->day_seconds, VWS_DEVICE_NAME, MAJOR(vws_devt),
		 MINOR(vws_devt));
	return 0;

err_sysfs:
	vws_sysfs_exit(vd);
err_device:
	device_destroy(&vws_class, vws_devt);
err_class:
	vws_dev = NULL;
	class_unregister(&vws_class);
err_cdev:
	cdev_del(&vd->cdev);
err_region:
	unregister_chrdev_region(vws_devt, 1);
err_fifo:
	vws_fifo_free(&vd->fifo);
err_free:
	mutex_destroy(&vd->cfg_lock);
	kfree(vd);
	return ret;
}

static void __exit vws_exit(void)
{
	struct vws_device *vd = vws_dev;

	vws_proc_exit();
	hrtimer_cancel(&vd->timer);
	vws_sysfs_exit(vd);
	device_destroy(&vws_class, vd->devt);
	class_unregister(&vws_class);
	cdev_del(&vd->cdev);
	unregister_chrdev_region(vd->devt, 1);
	vws_fifo_free(&vd->fifo);
	mutex_destroy(&vd->cfg_lock);
	vws_dev = NULL;
	kfree(vd);

	pr_info("vws: unloaded\n");
}

module_init(vws_init);
module_exit(vws_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arpit Abhigyan Parida");
MODULE_DESCRIPTION("Virtual Weather Station: emulated multi-sensor bank with fault injection");
MODULE_VERSION("1.0");
