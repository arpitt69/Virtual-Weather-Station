/*
 * vws_model.c - integer-only sensor models and fault injection.
 *
 * Nothing here may touch the FPU: kernel code runs with the user's FPU state
 * live, so floating point is off limits without kernel_fpu_begin(). Every
 * quantity is therefore a fixed-point integer in milli-units, and the diurnal
 * curve comes from a Q4.14-ish sine table (+/-10000) rather than sin().
 */
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/random.h>

#include "vws.h"

/* sin(2*pi*i/256) * 10000, generated once and frozen into the driver. */
static const s16 vws_sin_tbl[256] = {
	     0,    245,    491,    736,    980,   1224,   1467,   1710,
	  1951,   2191,   2430,   2667,   2903,   3137,   3369,   3599,
	  3827,   4052,   4276,   4496,   4714,   4929,   5141,   5350,
	  5556,   5758,   5957,   6152,   6344,   6532,   6716,   6895,
	  7071,   7242,   7410,   7572,   7730,   7883,   8032,   8176,
	  8315,   8449,   8577,   8701,   8819,   8932,   9040,   9142,
	  9239,   9330,   9415,   9495,   9569,   9638,   9700,   9757,
	  9808,   9853,   9892,   9925,   9952,   9973,   9988,   9997,
	 10000,   9997,   9988,   9973,   9952,   9925,   9892,   9853,
	  9808,   9757,   9700,   9638,   9569,   9495,   9415,   9330,
	  9239,   9142,   9040,   8932,   8819,   8701,   8577,   8449,
	  8315,   8176,   8032,   7883,   7730,   7572,   7410,   7242,
	  7071,   6895,   6716,   6532,   6344,   6152,   5957,   5758,
	  5556,   5350,   5141,   4929,   4714,   4496,   4276,   4052,
	  3827,   3599,   3369,   3137,   2903,   2667,   2430,   2191,
	  1951,   1710,   1467,   1224,    980,    736,    491,    245,
	     0,   -245,   -491,   -736,   -980,  -1224,  -1467,  -1710,
	 -1951,  -2191,  -2430,  -2667,  -2903,  -3137,  -3369,  -3599,
	 -3827,  -4052,  -4276,  -4496,  -4714,  -4929,  -5141,  -5350,
	 -5556,  -5758,  -5957,  -6152,  -6344,  -6532,  -6716,  -6895,
	 -7071,  -7242,  -7410,  -7572,  -7730,  -7883,  -8032,  -8176,
	 -8315,  -8449,  -8577,  -8701,  -8819,  -8932,  -9040,  -9142,
	 -9239,  -9330,  -9415,  -9495,  -9569,  -9638,  -9700,  -9757,
	 -9808,  -9853,  -9892,  -9925,  -9952,  -9973,  -9988,  -9997,
	-10000,  -9997,  -9988,  -9973,  -9952,  -9925,  -9892,  -9853,
	 -9808,  -9757,  -9700,  -9638,  -9569,  -9495,  -9415,  -9330,
	 -9239,  -9142,  -9040,  -8932,  -8819,  -8701,  -8577,  -8449,
	 -8315,  -8176,  -8032,  -7883,  -7730,  -7572,  -7410,  -7242,
	 -7071,  -6895,  -6716,  -6532,  -6344,  -6152,  -5957,  -5758,
	 -5556,  -5350,  -5141,  -4929,  -4714,  -4496,  -4276,  -4052,
	 -3827,  -3599,  -3369,  -3137,  -2903,  -2667,  -2430,  -2191,
	 -1951,  -1710,  -1467,  -1224,   -980,   -736,   -491,   -245,
};

/**
 * vws_sin_q - interpolated sine, scaled to +/-10000.
 * @phase: 16-bit position in the cycle - high byte indexes the table, low
 *         byte is the fraction between that entry and the next. Wraps
 *         naturally at 65536.
 *
 * The table alone resolves a cycle into 256 steps, and adjacent entries differ
 * by up to 245 - which scales to jumps larger than the sensors' own noise, so
 * the model would emit a staircase and user space would rightly read the
 * risers as discontinuities. Interpolating gives 65536 steps per cycle and a
 * signal that is smooth at sample resolution.
 */
