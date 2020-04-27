/* PTP 1588 clock using the Renesas Ethernet AVB
 *
 * Copyright (C) 2013-2017 Renesas Electronics Corporation
 * Copyright (C) 2015 Renesas Solutions Corp.
 * Copyright (C) 2015-2016 Cogent Embedded, Inc. <source@cogentembedded.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <linux/bitops.h>
#include <linux/kthread.h>
#include <linux/mediats_ops.h>

#include "ravb.h"

static void ravb_avtp_capture_int(struct ravb_private *priv, u32 gis);

static int ravb_ptp_tcr_request(struct ravb_private *priv, u32 request)
{
	struct net_device *ndev = priv->ndev;
	int error;

	error = ravb_wait(ndev, GCCR, GCCR_TCR, GCCR_TCR_NOREQ);
	if (error)
		return error;

	ravb_modify(ndev, GCCR, request, request);
	return ravb_wait(ndev, GCCR, GCCR_TCR, GCCR_TCR_NOREQ);
}

/* Caller must hold the lock */
static int ravb_ptp_time_read(struct ravb_private *priv, struct timespec64 *ts)
{
	struct net_device *ndev = priv->ndev;
	int error;

	error = ravb_ptp_tcr_request(priv, GCCR_TCR_CAPTURE);
	if (error)
		return error;

	ts->tv_nsec = ravb_read(ndev, GCT0);
	ts->tv_sec  = ravb_read(ndev, GCT1) |
		((s64)ravb_read(ndev, GCT2) << 32);

	return 0;
}

#define CTS_MAX_DELAY 1000 // 1 µs
#define CTS_MAX_ITER 100

int ravb_ptp_time_read_xts(struct ravb_private *priv, struct timespec64 *ts,
						   ktime_t *time)
{
	struct net_device *ndev = priv->ndev;
	int error;
	ktime_t kt_before, kt_after;
	int i = CTS_MAX_ITER;

	do {
		error = ravb_wait(ndev, GCCR, GCCR_TCR, GCCR_TCR_NOREQ);
		if (error)
			return error;

		kt_before = ktime_get();
		ravb_modify(ndev, GCCR, GCCR_TCR_CAPTURE, GCCR_TCR_CAPTURE);
		error = ravb_wait(ndev, GCCR, GCCR_TCR, GCCR_TCR_NOREQ);
		kt_after = ktime_get();
		if (error)
			return error;
	} while (ktime_to_ns(ktime_sub(kt_after, kt_before)) > CTS_MAX_DELAY &&
			 i-- > 0);

	if (i <= 0)
		return -ETIMEDOUT;

	*time = ktime_add_ns(kt_before, ktime_divns(ktime_sub(kt_after, kt_before), 2));
	ts->tv_nsec = ravb_read(ndev, GCT0);
	ts->tv_sec  = ravb_read(ndev, GCT1) |
		((s64)ravb_read(ndev, GCT2) << 32);

	return 0;
}

/* Caller must hold the lock */
static int ravb_ptp_time_write(struct ravb_private *priv,
				const struct timespec64 *ts)
{
	struct net_device *ndev = priv->ndev;
	int error;
	u32 gccr;

	error = ravb_ptp_tcr_request(priv, GCCR_TCR_RESET);
	if (error)
		return error;

	gccr = ravb_read(ndev, GCCR);
	if (gccr & GCCR_LTO)
		return -EBUSY;
	ravb_write(ndev, ts->tv_nsec, GTO0);
	ravb_write(ndev, ts->tv_sec,  GTO1);
	ravb_write(ndev, (ts->tv_sec >> 32) & 0xffff, GTO2);
	ravb_write(ndev, gccr | GCCR_LTO, GCCR);

	return 0;
}

