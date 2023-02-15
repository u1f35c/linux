// SPDX-License-Identifier: GPL-2.0
/*
 * SPDIF driver for the StarFive JH7110 SoC
 *
 * Copyright (C) 2022 StarFive Technology Co., Ltd.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/dmaengine_pcm.h>
#include "starfive_spdif.h"

static irqreturn_t spdif_irq_handler(int irq, void *dev_id)
{
	struct sf_spdif_dev *dev = dev_id;
	bool irq_valid = false;
	unsigned int intr;
	unsigned int stat;

	regmap_read(dev->regmap, SPDIF_INT_REG, &intr);
	regmap_read(dev->regmap, SPDIF_STAT_REG, &stat);
	regmap_update_bits(dev->regmap, SPDIF_CTRL,
		SPDIF_MASK_ENABLE, 0);
	regmap_update_bits(dev->regmap, SPDIF_INT_REG,
		SPDIF_INT_REG_BIT, 0);

	if ((stat & SPDIF_EMPTY_FLAG) || (stat & SPDIF_AEMPTY_FLAG)) {
		sf_spdif_pcm_push_tx(dev);
		irq_valid = true;
	}

	if ((stat & SPDIF_FULL_FLAG) || (stat & SPDIF_AFULL_FLAG)) {
		sf_spdif_pcm_pop_rx(dev);
		irq_valid = true;
	}

	if (stat & SPDIF_PARITY_FLAG)
		irq_valid = true;

	if (stat & SPDIF_UNDERR_FLAG)
		irq_valid = true;

	if (stat & SPDIF_OVRERR_FLAG)
		irq_valid = true;

	if (stat & SPDIF_SYNCERR_FLAG)
		irq_valid = true;

	if (stat & SPDIF_LOCK_FLAG)
		irq_valid = true;

	if (stat & SPDIF_BEGIN_FLAG)
		irq_valid = true;

	if (stat & SPDIF_RIGHT_LEFT)
		irq_valid = true;

	regmap_update_bits(dev->regmap, SPDIF_CTRL,
		SPDIF_MASK_ENABLE, SPDIF_MASK_ENABLE);

	if (irq_valid)
		return IRQ_HANDLED;
	else
		return IRQ_NONE;
}

static int sf_spdif_trigger(struct snd_pcm_substream *substream, int cmd,
	struct snd_soc_dai *dai)
{
	struct sf_spdif_dev *spdif = snd_soc_dai_get_drvdata(dai);
	bool tx;

	tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	if (tx) {
		/* tx mode */
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_TR_MODE, SPDIF_TR_MODE);

		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_MASK_FIFO, SPDIF_EMPTY_MASK | SPDIF_AEMPTY_MASK);
	} else {
		/* rx mode */
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_TR_MODE, 0);

		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_MASK_FIFO, SPDIF_FULL_MASK | SPDIF_AFULL_MASK);
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/* clock recovery form the SPDIF data stream  0:clk_enable */
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_CLK_ENABLE, 0);

		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_ENABLE, SPDIF_ENABLE);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/* clock recovery form the SPDIF data stream  1:power save mode */
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_CLK_ENABLE, SPDIF_CLK_ENABLE);

		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_ENABLE, 0);
		break;
	default:
		dev_err(dai->dev, "%s L.%d cmd:%d\n", __func__, __LINE__, cmd);
		return -EINVAL;
	}

	return 0;
}

static int sf_spdif_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct sf_spdif_dev *spdif = snd_soc_dai_get_drvdata(dai);
	unsigned int channels;
	unsigned int rate;
	unsigned int format;
	unsigned int tsamplerate;
	unsigned int mclk;
	unsigned int audio_root;
	int ret;

	channels = params_channels(params);
	rate = params_rate(params);
	format = params_format(params);

	switch (channels) {
	case 1:
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_CHANNEL_MODE, SPDIF_CHANNEL_MODE);
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_DUPLICATE, SPDIF_DUPLICATE);
		spdif->channels = false;
		break;
	case 2:
		regmap_update_bits(spdif->regmap, SPDIF_CTRL,
			SPDIF_CHANNEL_MODE, 0);
		spdif->channels = true;
		break;
	default:
		dev_err(dai->dev, "invalid channels number\n");
		return -EINVAL;
	}

	switch (format) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_3LE:
	case SNDRV_PCM_FORMAT_S32_LE:
		break;
	default:
		dev_err(dai->dev, "invalid format\n");
		return -EINVAL;
	}

	audio_root = 204800000;
	switch (rate) {
	case 8000:
		mclk = 4096000;
		break;
	case 11025:
		mclk = 5644800;
		break;
	case 16000:
		mclk = 8192000;
		break;
	case 22050:
		audio_root = 153600000;
		mclk = 11289600;
		break;
	default:
		dev_err(dai->dev, "channel:%d sample rate:%d\n", channels, rate);
		return -EINVAL;
	}

	ret = clk_set_rate(spdif->audio_root, audio_root);
	if (ret) {
		dev_err(dai->dev, "failed to set audio_root rate :%d\n", ret);
		return ret;
	}
	dev_dbg(dai->dev, "audio_root get rate:%ld\n",
			clk_get_rate(spdif->audio_root));

	ret = clk_set_rate(spdif->mclk_inner, mclk);
	if (ret) {
		dev_err(dai->dev, "failed to set mclk_inner rate :%d\n", ret);
		return ret;
	}

	mclk = clk_get_rate(spdif->mclk_inner);
	dev_dbg(dai->dev, "mclk_inner get rate:%d\n", mclk);
	/* (FCLK)4096000/128=32000 */
	tsamplerate = (mclk / 128 + rate / 2) / rate - 1;
	if (tsamplerate < 3)
		tsamplerate = 3;

	/* transmission sample rate */
	regmap_update_bits(spdif->regmap, SPDIF_CTRL, 0xFF, tsamplerate);

	return 0;
}

