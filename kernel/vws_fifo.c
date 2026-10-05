/*
 * vws_fifo.c - fixed-size sample ring shared between the sampling timer
 * (producer, atomic context) and read() (consumer, process context).
 *
 * All callers must hold vws_device.fifo_lock; the lock lives in the device
 * rather than in here because read() takes it across a batch of pops.
 */
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#include "vws.h"

int vws_fifo_alloc(struct vws_fifo *f, u32 depth)
{
	depth = roundup_pow_of_two(clamp_val(depth, VWS_FIFO_MIN_DEPTH,
					     VWS_FIFO_MAX_DEPTH));

	f->buf = kcalloc(depth, sizeof(*f->buf), GFP_KERNEL);
	if (!f->buf)
		return -ENOMEM;

	f->depth = depth;
	f->head = 0;
	f->tail = 0;
	f->overflows = 0;
	f->dropped = 0;
	f->overflowing = false;
	return 0;
}

void vws_fifo_free(struct vws_fifo *f)
{
	kfree(f->buf);
	f->buf = NULL;
	f->depth = 0;
}

u32 vws_fifo_used(const struct vws_fifo *f)
{
	return (f->head - f->tail) & (f->depth - 1);
}

void vws_fifo_reset(struct vws_fifo *f)
{
	f->head = 0;
	f->tail = 0;
	f->overflowing = false;
}

/**
 * vws_fifo_push - append one sample, overwriting the oldest if the ring is full.
 *
 * A reader that cannot keep up loses the oldest unread samples, never the
 * incoming one, so the ring always holds the most recent `depth` samples and
 * the reader is back at real time on its next read(). Discarding the incoming
 * sample instead would leave it draining a stale backlog, turning a transient
 * overload into a persistent lag.
 *
 * VWS_F_RESYNC goes on the sample at the new tail, which is the first one the
 * reader will see after the gap. If a later overflow abandons that sample too
 * its flag goes with it, so whatever is eventually read carries exactly one
 * flag per gap.
 *
 * This makes the producer write f->tail, which would otherwise belong to the
 * consumer alone. Both ends are serialised by vws_device.fifo_lock, but it
 * does rule out a lockless single-producer, single-consumer ring without
 * revisiting the policy.
 */
void vws_fifo_push(struct vws_fifo *f, const struct vws_sample *s)
{
	u32 next = (f->head + 1) & (f->depth - 1);

	if (next == f->tail) {
		f->tail = (f->tail + 1) & (f->depth - 1);
		f->dropped++;

		/* Count episodes, not samples: `dropped` already has those. */
		if (!f->overflowing) {
			f->overflows++;
			f->overflowing = true;
		}

		f->buf[f->tail].flags |= VWS_F_RESYNC;
	} else {
		f->overflowing = false;
	}

	f->buf[f->head] = *s;
	f->head = next;
}

bool vws_fifo_pop(struct vws_fifo *f, struct vws_sample *s)
{
	if (f->head == f->tail)
		return false;

	*s = f->buf[f->tail];
	f->tail = (f->tail + 1) & (f->depth - 1);
	return true;
}