/* Caller must hold the lock */
static int ravb_ptp_update_compare(struct ravb_private *priv, u32 ns)
{
	struct net_device *ndev = priv->ndev;
	/* When the comparison value (GPTC.PTCV) is in range of
	 * [x-1 to x+1] (x is the configured increment value in
	 * GTI.TIV), it may happen that a comparison match is
	 * not detected when the timer wraps around.
	 */
	u32 gti_ns_plus_1 = (priv->ptp.current_addend >> 20) + 1;
	u32 gccr;

	if (ns < gti_ns_plus_1)
		ns = gti_ns_plus_1;
	else if (ns > 0 - gti_ns_plus_1)
		ns = 0 - gti_ns_plus_1;

	gccr = ravb_read(ndev, GCCR);
	if (gccr & GCCR_LPTC)
		return -EBUSY;
	ravb_write(ndev, ns, GPTC);
	ravb_write(ndev, gccr | GCCR_LPTC, GCCR);

	return 0;
}

/* PTP clock operations */
static int ravb_ptp_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	struct ravb_private *priv = container_of(ptp, struct ravb_private,
						 ptp.info);
	struct net_device *ndev = priv->ndev;
	unsigned long flags;
	u32 diff, addend;
	bool neg_adj = false;
	u32 gccr;

	if (ppb < 0) {
		neg_adj = true;
		ppb = -ppb;
	}
	addend = priv->ptp.default_addend;
	diff = div_u64((u64)addend * ppb, NSEC_PER_SEC);

	addend = neg_adj ? addend - diff : addend + diff;

	spin_lock_irqsave(&priv->lock, flags);

	priv->ptp.current_addend = addend;

	gccr = ravb_read(ndev, GCCR);
	if (gccr & GCCR_LTI) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return -EBUSY;
	}
	ravb_write(ndev, addend & GTI_TIV, GTI);
	ravb_write(ndev, gccr | GCCR_LTI, GCCR);

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int ravb_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct ravb_private *priv = container_of(ptp, struct ravb_private,
						 ptp.info);
	struct timespec64 ts;
	unsigned long flags;
	int error;

	spin_lock_irqsave(&priv->lock, flags);
	error = ravb_ptp_time_read(priv, &ts);
	if (!error) {
		u64 now = ktime_to_ns(timespec64_to_ktime(ts));

		ts = ns_to_timespec64(now + delta);
		error = ravb_ptp_time_write(priv, &ts);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return error;
}

static int ravb_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct ravb_private *priv = container_of(ptp, struct ravb_private,
						 ptp.info);
	unsigned long flags;
	int error;

	spin_lock_irqsave(&priv->lock, flags);
	error = ravb_ptp_time_read(priv, ts);
	spin_unlock_irqrestore(&priv->lock, flags);

	return error;
}

static int ravb_ptp_settime64(struct ptp_clock_info *ptp,
			      const struct timespec64 *ts)
{
	struct ravb_private *priv = container_of(ptp, struct ravb_private,
						 ptp.info);
	unsigned long flags;
	int error;

	spin_lock_irqsave(&priv->lock, flags);
	error = ravb_ptp_time_write(priv, ts);
	spin_unlock_irqrestore(&priv->lock, flags);

	return error;
}

static int ravb_ptp_extts(struct ptp_clock_info *ptp,
			  struct ptp_extts_request *req, int on)
{
	struct ravb_private *priv = container_of(ptp, struct ravb_private,
						 ptp.info);
	struct net_device *ndev = priv->ndev;
	unsigned long flags;

	if (req->index)
		return -EINVAL;

	if (priv->ptp.extts[req->index] == on)
		return 0;
	priv->ptp.extts[req->index] = on;

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->chip_id == RCAR_GEN2)
		ravb_modify(ndev, GIC, GIC_PTCE, on ? GIC_PTCE : 0);
	else if (on)
		ravb_write(ndev, GIE_PTCS, GIE);
	else
		ravb_write(ndev, GID_PTCD, GID);
	mmiowb();
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int ravb_ptp_perout(struct ptp_clock_info *ptp,
			   struct ptp_perout_request *req, int on)
{
	struct ravb_private *priv = container_of(ptp, struct ravb_private,
						 ptp.info);
	struct net_device *ndev = priv->ndev;
	struct ravb_ptp_perout *perout;
	unsigned long flags;
	int error = 0;

	if (req->index)
		return -EINVAL;

