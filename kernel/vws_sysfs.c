/*
 * vws_sysfs.c - sysfs is this driver's hardware configuration interface.
 *
 * Layout:
 *   /sys/class/vws/vws0/sample_rate      sampling clock, Hz (rw)
 *   /sys/class/vws/vws0/day_seconds      length of one simulated day (rw)
 *   /sys/class/vws/vws0/fifo_depth       ring size in samples (ro)
 *   /sys/class/vws/vws0/sensorN/...      one directory per emulated chip
 *
 * The per-sensor directories are kobjects embedded in struct vws_sensor, so a
 * store callback recovers its sensor with container_of() and no lookups.
 */
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/sysfs.h>

#include "vws.h"

/* ------------------------------------------------------ device attributes */

static ssize_t sample_rate_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct vws_device *vd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", vd->sample_rate_hz);
}

static ssize_t sample_rate_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct vws_device *vd = dev_get_drvdata(dev);
	u32 hz;
	int ret;

	ret = kstrtou32(buf, 0, &hz);
	if (ret)
		return ret;
	if (hz < VWS_RATE_MIN_HZ || hz > VWS_RATE_MAX_HZ)
		return -ERANGE;

	mutex_lock(&vd->cfg_lock);
	vws_set_rate_locked(vd, hz);
	mutex_unlock(&vd->cfg_lock);
	return len;
}
static DEVICE_ATTR_RW(sample_rate);

static ssize_t day_seconds_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct vws_device *vd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", vd->day_seconds);
}

static ssize_t day_seconds_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct vws_device *vd = dev_get_drvdata(dev);
	u32 secs;
	int ret;

	ret = kstrtou32(buf, 0, &secs);
	if (ret)
		return ret;
	if (secs < 10 || secs > 86400)
		return -ERANGE;

	mutex_lock(&vd->cfg_lock);
	vd->day_seconds = secs;
	mutex_unlock(&vd->cfg_lock);
	return len;
}
static DEVICE_ATTR_RW(day_seconds);

static ssize_t fifo_depth_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct vws_device *vd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", vd->fifo.depth);
}
static DEVICE_ATTR_RO(fifo_depth);

static ssize_t n_sensors_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct vws_device *vd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", vd->n_sensors);
}
static DEVICE_ATTR_RO(n_sensors);

static struct attribute *vws_dev_attrs[] = {
	&dev_attr_sample_rate.attr,
	&dev_attr_day_seconds.attr,
	&dev_attr_fifo_depth.attr,
	&dev_attr_n_sensors.attr,
	NULL
};
ATTRIBUTE_GROUPS(vws_dev);

/* ------------------------------------------------------ sensor attributes */

#define to_vws_sensor(k) container_of(k, struct vws_sensor, kobj)

static ssize_t name_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%s\n", to_vws_sensor(kobj)->name);
}
static struct kobj_attribute vws_attr_name = __ATTR_RO(name);

static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%s\n", vws_type_name(to_vws_sensor(kobj)->type));
}
static struct kobj_attribute vws_attr_type = __ATTR_RO(type);

static ssize_t unit_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%s\n", vws_type_unit(to_vws_sensor(kobj)->type));
}
static struct kobj_attribute vws_attr_unit = __ATTR_RO(unit);

static ssize_t chip_id_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "0x%02x\n",
			  to_vws_sensor(kobj)->regs[VWS_REG_CHIP_ID]);
}
static struct kobj_attribute vws_attr_chip_id = __ATTR_RO(chip_id);

static ssize_t range_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct vws_sensor *s = to_vws_sensor(kobj);

	return sysfs_emit(buf, "%d %d\n", s->range_min, s->range_max);
}
static struct kobj_attribute vws_attr_range = __ATTR_RO(range);

static ssize_t value_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct vws_sensor *s = to_vws_sensor(kobj);
	unsigned long flags;
	bool valid;
	s32 v;

	spin_lock_irqsave(&vws_dev->fifo_lock, flags);
	valid = s->has_last;
	v = s->last_value;
	spin_unlock_irqrestore(&vws_dev->fifo_lock, flags);

	if (!valid)
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%d\n", v);
}
static struct kobj_attribute vws_attr_value = __ATTR_RO(value);

