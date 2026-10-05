/*
 * vws.h - internal definitions for the Virtual Weather Station driver.
 */
#ifndef _VWS_INTERNAL_H
#define _VWS_INTERNAL_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "../include/vws_ioctl.h"

#define VWS_FIFO_MIN_DEPTH	16
#define VWS_FIFO_MAX_DEPTH	65536
#define VWS_RATE_MIN_HZ		1
#define VWS_RATE_MAX_HZ		1000

/*
 * Emulated register map. A real BME280-class part exposes its configuration
 * and its last conversion over I2C as a handful of 8-bit registers; the
 * sampling path below writes the same layout so that user space (and anyone
 * reading the code) sees a device, not a random number generator.
 */
enum vws_reg {
	VWS_REG_CHIP_ID   = 0x00,
	VWS_REG_STATUS    = 0x01,
	VWS_REG_CTRL      = 0x02,
	VWS_REG_DATA_MSB  = 0x03,	/* big-endian, as most sensor parts are */
	VWS_REG_DATA_LSB2 = 0x04,
	VWS_REG_DATA_LSB1 = 0x05,
	VWS_REG_DATA_LSB  = 0x06,
	VWS_REG_COUNT     = 0x08
};

#define VWS_REG_STATUS_MEASURING	BIT(0)
#define VWS_REG_STATUS_NEW_DATA		BIT(1)
#define VWS_REG_CTRL_ENABLE		BIT(0)

/* One emulated sensor chip. */
struct vws_sensor {
	u8	id;
	u8	type;
	char	name[VWS_NAME_LEN];

	/* base model */
	s32	base;		/* mid-point, milli-units                    */
	s32	amplitude;	/* peak deviation of the diurnal term        */
	u16	phase_shift;	/* 0..255, offset into the simulated day     */
	u8	harmonic;	/* cycles per simulated day                  */
	s32	bias;		/* fixed per-unit calibration error          */
	s32	noise_amp;	/* +/- uniform noise, milli-units            */
	s32	range_min;
	s32	range_max;

	/* fault injection */
	u8	fault_mode;
	s32	fault_param;
	s32	drift_accum;

	/* model state that has to persist between conversions */
	s32	model_state;

	/* last conversion */
	s32	last_value;
	bool	has_last;
	bool	enabled;

	u8	regs[VWS_REG_COUNT];

	struct kobject kobj;		/* /sys/class/vws/vws0/sensorN/ */
};

/* Fixed-size sample ring. Written from hrtimer (atomic) context. */
struct vws_fifo {
	struct vws_sample *buf;
	u32	depth;		/* power of two */
	u32	head;		/* producer index */
	u32	tail;		/* consumer index */
	u64	overflows;	/* distinct overflow episodes */
	u64	dropped;	/* individual samples lost     */
	bool	overflowing;	/* mid-episode, for the above  */
};

struct vws_device {
	struct cdev		cdev;
	struct device		*dev;
	dev_t			devt;

	struct vws_sensor	sensors[VWS_MAX_SENSORS];
	u32			n_sensors;

	struct vws_fifo		fifo;
	spinlock_t		fifo_lock;	/* guards fifo, taken in timer */
	struct mutex		cfg_lock;	/* guards sensor config, process ctx */
	wait_queue_head_t	readq;

	struct hrtimer		timer;
	ktime_t			period;
	u32			sample_rate_hz;
	u32			day_seconds;
	bool			running;

	atomic_t		readers;
	u64			timer_ticks;
	u64			samples_generated;
	u64			samples_read;
	u64			dropouts_injected;
	ktime_t			t0;
};

extern struct vws_device *vws_dev;

/*
 * Configuration-write helpers: take cfg_lock to serialise process-context
 * writers, then fifo_lock to exclude the sampling timer while the fields it
 * reads are updated. Only valid in process context.
 */
#define vws_cfg_begin(flags)					\
	do {							\
		mutex_lock(&vws_dev->cfg_lock);			\
		spin_lock_irqsave(&vws_dev->fifo_lock, flags);	\
	} while (0)

#define vws_cfg_end(flags)						\
	do {								\
		spin_unlock_irqrestore(&vws_dev->fifo_lock, flags);	\
		mutex_unlock(&vws_dev->cfg_lock);			\
	} while (0)

/* vws_model.c */
s32 vws_sin_q(u16 phase);
void vws_sensor_bank_init(struct vws_device *vd);
bool vws_sensor_convert(struct vws_device *vd, struct vws_sensor *s,
			u64 elapsed_ns, struct vws_sample *out);
const char *vws_type_name(u8 type);
const char *vws_type_unit(u8 type);
const char *vws_fault_name(u8 mode);

/* vws_fifo.c */
int  vws_fifo_alloc(struct vws_fifo *f, u32 depth);
void vws_fifo_free(struct vws_fifo *f);
void vws_fifo_push(struct vws_fifo *f, const struct vws_sample *s);
bool vws_fifo_pop(struct vws_fifo *f, struct vws_sample *s);
u32  vws_fifo_used(const struct vws_fifo *f);
void vws_fifo_reset(struct vws_fifo *f);

/* vws_sysfs.c */
extern struct class vws_class;
int  vws_sysfs_init(struct vws_device *vd);
void vws_sysfs_exit(struct vws_device *vd);

/* vws_proc.c */
int  vws_proc_init(void);
void vws_proc_exit(void);

/* vws_main.c */
void vws_set_rate_locked(struct vws_device *vd, u32 hz);

#endif /* _VWS_INTERNAL_H */