	if (on) {
		u64 start_ns;
		u64 period_ns;

		start_ns = req->start.sec * NSEC_PER_SEC + req->start.nsec;
		period_ns = req->period.sec * NSEC_PER_SEC + req->period.nsec;

		if (start_ns > U32_MAX) {
			netdev_warn(ndev,
				    "ptp: start value (nsec) is over limit. Maximum size of start is only 32 bits\n");
			return -ERANGE;
		}

		if (period_ns > U32_MAX) {
			netdev_warn(ndev,
				    "ptp: period value (nsec) is over limit. Maximum size of period is only 32 bits\n");
			return -ERANGE;
		}

		spin_lock_irqsave(&priv->lock, flags);

		perout = &priv->ptp.perout[req->index];
		perout->target = (u32)start_ns;
		perout->period = (u32)period_ns;
		error = ravb_ptp_update_compare(priv, (u32)start_ns);
		if (!error) {
			/* Unmask interrupt */
			if (priv->chip_id == RCAR_GEN2)
				ravb_modify(ndev, GIC, GIC_PTME, GIC_PTME);
			else
				ravb_write(ndev, GIE_PTMS0, GIE);
		}
	} else	{
		spin_lock_irqsave(&priv->lock, flags);

		perout = &priv->ptp.perout[req->index];
		perout->period = 0;

		/* Mask interrupt */
		if (priv->chip_id == RCAR_GEN2)
			ravb_modify(ndev, GIC, GIC_PTME, 0);
		else
			ravb_write(ndev, GID_PTMD0, GID);
	}
	mmiowb();
	spin_unlock_irqrestore(&priv->lock, flags);

	return error;
}