static int sf_spdif_clks_get(struct platform_device *pdev,
				struct sf_spdif_dev *spdif)
{
	static struct clk_bulk_data clks[] = {
		{ .id = "spdif-apb" },		/* clock-names in dts file */
		{ .id = "spdif-core" },
		{ .id = "audroot" },
		{ .id = "mclk_inner"},
	};
	int ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(clks), clks);

	spdif->spdif_apb = clks[0].clk;
	spdif->spdif_core = clks[1].clk;
	spdif->audio_root = clks[2].clk;
	spdif->mclk_inner = clks[3].clk;
	return ret;
}

static int sf_spdif_resets_get(struct platform_device *pdev,
				struct sf_spdif_dev *spdif)
{
	struct reset_control_bulk_data resets[] = {
			{ .id = "rst_apb" },
	};
	int ret = devm_reset_control_bulk_get_exclusive(&pdev->dev, ARRAY_SIZE(resets), resets);

	if (ret)
		return ret;

	spdif->rst_apb = resets[0].rstc;

	return 0;
}

static int sf_spdif_clk_init(struct platform_device *pdev,
				struct sf_spdif_dev *spdif)
{
	int ret = 0;

	ret = clk_prepare_enable(spdif->spdif_apb);
	if (ret) {
		dev_err(&pdev->dev, "failed to prepare enable spdif_apb\n");
		goto disable_apb_clk;
	}

	ret = clk_prepare_enable(spdif->spdif_core);
	if (ret) {
		dev_err(&pdev->dev, "failed to prepare enable spdif_core\n");
		goto disable_core_clk;
	}

	ret = clk_set_rate(spdif->audio_root, 204800000);
	if (ret) {
		dev_err(&pdev->dev, "failed to set rate for spdif audroot ret=%d\n", ret);
		goto disable_core_clk;
	}

	ret = clk_set_rate(spdif->mclk_inner, 8192000);
	if (ret) {
		dev_err(&pdev->dev, "failed to set rate for spdif mclk_inner ret=%d\n", ret);
		goto disable_core_clk;
	}

	dev_dbg(&pdev->dev, "spdif->spdif_apb = %lu\n", clk_get_rate(spdif->spdif_apb));
	dev_dbg(&pdev->dev, "spdif->spdif_core = %lu\n", clk_get_rate(spdif->spdif_core));

	ret = reset_control_deassert(spdif->rst_apb);
	if (ret) {
		dev_err(&pdev->dev, "failed to deassert apb\n");
		goto disable_core_clk;
	}

	return 0;

disable_core_clk:
	clk_disable_unprepare(spdif->spdif_core);
disable_apb_clk:
	clk_disable_unprepare(spdif->spdif_apb);

	return ret;
}

