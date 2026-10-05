/*
 * vws_proc.c - /proc/vws/stats and /proc/vws/sensors, the human-readable
 * counterpart to the ioctl statistics interface.
 */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "vws.h"

static struct proc_dir_entry *vws_proc_dir;

static int vws_stats_show(struct seq_file *m, void *v)
{
	struct vws_device *vd = vws_dev;
	unsigned long flags;
	u32 used;

	spin_lock_irqsave(&vd->fifo_lock, flags);
	used = vws_fifo_used(&vd->fifo);
	seq_printf(m, "abi_version:       %u\n", VWS_ABI_VERSION);
	seq_printf(m, "sample_rate_hz:    %u\n", vd->sample_rate_hz);
	seq_printf(m, "day_seconds:       %u\n", vd->day_seconds);
	seq_printf(m, "streaming:         %u\n", vd->running ? 1 : 0);
	seq_printf(m, "readers:           %d\n", atomic_read(&vd->readers));
	seq_printf(m, "timer_ticks:       %llu\n", vd->timer_ticks);
	seq_printf(m, "samples_generated: %llu\n", vd->samples_generated);
	seq_printf(m, "samples_read:      %llu\n", vd->samples_read);
	seq_printf(m, "samples_dropped:   %llu\n", vd->fifo.dropped);
	seq_printf(m, "fifo_overflows:    %llu\n", vd->fifo.overflows);
	seq_printf(m, "dropouts_injected: %llu\n", vd->dropouts_injected);
	seq_printf(m, "fifo_used:         %u/%u\n", used, vd->fifo.depth);
	spin_unlock_irqrestore(&vd->fifo_lock, flags);
	return 0;
}

static int vws_sensors_show(struct seq_file *m, void *v)
{
	struct vws_device *vd = vws_dev;
	unsigned long flags;
	unsigned int i;

	seq_printf(m, "%-3s %-10s %-12s %-8s %6s %-8s %10s %12s\n",
		   "id", "name", "type", "enabled", "noise", "fault",
		   "param", "last");

	spin_lock_irqsave(&vd->fifo_lock, flags);
	for (i = 0; i < vd->n_sensors; i++) {
		struct vws_sensor *s = &vd->sensors[i];

		seq_printf(m, "%-3u %-10s %-12s %-8u %6d %-8s %10d %12d\n",
			   s->id, s->name, vws_type_name(s->type),
			   s->enabled ? 1 : 0, s->noise_amp,
			   vws_fault_name(s->fault_mode), s->fault_param,
			   s->has_last ? s->last_value : 0);
	}
	spin_unlock_irqrestore(&vd->fifo_lock, flags);
	return 0;
}

static int vws_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, vws_stats_show, NULL);
}

static int vws_sensors_open(struct inode *inode, struct file *file)
{
	return single_open(file, vws_sensors_show, NULL);
}

static const struct proc_ops vws_stats_pops = {
	.proc_open    = vws_stats_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops vws_sensors_pops = {
	.proc_open    = vws_sensors_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

int vws_proc_init(void)
{
	vws_proc_dir = proc_mkdir("vws", NULL);
	if (!vws_proc_dir)
		return -ENOMEM;

	if (!proc_create("stats", 0444, vws_proc_dir, &vws_stats_pops) ||
	    !proc_create("sensors", 0444, vws_proc_dir, &vws_sensors_pops)) {
		remove_proc_subtree("vws", NULL);
		vws_proc_dir = NULL;
		return -ENOMEM;
	}
	return 0;
}

void vws_proc_exit(void)
{
	if (vws_proc_dir) {
		remove_proc_subtree("vws", NULL);
		vws_proc_dir = NULL;
	}
}
