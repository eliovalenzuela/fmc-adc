/*
 * Copyright CERN 2012, author Federico Vaga
 *
 * Trigger for the driver of the mezzanine ADC
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#include "spec.h"
#include "fmc-adc.h"

struct zfat_instance {
	struct zio_ti ti;
	struct spec_fa *fa;
	unsigned int n_acq_dev;	/* number of acquisitions on device memory */
	unsigned int n_err;	/* number of errors */
};
#define to_zfat_instance(_ti) container_of(_ti, struct zfat_instance, ti)

/* zio trigger attributes */
static DEFINE_ZATTR_STD(TRIG, zfat_std_zattr) = {
	/* Number of shots */
	ZATTR_REG(trig, ZATTR_TRIG_REENABLE, S_IRUGO | S_IWUGO, ZFAT_SHOTS_NB, 0),

	/*
	 * The ADC has pre-sample and post-sample configuration. NSAMPLES doesn't
	 * apply in this case, so it is a read-only attribute update with the
	 * value calculated pre-sample + post-sample.
	 * If read from device, it contains the numer of samples acquired in a
	 * particular moment
	 */
	ZATTR_REG(trig, ZATTR_TRIG_NSAMPLES, S_IRUGO, ZFAT_CNT, 0),
};
static struct zio_attribute zfat_ext_zattr[] = {
	/* Config register */
	/* Hardware trigger selction
	 * 0: internal (data threshold)
	 * 1: external (front panel trigger input)
	 */
	ZATTR_EXT_REG("hw_select", S_IRUGO | S_IWUGO, ZFAT_CFG_INT_SEL, 0),
	/*
	 * Hardware trigger polarity
	 * 0: positive edge/slope
	 * 1: negative edge/slope
	 */
	ZATTR_EXT_REG("polarity", S_IRUGO | S_IWUGO, ZFAT_CFG_HW_POL, 0),
	/* Enable (1) or disable (0) hardware trigger */
	ZATTR_EXT_REG("hw_trig_enable", S_IRUGO | S_IWUGO, ZFAT_CFG_HW_EN, 0),
	/* Enable (1) or disable (0) software trigger */
	ZATTR_EXT_REG("sw_trig_enable", S_IRUGO | S_IWUGO, ZFAT_CFG_SW_EN, 0),
	/*
	 * Channel selection for internal trigger
	 * 0: channel 1
	 * 1: channel 2
	 * 2: channel 3
	 * 3: channel 4
	 */
	ZATTR_EXT_REG("int_select", S_IRUGO | S_IWUGO, ZFAT_CFG_INT_SEL, 0),
	/* Threshold value for internal trigger */
	ZATTR_EXT_REG("int_threshold", S_IRUGO | S_IWUGO, ZFAT_CFG_THRES, 0),

	/* Delay */
	ZATTR_EXT_REG("delay", S_IRUGO | S_IWUGO, ZFAT_DLY, 0),

	/* Software */
	PARAM_EXT_REG("sw_fire", S_IWUGO, ZFAT_SW, 0),

	/* Position address */
	ZATTR_EXT_REG("position_addr", S_IRUGO, ZFAT_POS, 0),

	/* Pre-sample*/
	ZATTR_EXT_REG("pre-sample", S_IRUGO | S_IWUGO, ZFAT_PRE, 0),

	/* Post-sample*/
	ZATTR_EXT_REG("post-sample", S_IRUGO | S_IWUGO, ZFAT_POST, 0),

	/* IRQ status */
	PARAM_EXT_REG("irq-status", S_IRUGO, ZFA_IRQ_SRC, 0),
	/* IRQ status */
	PARAM_EXT_REG("irq-multi", S_IRUGO, ZFA_IRQ_MULTI, 0),
	/* IRQ status */
	PARAM_EXT_REG("irq-mask", S_IRUGO, ZFA_IRQ_MASK, 0),
};