static int sf_spdif_dai_probe(struct snd_soc_dai *dai)
{
	struct sf_spdif_dev *spdif = snd_soc_dai_get_drvdata(dai);

	/* reset */
	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_ENABLE | SPDIF_SFR_ENABLE | SPDIF_FIFO_ENABLE, 0);

	/* clear irq */
	regmap_update_bits(spdif->regmap, SPDIF_INT_REG,
		SPDIF_INT_REG_BIT, 0);

	/* power save mode */
	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_CLK_ENABLE, SPDIF_CLK_ENABLE);

	/* power save mode */
	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_CLK_ENABLE, SPDIF_CLK_ENABLE);

	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_PARITCHECK|SPDIF_VALIDITYCHECK|SPDIF_DUPLICATE,
		SPDIF_PARITCHECK|SPDIF_VALIDITYCHECK|SPDIF_DUPLICATE);

	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_SETPREAMBB, SPDIF_SETPREAMBB);

	regmap_update_bits(spdif->regmap, SPDIF_INT_REG,
		BIT8TO20MASK<<SPDIF_PREAMBLEDEL, 0x3<<SPDIF_PREAMBLEDEL);

	regmap_update_bits(spdif->regmap, SPDIF_FIFO_CTRL,
		ALLBITMASK, 0x20|(0x20<<SPDIF_AFULL_THRESHOLD));

	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_PARITYGEN, SPDIF_PARITYGEN);

	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_MASK_ENABLE, SPDIF_MASK_ENABLE);

	/* APB access to FIFO enable, disable if use DMA/FIFO */
	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_USE_FIFO_IF, 0);

	/* two channel */
	regmap_update_bits(spdif->regmap, SPDIF_CTRL,
		SPDIF_CHANNEL_MODE, 0);

	return 0;
}

static const struct snd_soc_dai_ops sf_spdif_dai_ops = {
	.trigger = sf_spdif_trigger,
	.hw_params = sf_spdif_hw_params,
};

#define SF_PCM_RATE_44100_192000  (SNDRV_PCM_RATE_44100 | \
				   SNDRV_PCM_RATE_48000 | \
				   SNDRV_PCM_RATE_96000 | \
				   SNDRV_PCM_RATE_192000)

#define SF_PCM_RATE_8000_22050  (SNDRV_PCM_RATE_8000 | \
				 SNDRV_PCM_RATE_11025 | \
				 SNDRV_PCM_RATE_16000 | \
				 SNDRV_PCM_RATE_22050)

static struct snd_soc_dai_driver sf_spdif_dai = {
	.name = "spdif",
	.id = 0,
	.probe = sf_spdif_dai_probe,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SF_PCM_RATE_8000_22050,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S24_3LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &sf_spdif_dai_ops,
	.symmetric_rate = 1,
};

static const struct snd_soc_component_driver sf_spdif_component = {
	.name = "starfive-spdif",
};

static const struct regmap_config sf_spdif_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x200,
};

static int sf_spdif_probe(struct platform_device *pdev)
{
	struct sf_spdif_dev *spdif;
	struct resource *res;
	void __iomem *base;
	int ret;
	int irq;

	spdif = devm_kzalloc(&pdev->dev, sizeof(*spdif), GFP_KERNEL);
	if (!spdif)
		return -ENOMEM;

	platform_set_drvdata(pdev, spdif);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	spdif->spdif_base = base;
	spdif->regmap = devm_regmap_init_mmio(&pdev->dev, spdif->spdif_base,
					    &sf_spdif_regmap_config);
	if (IS_ERR(spdif->regmap))
		return PTR_ERR(spdif->regmap);

	ret = sf_spdif_clks_get(pdev, spdif);
	if (ret) {
		dev_err(&pdev->dev, "failed to get audio clock\n");
		return ret;
	}

	ret = sf_spdif_resets_get(pdev, spdif);
	if (ret) {
		dev_err(&pdev->dev, "failed to get audio reset controls\n");
		return ret;
	}

	ret = sf_spdif_clk_init(pdev, spdif);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable audio clock\n");
		return ret;
	}

	spdif->dev = &pdev->dev;
	spdif->fifo_th = 16;

	irq = platform_get_irq(pdev, 0);
	if (irq >= 0) {
		ret = devm_request_irq(&pdev->dev, irq, spdif_irq_handler, 0,
				pdev->name, spdif);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed to request irq\n");
			return ret;
		}
	}

	ret = devm_snd_soc_register_component(&pdev->dev, &sf_spdif_component,
					 &sf_spdif_dai, 1);
	if (ret)
		goto err_clk_disable;

	if (irq >= 0) {
		ret = sf_spdif_pcm_register(pdev);
		spdif->use_pio = true;
	} else {
		ret = devm_snd_dmaengine_pcm_register(&pdev->dev, NULL,
					0);
		spdif->use_pio = false;
	}

	if (ret)
		goto err_clk_disable;

	return 0;

err_clk_disable:
	return ret;
}

static const struct of_device_id sf_spdif_of_match[] = {
	{ .compatible = "starfive,jh7110-spdif", },
	{},
};
MODULE_DEVICE_TABLE(of, sf_spdif_of_match);

static struct platform_driver sf_spdif_driver = {
	.driver = {
		.name = "starfive-spdif",
		.of_match_table = sf_spdif_of_match,
	},
	.probe = sf_spdif_probe,
};
module_platform_driver(sf_spdif_driver);

MODULE_AUTHOR("curry.zhang <curry.zhang@starfive.com>");
MODULE_DESCRIPTION("starfive SPDIF driver");
MODULE_LICENSE("GPL v2");
