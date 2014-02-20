/*
 * core fmc-adc-100m14b driver
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *		Copied from fine-delay fd-core.c
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/fmc.h>

#include "fmc-adc.h"

static struct fmc_driver fa_dev_drv;
FMC_PARAM_BUSID(fa_dev_drv);

static char *fa_binary = FA_GATEWARE_DEFAULT_NAME;
module_param_named(file, fa_binary, charp, 0444);

/* This structure lists the various subsystems */
struct fa_modlist {
	char *name;
	int (*init)(struct fa_dev *);
	void (*exit)(struct fa_dev *);
};

static struct fa_modlist mods[] = {
	{"spi", fa_spi_init, fa_spi_exit}, 
	{"onewire", fa_onewire_init, fa_onewire_exit},
	{"zio", fa_zio_init, fa_zio_exit},
};

/* probe and remove are called by fa-spec.c */
int fa_probe(struct fmc_device *fmc)
{
	struct fa_modlist *m = NULL;
	struct fa_dev *fa;
	int err, i = 0;

	/* Validate the new FMC device */
	i = fmc->op->validate(fmc, &fa_dev_drv);
	if (i < 0) {
		dev_info(&fmc->dev, "not using \"%s\" according to "
			 "modparam\n", KBUILD_MODNAME);
		return -ENODEV;
	}

	pr_info("%s:%d\n", __func__, __LINE__);
	/* Driver data */
	fa = devm_kzalloc(&fmc->dev, sizeof(struct fa_dev), GFP_KERNEL);
	if (!fa)
		return -ENOMEM;
	fmc_set_drvdata(fmc, fa);
	fa->fmc = fmc;

	/* We first write a new binary (and lm32) within the carrier */
	err = fmc->op->reprogram(fmc, &fa_dev_drv, fa_binary);
	if (err) {
		dev_err(fmc->hwdev, "write firmware \"%s\": error %i\n",
				fa_binary, err);
		goto out;
	}

	/* init all subsystems */
	for (i = 0, m = mods; i < ARRAY_SIZE(mods); i++, m++) {
		dev_dbg(&fmc->dev, "Calling init for \"%s\"\n", m->name);
		err = m->init(fa);
		if (err) {
			dev_err(&fmc->dev, "error initializing %s\n", m->name);
			goto out;
		}

	}

	return 0;
out:
	while (--m, --i >= 0)
		if (m->exit)
			m->exit(fa);
	return err;
}
int fa_remove(struct fmc_device *fmc)
{
	struct fa_dev *fa = fmc_get_drvdata(fmc);

	fa_zio_exit(fa);
	return 0;
}

static struct fmc_fru_id fa_fru_id[] = {
	{
		.product_name = "FmcAdc100m14b4cha",
	},
};

static struct fmc_driver fa_dev_drv = {
	.version = FMC_VERSION,
	.driver.name = KBUILD_MODNAME,
	.probe = fa_probe,
	.remove = fa_remove,
	.id_table = {
		.fru_id = fa_fru_id,
		.fru_id_nr = ARRAY_SIZE(fa_fru_id),
	},
};

static int fa_init(void)
{
	int ret;

	pr_debug("%s\n",__func__);

	/* First trigger and zio driver */
	ret = fa_trig_init();
	if (ret)
		return ret;

	ret = fa_zio_register();
	if (ret) {
		fa_trig_exit();
		return ret;
	}
	/* Finally the fmc driver, whose probe instantiates zio devices */
	ret = fmc_driver_register(&fa_dev_drv);
	if (ret) {
		fmc_driver_unregister(&fa_dev_drv);
		fa_trig_exit();
		fa_zio_unregister();
		return ret;
	}
	return 0;
}

static void fa_exit(void)
{
	fmc_driver_unregister(&fa_dev_drv);
	fa_zio_unregister();
	fa_trig_exit();
}

module_init(fa_init);
module_exit(fa_exit);

MODULE_AUTHOR("Federico Vaga");
MODULE_DESCRIPTION("FMC-ADC-100MS-14b Linux Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(GIT_VERSION);