static int ravb_ptp_enable(struct ptp_clock_info *ptp,
			   struct ptp_clock_request *req, int on)
{
	switch (req->type) {
	case PTP_CLK_REQ_EXTTS:
		return ravb_ptp_extts(ptp, &req->extts, on);
	case PTP_CLK_REQ_PEROUT:
		return ravb_ptp_perout(ptp, &req->perout, on);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct ptp_clock_info ravb_ptp_info = {
	.owner		= THIS_MODULE,
	.name		= "ravb clock",
	.max_adj	= 50000000,
	.n_ext_ts	= N_EXT_TS,
	.n_per_out	= N_PER_OUT,
	.adjfreq	= ravb_ptp_adjfreq,
	.adjtime	= ravb_ptp_adjtime,
	.gettime64	= ravb_ptp_gettime64,
	.settime64	= ravb_ptp_settime64,
	.enable		= ravb_ptp_enable,
};

/* Caller must hold the lock */
irqreturn_t ravb_ptp_interrupt(struct net_device *ndev)
{
	struct ravb_private *priv = netdev_priv(ndev);
	u32 gis = ravb_read(ndev, GIS);
	irqreturn_t result = IRQ_NONE;

	gis &= ravb_read(ndev, GIC);
	if (gis & GIS_PTCF) {
		struct ptp_clock_event event;

		event.type = PTP_CLOCK_EXTTS;
		event.index = 0;
		event.timestamp = ravb_read(ndev, GCPT);
		ptp_clock_event(priv->ptp.clock, &event);

		result = IRQ_HANDLED;
		ravb_write(ndev, ~(GIS_PTCF | GIS_RESERVED), GIS);
	}
	if (gis & 0xffff0000) { // Any ATCFi bit
		ravb_avtp_capture_int(priv, gis);

		result = IRQ_HANDLED;
		ravb_write(ndev, ~((gis&0xffff0000) | GIS_RESERVED), GIS);
	}
	if (gis & GIS_PTMF) {
		struct ravb_ptp_perout *perout = priv->ptp.perout;

		if (perout->period) {
			perout->target += perout->period;
			ravb_ptp_update_compare(priv, perout->target);
		}

		result = IRQ_HANDLED;
		ravb_write(ndev, ~(GIS_PTMF | GIS_RESERVED), GIS);
	}

	return result;
}

void ravb_ptp_init(struct net_device *ndev, struct platform_device *pdev)
{
	struct ravb_private *priv = netdev_priv(ndev);
	unsigned long flags;
	unsigned capture_unit;

	priv->ptp.info = ravb_ptp_info;

	priv->ptp.default_addend = ravb_read(ndev, GTI);
	priv->ptp.current_addend = priv->ptp.default_addend;

	init_waitqueue_head(&priv->avtp_capture_wq);
	for(capture_unit = 0;capture_unit < NUM_AVTP_CAPTURE;++capture_unit) {
		INIT_LIST_HEAD(&priv->avtp_capture[capture_unit]);
	}

	spin_lock_irqsave(&priv->lock, flags);
	ravb_wait(ndev, GCCR, GCCR_TCR, GCCR_TCR_NOREQ);
	ravb_modify(ndev, GCCR, GCCR_TCSS, GCCR_TCSS_ADJGPTP);
	mmiowb();
	spin_unlock_irqrestore(&priv->lock, flags);

	priv->ptp.clock = ptp_clock_register(&priv->ptp.info, &pdev->dev);
}


static int ravb_mediats_close(struct ctc_mediats_ctx* ctx);
static int ravb_mediats_get(struct ctc_mediats_ctx* ctx, uint32_t *ts, bool blocking);
static int ravb_mediats_flush(struct ctc_mediats_ctx *mctx);

static const struct ctc_mediats_ops ravb_mediats_ops = {
	.close = &ravb_mediats_close,
	.get = &ravb_mediats_get,
	.flush = &ravb_mediats_flush,
};

struct ravb_avtp_capture {
	struct ctc_mediats_ctx ctx;
	struct list_head list;
	struct ravb_private *priv;
	unsigned ring_size, read_pos, write_pos, overruns;
	u8 capture_unit, prescaler;
	u32 timestamps[];
};

static u32 ravb_avtp_capture_gis_bit(unsigned capture_unit)
{
	return (GIS_ATCF0<<capture_unit);
}

struct ctc_mediats_ctx* ravb_mediats_open(struct net_device *ndev, unsigned capture_unit,
										  u8 prescaler, unsigned ring_size)
{
	struct ravb_private *priv = netdev_priv(ndev);
	struct ravb_avtp_capture *ret, *it;
	u32 gis;

	if (capture_unit >= NUM_AVTP_CAPTURE || !prescaler)
		return NULL;

	ret = kvzalloc(sizeof(*ret)+4*ring_size, GFP_KERNEL);
	if (!ret)
		return NULL;
	ret->ctx.ops = &ravb_mediats_ops;
	ret->priv = priv;
	ret->ring_size = ring_size;
	ret->prescaler = prescaler;
	ret->capture_unit = capture_unit;

	spin_lock_irq(&priv->lock);
	/* For now, only support the trivial case that all contexts on the same unit
	   have the same prescaler. */
	list_for_each_entry(it, &priv->avtp_capture[capture_unit], list) {
		if (it->prescaler != prescaler)
			goto err_free;
	}

	if (list_empty(&priv->avtp_capture[capture_unit])) {
		ravb_write(ndev, (capture_unit<<8)|(prescaler-1), GACP);

		/* Clear any stale data */
		gis = ravb_read(ndev, GIS);
		if (gis&ravb_avtp_capture_gis_bit(capture_unit)) {
			ravb_read(ndev, GCAT0+capture_unit*4);
			ravb_write(ndev, ~(ravb_avtp_capture_gis_bit(capture_unit) | GIS_RESERVED), GIS);
		}

		ravb_write(ndev, GIE_ATCS0<<capture_unit, GIE);

		netdev_info(ndev, "AVTP: Opened unit %u with prescaler %hhu as %pK", capture_unit, prescaler, ret);
	}
	netdev_info(ndev, "AVTP: Created context %pK with queue depth %u\n", &ret->ctx, ret->ring_size);

	list_add(&ret->list, &priv->avtp_capture[capture_unit]);
	goto done;

  err_free:
	kvfree(ret);
	ret = NULL;
  done:
	spin_unlock_irq(&priv->lock);
	return ret?&ret->ctx:NULL;
}
EXPORT_SYMBOL(ravb_mediats_open);

static int ravb_mediats_close(struct ctc_mediats_ctx *mctx)
{
	struct ravb_avtp_capture *ctx = container_of(mctx, struct ravb_avtp_capture, ctx);
	struct ravb_private *priv = ctx->priv;
	struct net_device *ndev = priv->ndev;

	spin_lock_irq(&priv->lock);
	list_del(&ctx->list);
	if (list_empty(&priv->avtp_capture[ctx->capture_unit]))
		ravb_write(ndev, GID_ATCD0<<ctx->capture_unit, GID);
	spin_unlock_irq(&priv->lock);

	kvfree(ctx);
	return 0;
}

/* Caller must hold the lock */
static void ravb_avtp_capture_int(struct ravb_private *priv, u32 gis)
{
	struct net_device *ndev = priv->ndev;
	bool wake = false;
	unsigned capture_unit;

	for(capture_unit = 0; capture_unit < NUM_AVTP_CAPTURE;++capture_unit) {
		if (gis&ravb_avtp_capture_gis_bit(capture_unit)) {
			struct ravb_avtp_capture *unit;
			u32 timestamp = ravb_read(ndev, GCAT0+capture_unit*4);
			if (list_empty(&priv->avtp_capture[capture_unit])) {
				// Spurious interrupt?
				netdev_warn(ndev, "AVTP: Got spurious int on capture unit %u\n",
							capture_unit);
				continue;
			}
			list_for_each_entry(unit, &priv->avtp_capture[capture_unit], list) {
				if ((unit->write_pos+1)%unit->ring_size == unit->read_pos) {
					// Overflow?
					if (!unit->overruns) // Warn on first overrun only
						netdev_warn(ndev, "AVTP: Timestamp buffer overflow on capture unit %u (r: %u, w: %u)\n",
									capture_unit, unit->read_pos, unit->write_pos);
					++unit->overruns;
					continue;
				}
				wake = true;
				unit->timestamps[unit->write_pos] = timestamp;
				unit->write_pos = (unit->write_pos+1)%unit->ring_size;
			}
		}
	}

	if (wake)
		// Got timestamps, wake sleepers
		wake_up_all(&priv->avtp_capture_wq);
}

static int ravb_mediats_get(struct ctc_mediats_ctx *mctx, u32 *avtp, bool wait)
{
	struct ravb_avtp_capture *ctx = container_of(mctx, struct ravb_avtp_capture, ctx);
	struct ravb_private *priv = ctx->priv;
	int err = 0;

	if (!avtp) {
		return -EINVAL;
	}

	spin_lock_irq(&priv->lock);
	if (ctx->overruns > 0) {
		err = -EIO;
		ctx->overruns = 0;
		goto out;
	}
	if (ctx->read_pos == ctx->write_pos) {
		if (!wait) {
			err = -EAGAIN;
			goto out;
		}
		if (current->flags & PF_KTHREAD) {
			wait_event_lock_irq(
				priv->avtp_capture_wq,
				ctx->read_pos != ctx->write_pos || kthread_should_stop(),
				priv->lock);
			if (kthread_should_stop())
				err = -ERESTARTSYS;
		} else {
			err = wait_event_interruptible_lock_irq(
				priv->avtp_capture_wq,
				ctx->read_pos != ctx->write_pos,
				priv->lock);
		}
		if (err != 0)
			goto out;
	}

	*avtp = ctx->timestamps[ctx->read_pos];
	ctx->read_pos = (ctx->read_pos+1)%ctx->ring_size;

  out:
	spin_unlock_irq(&priv->lock);
	return err;
}

static int ravb_mediats_flush(struct ctc_mediats_ctx *mctx)
{
	struct ravb_avtp_capture *ctx = container_of(mctx, struct ravb_avtp_capture, ctx);
	struct ravb_private *priv = ctx->priv;
	spin_lock_irq(&priv->lock);
	ctx->overruns = 0;
	ctx->read_pos = ctx->write_pos;
	spin_unlock_irq(&priv->lock);
	return 0;
}

void ravb_ptp_stop(struct net_device *ndev)
{
	struct ravb_private *priv = netdev_priv(ndev);

	ravb_write(ndev, 0, GIC);
	ravb_write(ndev, 0, GIS);

	ptp_clock_unregister(priv->ptp.clock);
}