/* set a value to a FMC-ADC trigger register */
static int zfat_conf_set(struct device *dev, struct zio_attribute *zattr,
			 uint32_t usr_val)
{
	const struct zio_reg_desc *reg = &zfad_regs[zattr->priv.addr];
	struct zio_ti *ti = to_zio_ti(dev);
	int err;
	/* These static value are synchronized with the ZIO attribute value */
	static uint32_t pre_s = 0, post_s = 0;

	switch (zattr->priv.addr) {
		case ZFAT_SW:
			/* Fire if software trigger is enabled */
			if (!ti->zattr_set.ext_zattr[3].value) {
				dev_err(dev, "sw trigger must be enable");
				return -EPERM;
			}
			/* Abort current acquisition if any */
			err = zfa_common_conf_set(dev,
					&zfad_regs[ZFA_CTL_FMS_CMD], 2);
			if (err)
				return err;
			zio_trigger_abort(ti->cset);
			/* Start a new acquisition */
			zio_fire_trigger(ti);
			break;
	}

	err = zfa_common_conf_set(dev, reg, usr_val);
	if (err)
		return err;

	/* Update static value */
	if (zattr->priv.addr == ZFAT_PRE) {
		pre_s = usr_val;
	}
	if (zattr->priv.addr == ZFAT_POST) {
		post_s = usr_val;
	}

	if ( zattr->priv.addr == ZFAT_PRE ||  zattr->priv.addr == ZFAT_POST) {
		/* Check if the acquisition of all required samples is possibile */
		if ((pre_s + post_s) * ti->cset->ssize >= FA_MAX_ACQ_BYTE) {
			dev_warn(&ti->cset->head.dev,
				 "you can't acquire more then %i samples"
				"(pre-samples = %i, post-samples = %i).",
				FA_MAX_ACQ_BYTE/ti->cset->ssize,
				pre_s, post_s);
		}
	}
	/* FIXME: zio need an update, introduce PRE and POST and replace NSAMPLES*/
	return 0;
}
/* get the value of a FMC-ADC trigger register */
static int zfat_info_get(struct device *dev, struct zio_attribute *zattr,
			 uint32_t *usr_val)
{
	zfa_common_info_get(dev, &zfad_regs[zattr->priv.addr], usr_val);

	return 0;
}
static const struct zio_sysfs_operations zfat_s_op = {
	.conf_set = zfat_conf_set,
	.info_get = zfat_info_get,
};

irqreturn_t zfadc_irq(int irq, void *ptr)
{
	struct zfat_instance *zfat = ptr;
	struct spec_fa *fa = zfat->fa;
	uint32_t irq_status = 0, val;

	zfa_common_info_get(&zfat->ti.cset->head.dev,
			    &zfad_regs[ZFA_IRQ_SRC],&irq_status);
	dev_dbg(&zfat->ti.head.dev, "irq status = 0x%x\n", irq_status);

	fa->fmc->op->irq_ack(fa->fmc);
	if (irq_status & (ZFAT_DMA_DONE | ZFAT_DMA_ERR)) {
		if (irq_status & ZFAT_DMA_DONE) { /* DMA done*/
			zio_trigger_data_done(zfat->ti.cset);
			zfat->n_acq_dev--;
		} else {	/* DMA error */
			zio_trigger_abort(zfat->ti.cset);
			zfat->n_err++;
		}
		/* unmap dma */
		zfad_unmap_dma(zfat->ti.cset);
		/* Enable all triggers */
		zfa_common_conf_set(&zfat->ti.cset->head.dev,
				    &zfad_regs[ZFAT_CFG_SW_EN], 0);
		zfa_common_conf_set(&zfat->ti.cset->head.dev,
				    &zfad_regs[ZFAT_CFG_HW_EN], 0);
		/* Start state machine */
		zfa_common_conf_set(&zfat->ti.cset->head.dev,
				    &zfad_regs[ZFA_CTL_FMS_CMD], ZFA_START);
	}

	if (irq_status & ZFAT_TRG_FIRE) { /* Trigger fire */
		/*
		 * FIXME: I think we don't care about this because ZIO fires
		 * a fake trigger before DMA
		 */
		zfat->n_acq_dev++;
	}
	if (irq_status & ZFAT_ACQ_END) { /* Acquisition end */
		/*
		 * We fire the trigger at the and of the acquisition because
		 * FMC-ADC allow DMA only when acquisition end and the
		 * state machine is in the idle status. When the real trigger
		 * fire the state machine is not idle. This has not any
		 * influence on the ZIO side: we are interested to DMA data
		 */
		zfa_common_info_get(&zfat->ti.cset->head.dev,
				    &zfad_regs[ZFA_STA_FSM],&val);
		if (val == ZFA_STATE_IDLE) {
			dev_dbg(&zfat->ti.head.dev, "Start DMA from device\n");
			/* Stop state machine */
			zfa_common_conf_set(&zfat->ti.cset->head.dev,
					    &zfad_regs[ZFA_CTL_FMS_CMD],
					    ZFA_STOP);
			/* Disable all triggers */
			zfa_common_conf_set(&zfat->ti.cset->head.dev,
					    &zfad_regs[ZFAT_CFG_HW_EN], 0);
			zfa_common_conf_set(&zfat->ti.cset->head.dev,
					    &zfad_regs[ZFAT_CFG_SW_EN], 0);
			/* Fire ZIO trigger so it will DMA */
			zio_fire_trigger(&zfat->ti);
		} else {
			/* we can't DMA if the state machine is not idle */
			dev_warn(&zfat->ti.cset->head.dev,
				 "Can't start DMA on the last acquisition\n");
		}
	}

	return IRQ_HANDLED;
}