/* Raw register dump, the way you would read a real part over I2C. */
static ssize_t registers_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct vws_sensor *s = to_vws_sensor(kobj);
	u8 snap[VWS_REG_COUNT];
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&vws_dev->fifo_lock, flags);
	memcpy(snap, s->regs, sizeof(snap));
	spin_unlock_irqrestore(&vws_dev->fifo_lock, flags);

	for (i = 0; i < VWS_REG_COUNT; i++)
		n += sysfs_emit_at(buf, n, "%02x%c", snap[i],
				   i == VWS_REG_COUNT - 1 ? '\n' : ' ');
	return n;
}
static struct kobj_attribute vws_attr_registers = __ATTR_RO(registers);

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%u\n", to_vws_sensor(kobj)->enabled ? 1 : 0);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t len)
{
	struct vws_sensor *s = to_vws_sensor(kobj);
	unsigned long flags;
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	vws_cfg_begin(flags);
	s->enabled = on;
	if (on)
		s->regs[VWS_REG_CTRL] |= VWS_REG_CTRL_ENABLE;
	else
		s->regs[VWS_REG_CTRL] &= ~VWS_REG_CTRL_ENABLE;
	vws_cfg_end(flags);
	return len;
}
static struct kobj_attribute vws_attr_enable = __ATTR_RW(enable);

static ssize_t noise_amp_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%d\n", to_vws_sensor(kobj)->noise_amp);
}

static ssize_t noise_amp_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t len)
{
	struct vws_sensor *s = to_vws_sensor(kobj);
	unsigned long flags;
	s32 amp;
	int ret;

	ret = kstrtos32(buf, 0, &amp);
	if (ret)
		return ret;
	if (amp < 0 || amp > (s->range_max - s->range_min) / 4)
		return -ERANGE;

	vws_cfg_begin(flags);
	s->noise_amp = amp;
	vws_cfg_end(flags);
	return len;
}
static struct kobj_attribute vws_attr_noise_amp = __ATTR_RW(noise_amp);

static ssize_t fault_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  vws_fault_name(to_vws_sensor(kobj)->fault_mode));
}

/* Accepts either the mode name ("drift") or its numeric value. */
static ssize_t fault_mode_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	struct vws_sensor *s = to_vws_sensor(kobj);
	unsigned long flags;
	u8 mode = VWS_FAULT_COUNT;
	u32 num;
	int i;

	for (i = 0; i < VWS_FAULT_COUNT; i++)
		if (sysfs_streq(buf, vws_fault_name(i)))
			mode = i;

	if (mode == VWS_FAULT_COUNT) {
		if (kstrtou32(buf, 0, &num) || num >= VWS_FAULT_COUNT)
			return -EINVAL;
		mode = num;
	}

	vws_cfg_begin(flags);
	s->fault_mode = mode;
	s->drift_accum = 0;
	vws_cfg_end(flags);
	return len;
}
static struct kobj_attribute vws_attr_fault_mode = __ATTR_RW(fault_mode);

static ssize_t fault_param_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_vws_sensor(kobj)->fault_param);
}

static ssize_t fault_param_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t len)
{
	struct vws_sensor *s = to_vws_sensor(kobj);
	unsigned long flags;
	s32 param;
	int ret;

	ret = kstrtos32(buf, 0, &param);
	if (ret)
		return ret;

	vws_cfg_begin(flags);
	s->fault_param = param;
	vws_cfg_end(flags);
	return len;
}
static struct kobj_attribute vws_attr_fault_param = __ATTR_RW(fault_param);

static struct attribute *vws_sensor_attrs[] = {
	&vws_attr_name.attr,
	&vws_attr_type.attr,
	&vws_attr_unit.attr,
	&vws_attr_chip_id.attr,
	&vws_attr_range.attr,
	&vws_attr_value.attr,
	&vws_attr_registers.attr,
	&vws_attr_enable.attr,
	&vws_attr_noise_amp.attr,
	&vws_attr_fault_mode.attr,
	&vws_attr_fault_param.attr,
	NULL
};
ATTRIBUTE_GROUPS(vws_sensor);

/*
 * The sensors live inside the device allocation, which outlives every
 * kobject, so release() has nothing to free. It still has to exist: the
 * kobject core warns loudly about a ktype without one.
 */
static void vws_sensor_kobj_release(struct kobject *kobj)
{
}

static const struct kobj_type vws_sensor_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.release = vws_sensor_kobj_release,
	.default_groups = vws_sensor_groups,
};

/*
 * Statically allocated so that dev_groups can be wired up at compile time;
 * class_register() in vws_main.c publishes it as /sys/class/vws/.
 */
struct class vws_class = {
	.name = VWS_CLASS_NAME,
	.dev_groups = vws_dev_groups,
};

int vws_sysfs_init(struct vws_device *vd)
{
	unsigned int i;
	int ret;

	for (i = 0; i < vd->n_sensors; i++) {
		ret = kobject_init_and_add(&vd->sensors[i].kobj,
					   &vws_sensor_ktype,
					   &vd->dev->kobj, "sensor%u", i);
		if (ret) {
			kobject_put(&vd->sensors[i].kobj);
			while (i--)
				kobject_put(&vd->sensors[i].kobj);
			return ret;
		}
	}
	return 0;
}

void vws_sysfs_exit(struct vws_device *vd)
{
	unsigned int i;

	for (i = 0; i < vd->n_sensors; i++)
		kobject_put(&vd->sensors[i].kobj);
}
