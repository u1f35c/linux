// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022 StarFive, Inc <clivia.cai@starfivetech.com>
 *
 * THE PRESENT SOFTWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING
 * CUSTOMERS WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER
 * FOR THEM TO SAVE TIME. AS A RESULT, STARFIVE SHALL NOT BE HELD LIABLE
 * FOR ANY DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY
 * CLAIMS ARISING FROM THE CONTENT OF SUCH SOFTWARE AND/OR THE USE MADE
 * BY CUSTOMERS OF THE CODING INFORMATION CONTAINED HEREIN IN CONNECTION
 * WITH THEIR PRODUCTS.
 */


#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

#define ALL_INT_CLR		0x1ffff
#define MAX_DELAY_CHAIN		32
struct starfive_priv {
	struct device *dev;
	struct regmap *reg_syscon;
	u32 syscon_offset;
	u32 syscon_shift;
	u32 syscon_mask;
};

static unsigned long dw_mci_starfive_caps[] = {
	MMC_CAP_CMD23,
	MMC_CAP_CMD23,
	MMC_CAP_CMD23
};

static void dw_mci_starfive_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	int ret;
	unsigned int clock;

	if (ios->timing == MMC_TIMING_MMC_DDR52 || ios->timing == MMC_TIMING_UHS_DDR50) {
		clock = (ios->clock > 50000000 && ios->clock <= 52000000) ? 100000000 : ios->clock;
		ret = clk_set_rate(host->ciu_clk, clock);
		if (ret)
			dev_dbg(host->dev, "Use an external frequency divider %uHz\n", ios->clock);
		host->bus_hz = clk_get_rate(host->ciu_clk);
	} else {
		dev_dbg(host->dev, "Using the internal divider\n");
	}
}

static int dw_mci_starfive_execute_tuning(struct dw_mci_slot *slot,
					     u32 opcode)
{
	static const int grade  = MAX_DELAY_CHAIN;
	struct dw_mci *host = slot->host;
	struct starfive_priv *priv = host->priv;
	int raise_point = -1, fall_point = -1;
	int err, prev_err = -1;
	int found = 0;
	int i;
	u32 regval;

	for (i = 0; i < grade; i++) {
		regval = i << priv->syscon_shift;
		err = regmap_update_bits(priv->reg_syscon, priv->syscon_offset, priv->syscon_mask, regval);
		if (err)
			return err;
		mci_writel(host, RINTSTS, ALL_INT_CLR);

		err = mmc_send_tuning(slot->mmc, opcode, NULL);
		if (!err)
			found = 1;

		if (i > 0) {
			if (err && !prev_err)
				fall_point = i - 1;
			if (!err && prev_err)
				raise_point = i;
		}

		if (raise_point != -1 && fall_point != -1)
			goto tuning_out;

		prev_err = err;
		err = 0;
	}

tuning_out:
	if (found) {
		if (raise_point == -1)
			raise_point = 0;
		if (fall_point == -1)
			fall_point = grade - 1;
		if (fall_point < raise_point) {
			if ((raise_point + fall_point) >
			    (grade - 1))
				i = fall_point / 2;
			else
				i = (raise_point + grade - 1) / 2;
		} else {
			i = (raise_point + fall_point) / 2;
		}

		regval = i << priv->syscon_shift;
		err = regmap_update_bits(priv->reg_syscon, priv->syscon_offset, priv->syscon_mask, regval);
		if (err)
			return err;
		dev_dbg(host->dev, "Found valid delay chain! use it [delay=%d]\n", i);
	} else {
		dev_err(host->dev, "No valid delay chain! use default\n");
		err = -EINVAL;
	}

	mci_writel(host, RINTSTS, ALL_INT_CLR);
	return err;
}

