/*
 * vws_ioctl.h - shared ABI between the vws kernel module and user space.
 *
 * This header is compiled by BOTH the kernel module (C) and the user-space
 * engine (C++17), so it must stay free of anything kernel- or C++-specific.
 * All sensor values crossing this boundary are fixed-point integers in
 * milli-units (see vws_unit_of()); the kernel never touches the FPU.
 */
#ifndef _VWS_IOCTL_H
#define _VWS_IOCTL_H

#ifdef __KERNEL__
#  include <linux/types.h>
#  include <linux/ioctl.h>
#else
#  include <linux/types.h>
#  include <sys/ioctl.h>
#endif

#define VWS_ABI_VERSION   1
#define VWS_DEVICE_NAME   "vws"
#define VWS_CLASS_NAME    "vws"
#define VWS_MAX_SENSORS   8
#define VWS_NAME_LEN      16

/* ---------------------------------------------------------------- sensors */

enum vws_sensor_type {
	VWS_TYPE_TEMPERATURE = 0,	/* m°C        */
	VWS_TYPE_HUMIDITY    = 1,	/* m%RH       */
	VWS_TYPE_PRESSURE    = 2,	/* m hPa      */
	VWS_TYPE_WIND        = 3,	/* mm/s       */
	VWS_TYPE_RAIN        = 4,	/* um/h       */
	VWS_TYPE_LIGHT       = 5,	/* m lux      */
	VWS_TYPE_COUNT
};

/* Fault modes that can be injected into an individual virtual sensor. */
enum vws_fault_mode {
	VWS_FAULT_NONE    = 0,
	VWS_FAULT_STUCK   = 1,	/* repeat the last value forever          */
	VWS_FAULT_DRIFT   = 2,	/* add `param` milli-units every sample   */
	VWS_FAULT_SPIKE   = 3,	/* `param`% of samples get a huge jump    */
	VWS_FAULT_DROPOUT = 4,	/* `param`% of samples are never produced */
	VWS_FAULT_NOISE   = 5,	/* override noise amplitude with `param`  */
	VWS_FAULT_COUNT
};

/* Per-sample status flags. */
#define VWS_F_OK         0x0000
#define VWS_F_FAULT      0x0001	/* a fault mode is active on this sensor */
#define VWS_F_STUCK      0x0002	/* value is a repeat of the previous one */
#define VWS_F_SATURATED  0x0004	/* value was clamped to the sensor range */
#define VWS_F_SPIKE      0x0008	/* this sample was perturbed by a spike  */
#define VWS_F_RESYNC     0x0010	/* first sample after a FIFO overflow    */

/*
 * One reading, exactly as it sits in the driver's FIFO and as read() hands it
 * to user space. Packed and explicitly sized so the layout is identical on
 * both sides of the syscall boundary; little-endian x86 ordering is assumed
 * (vws_sample_is_native_endian() in user space asserts it).
 */
struct vws_sample {
	__u8  sensor_id;
	__u8  type;		/* enum vws_sensor_type */
	__u16 flags;		/* VWS_F_*              */
	__s32 value;		/* milli-units          */
	__u64 timestamp_ns;	/* CLOCK_MONOTONIC      */
} __attribute__((packed));

#define VWS_SAMPLE_SIZE 16

/* ------------------------------------------------------------- ioctl data */

struct vws_info {
	__u32 abi_version;
	__u32 n_sensors;
	__u32 sample_rate_hz;
	__u32 fifo_depth;
	__u32 day_seconds;	/* length of one simulated day, in seconds */
	__u32 _pad;
};

struct vws_sensor_info {
	__u8  id;		/* [in]  sensor to query */
	__u8  type;		/* [out] enum vws_sensor_type */
	__u8  enabled;
	__u8  fault_mode;	/* enum vws_fault_mode */
	__s32 fault_param;
	__s32 range_min;	/* milli-units */
	__s32 range_max;
	__s32 noise_amp;	/* +/- milli-units of uniform noise */
	__u32 _pad;
	char  name[VWS_NAME_LEN];
};

struct vws_fault_req {
	__u8  sensor_id;	/* VWS_ALL_SENSORS applies to every sensor */
	__u8  mode;		/* enum vws_fault_mode */
	__u16 _pad;
	__s32 param;
};

#define VWS_ALL_SENSORS 0xff

struct vws_enable_req {
	__u8  sensor_id;
	__u8  enable;
	__u16 _pad;
};

struct vws_stats {
	__u64 timer_ticks;
	__u64 samples_generated;
	__u64 samples_dropped;	/* lost to FIFO overflow   */
	__u64 samples_read;
	__u64 fifo_overflows;
	__u64 dropouts_injected;
	__u32 fifo_used;
	__u32 readers;
};

/* ----------------------------------------------------------------- ioctls */

#define VWS_IOC_MAGIC 'W'

#define VWS_IOC_GET_INFO	_IOR(VWS_IOC_MAGIC,  1, struct vws_info)
#define VWS_IOC_GET_SENSOR	_IOWR(VWS_IOC_MAGIC, 2, struct vws_sensor_info)
#define VWS_IOC_SET_RATE	_IOW(VWS_IOC_MAGIC,  3, __u32)
#define VWS_IOC_INJECT_FAULT	_IOW(VWS_IOC_MAGIC,  4, struct vws_fault_req)
#define VWS_IOC_CLEAR_FAULT	_IOW(VWS_IOC_MAGIC,  5, __u8)
#define VWS_IOC_FLUSH		_IO(VWS_IOC_MAGIC,   6)
#define VWS_IOC_GET_STATS	_IOR(VWS_IOC_MAGIC,  7, struct vws_stats)
#define VWS_IOC_SET_ENABLE	_IOW(VWS_IOC_MAGIC,  8, struct vws_enable_req)
#define VWS_IOC_MAXNR 8

#endif /* _VWS_IOCTL_H */