/* create an instance of the FMC-ADC trigger */
static struct zio_ti *zfat_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct spec_fa *fa = cset->zdev->priv_d;
	struct zfat_instance *zfat;
	int err;

	if (!fa) {
		dev_err(&cset->head.dev, "no spec device defined\n");
		return ERR_PTR(-ENODEV);
	}

	zfat = kzalloc(sizeof(struct zfat_instance), GFP_KERNEL);
	if (!zfat)
		return ERR_PTR(-ENOMEM);

	zfat->fa = fa;

	err = fa->fmc->op->irq_request(fa->fmc, zfadc_irq, "fmc-adc", 0);
	if (err) {
		dev_err(&fa->spec->pdev->dev, "can't request irq %i (err %i)\n",
				fa->spec->pdev->irq, err);
		return ERR_PTR(err);
	}

	/* Enable all interrupt */
	zfa_common_conf_set(&cset->head.dev, &zfad_regs[ZFA_IRQ_MASK],
			    ZFAT_ALL);

	return &zfat->ti;
}

static void zfat_destroy(struct zio_ti *ti)
{
	struct spec_fa *fa = ti->cset->zdev->priv_d;
	struct zfat_instance *zfat = to_zfat_instance(ti);

	/* Disable all interrupt */
	zfa_common_conf_set(&ti->cset->head.dev, &zfad_regs[ZFA_IRQ_MASK],
			    ZFAT_NONE);
	fa->fmc->op->irq_free(fa->fmc);
	kfree(zfat);
}

/* status is active low on ZIO but active high on the FMC-ADC, then use ! */
static void zfat_change_status(struct zio_ti *ti, unsigned int status)
{
	/* Enable/Disable HW trigger */
	zfa_common_conf_set(&ti->head.dev, &zfad_regs[ZFAT_CFG_HW_EN], !status);
	/* Enable/Disable SW trigger */
	zfa_common_conf_set(&ti->head.dev, &zfad_regs[ZFAT_CFG_SW_EN], !status);
}

/* Transfer is over, store the interleaved block */
static void zfat_data_done(struct zio_cset *cset)
{
	struct zio_block *block = cset->interleave->active_block;
	struct zio_buffer_type *zbuf = cset->zbuf;
	struct zio_bi *bi = cset->interleave->bi;

	if (!block)
		return;
	if (zbuf->b_op->store_block(bi, block)) /* may fail, no prob */
		zbuf->b_op->free_block(bi, block);
}
/*
 * interleaved trigger fire. It set the control timestamp with the
 * hardware timestamp value
 */
static void zfat_input_fire(struct zio_ti *ti)
{
	struct zio_channel *interleave = ti->cset->interleave;
	struct zio_timestamp *tstamp = &interleave->current_ctrl->tstamp;
	struct zio_buffer_type *zbuf = ti->cset->zbuf;
	struct zio_control *ctrl;
	struct zio_block *block;
	int err;

	ctrl = zio_alloc_control(GFP_ATOMIC);
	/* Update sequence number */
	interleave->current_ctrl->seq_num++;

	/* Retrieve last trigger time-stamp (hw trigger already fired) */
	zfa_common_info_get(&ti->cset->head.dev,
			    &zfad_regs[ZFA_UTC_TRIG_SECONDS], &tstamp->secs);
	zfa_common_info_get(&ti->cset->head.dev,
			    &zfad_regs[ZFA_UTC_TRIG_COARSE], &tstamp->ticks);
	zfa_common_info_get(&ti->cset->head.dev,
			    &zfad_regs[ZFA_UTC_TRIG_FINE], &tstamp->bins);
	memcpy(ctrl, interleave->current_ctrl, ZIO_CONTROL_SIZE);

	/* Allocate a new block for DMA transfer */
	block = zbuf->b_op->alloc_block(interleave->bi, ctrl,
					ctrl->ssize * ctrl->nsamples,
					GFP_ATOMIC);
	if (IS_ERR(block)) {
		dev_err(&ti->cset->head.dev, "can't alloc block\n");
		goto out;
	}
	interleave->active_block = block;

	err = ti->cset->raw_io(ti->cset);
	if (err != -EAGAIN) {
		dev_err(&ti->cset->head.dev, "Can't transfer err %i \n", err);
		goto out_free;
	}
	return;

out_free:
	zbuf->b_op->free_block(interleave->bi, block);
out:
	zio_free_control(ctrl);
	interleave->active_block = NULL;
}

/* Abort depends on the state machine status and DMA status.*/
static void zfat_abort(struct zio_cset *cset)
{
	struct zio_block *block = cset->interleave->active_block;
	struct zio_buffer_type *zbuf = cset->zbuf;
	struct zio_bi *bi = cset->interleave->bi;

	zbuf->b_op->free_block(bi, block);
}

static const struct zio_trigger_operations zfat_ops = {
	.create =		zfat_create,
	.destroy =		zfat_destroy,
	.change_status =	zfat_change_status,
	.data_done =		zfat_data_done,
	.input_fire =		zfat_input_fire,
	.abort =		zfat_abort,
};

struct zio_trigger_type zfat_type = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = zfat_std_zattr,
		.ext_zattr = zfat_ext_zattr,
		.n_ext_attr = ARRAY_SIZE(zfat_ext_zattr),
	},
	.s_op = &zfat_s_op,
	.t_op = &zfat_ops,
};