static int dw_mci_starfive_switch_voltage(struct mmc_host *mmc, struct mmc_ios *ios)
{

	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	u32 ret;

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330)
		ret = gpio_direction_output(25, 0);
	else if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)
		ret = gpio_direction_output(25, 1);
	if (ret)
		return ret;

	if (!IS_ERR(mmc->supply.vqmmc)) {
		ret = mmc_regulator_set_vqmmc(mmc, ios);
		if (ret < 0) {
			dev_err(host->dev, "Regulator set error %d\n", ret);
			return ret;
		}
	}

	/* We should delay 20ms wait for timing setting finished. */
	mdelay(20);
	return 0;
}

static int dw_mci_starfive_parse_dt(struct dw_mci *host)
{
	struct of_phandle_args args;
	struct starfive_priv *priv;
	int ret;

	priv = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = of_parse_phandle_with_fixed_args(host->dev->of_node,
						"starfive,sys-syscon", 3, 0, &args);
	if (ret) {
		dev_err(host->dev, "Failed to parse starfive,sys-syscon\n");
		return -EINVAL;
	}

	priv->reg_syscon = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(priv->reg_syscon))
		return PTR_ERR(priv->reg_syscon);

	priv->syscon_offset = args.args[0];
	priv->syscon_shift  = args.args[1];
	priv->syscon_mask   = args.args[2];

	host->priv = priv;

	return 0;
}

static const struct dw_mci_drv_data starfive_data = {
	.caps = dw_mci_starfive_caps,
	.num_caps = ARRAY_SIZE(dw_mci_starfive_caps),
	.set_ios = dw_mci_starfive_set_ios,
	.parse_dt = dw_mci_starfive_parse_dt,
	.execute_tuning = dw_mci_starfive_execute_tuning,
	.switch_voltage  = dw_mci_starfive_switch_voltage,
};

static const struct of_device_id dw_mci_starfive_match[] = {
	{ .compatible = "starfive,jh7110-sdio",
		.data = &starfive_data },
	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_starfive_match);

static int dw_mci_starfive_probe(struct platform_device *pdev)
{
	const struct dw_mci_drv_data *drv_data;
	const struct of_device_id *match;
	int ret;

	match = of_match_node(dw_mci_starfive_match, pdev->dev.of_node);
	drv_data = match->data;

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = dw_mci_pltfm_register(pdev, drv_data);
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		pm_runtime_set_suspended(&pdev->dev);
		pm_runtime_put_noidle(&pdev->dev);

		return ret;
	}

	return 0;
}

static int dw_mci_starfive_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	return dw_mci_pltfm_remove(pdev);
}

#ifdef CONFIG_PM
static int dw_mci_starfive_runtime_suspend(struct device *dev)
{
	struct dw_mci *host = dev_get_drvdata(dev);

	clk_disable_unprepare(host->biu_clk);
	clk_disable_unprepare(host->ciu_clk);

	return 0;
}

static int dw_mci_starfive_runtime_resume(struct device *dev)
{
	struct dw_mci *host = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(host->biu_clk);
	if (ret) {
		dev_err(host->dev, "Failed to prepare_enable biu_clk clock\n");
		return ret;
	}

	ret = clk_prepare_enable(host->ciu_clk);
	if (ret) {
		dev_err(host->dev, "Failed to prepare_enable ciu_clk clock\n");
		return ret;
	}

	return 0;
}
#endif

static const struct dev_pm_ops dw_mci_starfive_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(dw_mci_starfive_runtime_suspend,
			   dw_mci_starfive_runtime_resume, NULL)
};

static struct platform_driver dw_mci_starfive_driver = {
	.probe = dw_mci_starfive_probe,
	.remove = dw_mci_starfive_remove,
	.driver = {
		.name = "dwmmc_starfive",
		.pm   = &dw_mci_starfive_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = dw_mci_starfive_match,
	},
};
module_platform_driver(dw_mci_starfive_driver);

MODULE_DESCRIPTION("StarFive JH7110 Specific DW-MSHC Driver Extension");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:dwmmc_starfive");