s32 vws_sin_q(u16 phase)
{
	u8 idx = phase >> 8;
	s32 frac = phase & 0xff;
	s32 a = vws_sin_tbl[idx];
	s32 b = vws_sin_tbl[(idx + 1) & 0xff];

	return a + ((b - a) * frac) / 256;
}

/* Uniform noise in [-amp, +amp]. get_random_u32() is safe in atomic context. */
static s32 vws_noise(s32 amp)
{
	if (amp <= 0)
		return 0;
	return (s32)(get_random_u32() % (u32)(2 * amp + 1)) - amp;
}

static s32 vws_clamp_s32(s32 v, s32 lo, s32 hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

const char *vws_type_name(u8 type)
{
	static const char * const names[VWS_TYPE_COUNT] = {
		"temperature", "humidity", "pressure", "wind", "rain", "light"
	};
	return type < VWS_TYPE_COUNT ? names[type] : "unknown";
}

const char *vws_type_unit(u8 type)
{
	static const char * const units[VWS_TYPE_COUNT] = {
		"m degC", "m%RH", "mhPa", "mm/s", "um/h", "mlux"
	};
	return type < VWS_TYPE_COUNT ? units[type] : "?";
}

const char *vws_fault_name(u8 mode)
{
	static const char * const names[VWS_FAULT_COUNT] = {
		"none", "stuck", "drift", "spike", "dropout", "noise"
	};
	return mode < VWS_FAULT_COUNT ? names[mode] : "unknown";
}

/*
 * The sensor bank. Two temperature units measure the same thing with
 * different noise and a different calibration bias, which is what gives the
 * user-space fusion engine something real to do.
 */
void vws_sensor_bank_init(struct vws_device *vd)
{
	static const struct vws_sensor tmpl[] = {
	{ .name = "temp_a",   .type = VWS_TYPE_TEMPERATURE,
	  .base =   18000, .amplitude = 9000,  .harmonic = 1, .phase_shift = 192,
	  .bias =       0, .noise_amp =  150,
	  .range_min = -40000, .range_max =  85000 },
	{ .name = "temp_b",   .type = VWS_TYPE_TEMPERATURE,
	  .base =   18000, .amplitude = 9000,  .harmonic = 1, .phase_shift = 192,
	  .bias =     450, .noise_amp =  600,
	  .range_min = -40000, .range_max =  85000 },
	{ .name = "humidity", .type = VWS_TYPE_HUMIDITY,
	  .base =   58000, .amplitude = 22000, .harmonic = 1, .phase_shift = 64,
	  .bias =       0, .noise_amp =  800,
	  .range_min =      0, .range_max = 100000 },
	{ .name = "pressure", .type = VWS_TYPE_PRESSURE,
	  .base = 1013250, .amplitude = 6000,  .harmonic = 1, .phase_shift = 30,
	  .bias =       0, .noise_amp =  120,
	  .range_min = 870000, .range_max = 1085000 },
	{ .name = "wind",     .type = VWS_TYPE_WIND,
	  .base =    4200, .amplitude = 3200,  .harmonic = 3, .phase_shift = 0,
	  .bias =       0, .noise_amp = 1100,
	  .range_min =      0, .range_max =  75000 },
	{ .name = "rain",     .type = VWS_TYPE_RAIN,
	  .base =       0, .amplitude = 0,     .harmonic = 1, .phase_shift = 0,
	  .bias =       0, .noise_amp =    0,
	  .range_min =      0, .range_max = 200000 },
	{ .name = "light",    .type = VWS_TYPE_LIGHT,
	  .base =       0, .amplitude = 0,     .harmonic = 1, .phase_shift = 192,
	  .bias =       0, .noise_amp = 60000,
	  .range_min =      0, .range_max = 120000000 },
	};
	unsigned int i;

	vd->n_sensors = ARRAY_SIZE(tmpl);
	BUILD_BUG_ON(ARRAY_SIZE(tmpl) > VWS_MAX_SENSORS);

	for (i = 0; i < vd->n_sensors; i++) {
		struct vws_sensor *s = &vd->sensors[i];

		*s = tmpl[i];
		s->id = i;
		s->enabled = true;
		s->fault_mode = VWS_FAULT_NONE;
		s->fault_param = 0;
		s->drift_accum = 0;
		s->model_state = 0;
		s->has_last = false;
		s->last_value = 0;

		memset(s->regs, 0, sizeof(s->regs));
		s->regs[VWS_REG_CHIP_ID] = 0x60 + i;	/* BME280 reports 0x60 */
		s->regs[VWS_REG_CTRL] = VWS_REG_CTRL_ENABLE;
	}
}

/*
 * Phase of the simulated day as a 16-bit fraction, ready for vws_sin_q().
 * One simulated day lasts vd->day_seconds of wall time so that a diurnal
 * curve is visible on a dashboard in a couple of minutes. @shift is in table
 * entries (0..255) for legibility at the call sites.
 */
static u16 vws_day_phase(const struct vws_device *vd, u64 elapsed_ns,
			 u8 harmonic, u16 shift)
{
	u64 day_ns = (u64)vd->day_seconds * NSEC_PER_SEC;
	u64 rem, step;

	if (!day_ns)
		return (u16)(shift << 8);

	/*
	 * Reduce modulo one day before scaling up: elapsed_ns * 65536 would
	 * overflow a u64 after about a day of uptime, while the remainder is
	 * bounded by day_ns and cannot.
	 */
	div64_u64_rem(elapsed_ns, day_ns, &rem);
	step = div64_u64(rem * 65536ULL * max_t(u8, harmonic, 1), day_ns);

	return (u16)((step + ((u32)shift << 8)) & 0xffff);
}

/*
 * The model's own output, before noise and faults. Not const: the rain
 * channel carries an intensity that persists between conversions.
 */
static s32 vws_base_model(const struct vws_device *vd, struct vws_sensor *s,
			  u64 elapsed_ns)
{
	u16 phase = vws_day_phase(vd, elapsed_ns, s->harmonic, s->phase_shift);
	s32 v;

	switch (s->type) {
	case VWS_TYPE_RAIN: {
		/* Showers while the (slow) synoptic term is in its low half. */
		u16 synoptic = vws_day_phase(vd, elapsed_ns, 1, 100);

		/*
		 * Rainfall intensity is strongly autocorrelated - a shower
		 * builds and eases over minutes - so a bounded random walk
		 * topping out at 18 mm/h, not a fresh uniform draw per sample.
		 */
		if (vws_sin_q(synoptic) > -3000) {
			s->model_state -= s->model_state / 8;	/* easing off */
			if (s->model_state < 100)
				s->model_state = 0;
		} else {
			/*
			 * Steps small relative to the sampling rate, so the
			 * intensity is smooth sample to sample, and mildly
			 * mean-reverting so the walk neither parks on the
			 * clamp nor dies out.
			 */
			s32 step = (s32)(get_random_u32() % 81) - 40;

			step += (6000 - s->model_state) / 400;
			s->model_state = clamp_val(s->model_state + step,
						   0, 18000);
		}
		v = s->model_state;
		break;
	}
	case VWS_TYPE_LIGHT:
		/* Daylight is a clipped sine: zero all night, ~100 klux at noon. */
		v = vws_sin_q(phase);
		if (v <= 0)
			return 0;
		v = v * 10000;		/* 10000 -> 100 000 000 mlux */
		break;
	case VWS_TYPE_WIND:
		v = s->base + (s->amplitude * vws_sin_q(phase)) / 10000;
		/* Gusts: short multiplicative excursions. */
		if ((get_random_u32() & 0x1f) == 0)
			v += v / 2;
		break;
	default:
		v = s->base + (s->amplitude * vws_sin_q(phase)) / 10000;
		break;
	}
	return v;
}

/* Record the conversion result in the emulated register map, big-endian. */
static void vws_regs_store(struct vws_sensor *s, s32 value)
{
	u32 raw = (u32)value;

	s->regs[VWS_REG_DATA_MSB]  = (raw >> 24) & 0xff;
	s->regs[VWS_REG_DATA_LSB2] = (raw >> 16) & 0xff;
	s->regs[VWS_REG_DATA_LSB1] = (raw >>  8) & 0xff;
	s->regs[VWS_REG_DATA_LSB]  =  raw        & 0xff;
	s->regs[VWS_REG_STATUS] = VWS_REG_STATUS_NEW_DATA;
}

/**
 * vws_sensor_convert - run one conversion for a sensor.
 * @vd:         device
 * @s:          sensor to sample
 * @elapsed_ns: nanoseconds since the module started streaming
 * @out:        filled in on success
 *
 * Returns false when no sample was produced at all, which is how an injected
 * dropout differs from a bad reading: user space sees a gap, not a value.
 * Called from hrtimer (atomic) context with fifo_lock held.
 */
bool vws_sensor_convert(struct vws_device *vd, struct vws_sensor *s,
			u64 elapsed_ns, struct vws_sample *out)
{
	s32 value, noise_amp;
	u16 flags = VWS_F_OK;

	if (!s->enabled)
		return false;

	if (s->fault_mode != VWS_FAULT_NONE)
		flags |= VWS_F_FAULT;

	switch (s->fault_mode) {
	case VWS_FAULT_DROPOUT:
		if ((get_random_u32() % 100) < (u32)clamp_val(s->fault_param, 0, 100)) {
			vd->dropouts_injected++;
			return false;
		}
		break;
	case VWS_FAULT_STUCK:
		if (s->has_last) {
			/* A wedged ADC: the same code, sample after sample. */
			*out = (struct vws_sample){
				.sensor_id = s->id, .type = s->type,
				.flags = flags | VWS_F_STUCK,
				.value = s->last_value,
				.timestamp_ns = ktime_get_ns(),
			};
			vws_regs_store(s, s->last_value);
			return true;
		}
		break;
	default:
		break;
	}

	value = vws_base_model(vd, s, elapsed_ns) + s->bias;

	noise_amp = s->noise_amp;
	if (s->fault_mode == VWS_FAULT_NOISE) {
		noise_amp = clamp_val(s->fault_param, 0, s->range_max / 4);
	} else if (s->type == VWS_TYPE_LIGHT && s->range_max > 0) {
		/*
		 * A photodiode's dominant error is photon shot noise, which
		 * grows as the square root of the illuminance, so the part is
		 * near-silent in the dark and noisiest at noon. int_sqrt()
		 * keeps this in integers; the FPU is off limits here.
		 */
		unsigned long lo = int_sqrt((unsigned long)max(value, 0));
		unsigned long hi = int_sqrt((unsigned long)s->range_max);

		noise_amp = hi ? (s32)div64_u64((u64)noise_amp * lo, hi) : 0;
	}
	value += vws_noise(noise_amp);

	if (s->fault_mode == VWS_FAULT_DRIFT) {
		s->drift_accum += s->fault_param;
		value += s->drift_accum;
	}

	if (s->fault_mode == VWS_FAULT_SPIKE &&
	    (get_random_u32() % 100) < (u32)clamp_val(s->fault_param, 0, 100)) {
		s32 span = (s->range_max - s->range_min) / 6;

		value += (get_random_u32() & 1) ? span : -span;
		flags |= VWS_F_SPIKE;
	}

	/*
	 * ADC quantisation: a 12-bit converter over the sensor's full range
	 * cannot resolve finer than one LSB, so round the model to that grid.
	 */
	{
		s32 lsb = (s->range_max - s->range_min) / 4096;

		if (lsb > 1)
			value -= value % lsb;
	}

	{
		s32 clamped = vws_clamp_s32(value, s->range_min, s->range_max);

		if (clamped != value)
			flags |= VWS_F_SATURATED;
		value = clamped;
	}

	s->last_value = value;
	s->has_last = true;
	vws_regs_store(s, value);

	*out = (struct vws_sample){
		.sensor_id = s->id,
		.type = s->type,
		.flags = flags,
		.value = value,
		.timestamp_ns = ktime_get_ns(),
	};
	return true;
}
