// SPDX-License-Identifier: GPL-2.0
/*
 * TDM driver for the StarFive JH7110 SoC
 *
 * Copyright (C) 2022 StarFive Technology Co., Ltd.
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/reset.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/dma/starfive-dma.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include "starfive_tdm.h"

static inline u32 sf_tdm_readl(struct sf_tdm_dev *dev, u16 reg)
{
	return readl_relaxed(dev->tdm_base + reg);
}

static inline void sf_tdm_writel(struct sf_tdm_dev *dev, u16 reg, u32 val)
{
	writel_relaxed(val, dev->tdm_base + reg);
}

static void sf_tdm_start(struct sf_tdm_dev *dev, struct snd_pcm_substream *substream)
{
	u32 data;
	unsigned int val;

	data = sf_tdm_readl(dev, TDM_PCMGBCR);
	sf_tdm_writel(dev, TDM_PCMGBCR, data | PCMGBCR_ENABLE);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		val = sf_tdm_readl(dev, TDM_PCMTXCR);
		sf_tdm_writel(dev, TDM_PCMTXCR, val | PCMTXCR_TXEN);
	} else {
		val = sf_tdm_readl(dev, TDM_PCMRXCR);
		sf_tdm_writel(dev, TDM_PCMRXCR, val | PCMRXCR_RXEN);
	}
}

static void sf_tdm_stop(struct sf_tdm_dev *dev, struct snd_pcm_substream *substream)
{
	unsigned int val;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		val = sf_tdm_readl(dev, TDM_PCMTXCR);
		val &= ~PCMTXCR_TXEN;
		sf_tdm_writel(dev, TDM_PCMTXCR, val);
	} else {
		val = sf_tdm_readl(dev, TDM_PCMRXCR);
		val &= ~PCMRXCR_RXEN;
		sf_tdm_writel(dev, TDM_PCMRXCR, val);
	}
}

static int sf_tdm_syncdiv(struct sf_tdm_dev *dev)
{
	u32 sl, sscale, syncdiv;

	sl = (dev->rx.sl >= dev->tx.sl) ? dev->rx.sl:dev->tx.sl;
	sscale = (dev->rx.sscale >= dev->tx.sscale) ? dev->rx.sscale:dev->tx.sscale;
	syncdiv = dev->pcmclk / dev->samplerate - 1;

	if ((syncdiv + 1) < (sl * sscale)) {
		pr_info("set syncdiv failed !\n");
		return -1;
	}

	if ((dev->syncm == TDM_SYNCM_LONG) &&
			((dev->rx.sscale <= 1) || (dev->tx.sscale <= 1))) {
		if ((syncdiv + 1) <= sl) {
			pr_info("set syncdiv failed! it must be (syncdiv+1) > max[tx.sl, rx.sl]\n");
			return -1;
		}
	}

	sf_tdm_writel(dev, TDM_PCMDIV, syncdiv);
	return 0;
}

static void sf_tdm_contrl(struct sf_tdm_dev *dev)
{
	u32 data;

	data = (dev->clkpolity << 5) | (dev->elm << 3) | (dev->syncm << 2) | (dev->ms_mode << 1);
	sf_tdm_writel(dev, TDM_PCMGBCR, data);
}

static void sf_tdm_config(struct sf_tdm_dev *dev, struct snd_pcm_substream *substream)
{
	u32 datarx, datatx;

	sf_tdm_contrl(dev);
	sf_tdm_syncdiv(dev);

	datarx = (dev->rx.ifl << 11) | (dev->rx.wl << 8) | (dev->rx.sscale << 4) |
		(dev->rx.sl << 2) | (dev->rx.lrj << 1);

	datatx = (dev->tx.ifl << 11) | (dev->tx.wl << 8) | (dev->tx.sscale << 4) |
		(dev->tx.sl << 2) | (dev->tx.lrj << 1);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		sf_tdm_writel(dev, TDM_PCMTXCR, datatx);
	else
		sf_tdm_writel(dev, TDM_PCMRXCR, datarx);
}


#define sf_tdm_suspend	NULL
#define sf_tdm_resume	NULL

/* 
 * To stop dma first, we must implement this function, because it is
 * called before stopping the stream. 
 */
static int sf_pcm_trigger(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;
	struct dma_chan *chan = snd_dmaengine_pcm_get_chan(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		axi_dma_cyclic_stop(chan);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static const struct snd_soc_component_driver sf_tdm_component = {
	.name		= "jh7110-tdm",
	.suspend	= sf_tdm_suspend,
	.resume		= sf_tdm_resume,
	.trigger	= sf_pcm_trigger,
};

static int sf_tdm_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct sf_tdm_dev *dev = snd_soc_dai_get_drvdata(dai);
	int chan_wl, chan_sl, chan_nr;
	unsigned int data_width;
	unsigned int mclk_rate;
	unsigned int dma_bus_width;
	int channels;
	int ret;
	struct snd_dmaengine_dai_dma_data *dma_data = NULL;
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai_link *dai_link = rtd->dai_link;

	dai_link->stop_dma_first = 1;

	channels = params_channels(params);
	data_width = params_width(params);

	dev->samplerate = params_rate(params);
	switch (dev->samplerate) {
	/*  There are some limitation when using 8k sample rate  */
	case 8000:
		mclk_rate = 12288000;
		dev->pcmclk = 512000;
		if ((data_width == 16) || (channels == 1)) {
			pr_err("TDM: not support 16bit or 1-channel when using 8k sample rate\n");
			return -EINVAL;
		}
		break;
	case 11025:
		mclk_rate = 11289600; //sysclk
		dev->pcmclk = 352800; //bit clock, for 16-bit
		break;
	case 16000:
		mclk_rate = 12288000; //sysclk
		dev->pcmclk = 512000; //bit clock
		break;
	case 22050:
		mclk_rate = 11289600;
		dev->pcmclk = 705600;
		break;
	case 32000:
		mclk_rate = 12288000;
		dev->pcmclk = 1024000;
		break;
	case 44100:
		mclk_rate = 11289600;
		dev->pcmclk = 1411200;
		break;
	case 48000:
		mclk_rate = 12288000;
		dev->pcmclk = 1536000;
		break;
	default:
		pr_err("TDM: not support sample rate:%d\n", dev->samplerate);
		return -EINVAL;
	}

	dev->pcmclk = channels * dev->samplerate * data_width;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		chan_wl = TDM_16BIT_WORD_LEN;
		chan_sl = TDM_16BIT_SLOT_LEN;
		dma_bus_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		break;

	case SNDRV_PCM_FORMAT_S32_LE:
		chan_wl = TDM_32BIT_WORD_LEN;
		chan_sl = TDM_32BIT_SLOT_LEN;
		dma_bus_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;

	default:
		dev_err(dev->dev, "tdm: unsupported PCM fmt");
		return -EINVAL;
	}

	chan_nr = params_channels(params);
	switch (chan_nr) {
	case ONE_CHANNEL_SUPPORT:
	case TWO_CHANNEL_SUPPORT:
	case FOUR_CHANNEL_SUPPORT:
	case SIX_CHANNEL_SUPPORT:
	case EIGHT_CHANNEL_SUPPORT:
		break;
	default:
		dev_err(dev->dev, "channel not supported\n");
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dev->tx.wl = chan_wl;
		dev->tx.sl = chan_sl;
		dev->tx.sscale = chan_nr;
		dev->play_dma_data.addr_width = dma_bus_width;
		dma_data = &dev->play_dma_data;
	} else {
		dev->rx.wl = chan_wl;
		dev->rx.sl = chan_sl;
		dev->rx.sscale = chan_nr;
		dev->capture_dma_data.addr_width = dma_bus_width;
		dma_data = &dev->capture_dma_data;
	}

	snd_soc_dai_set_dma_data(dai, substream, dma_data);

	ret = clk_set_rate(dev->clk_mclk_inner, mclk_rate);
	if (ret) {
		dev_info(dev->dev, "Can't set clk_mclk: %d\n", ret);
		return ret;
	}

	ret = clk_set_rate(dev->clk_tdm_internal, dev->pcmclk);
	if (ret) {
		dev_info(dev->dev, "Can't set clk_tdm_internal: %d\n", ret);
		return ret;
	}

	ret = clk_set_parent(dev->clk_tdm, dev->clk_tdm_ext);
	if (ret) {
		dev_info(dev->dev, "Can't set clock source for clk_tdm: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(dev->clk_tdm_ahb);
	if (ret) {
		dev_err(dev->dev, "Failed to prepare enable clk_tdm_ahb\n");
		return ret;
	}

	ret = clk_prepare_enable(dev->clk_tdm_apb);
	if (ret) {
		dev_err(dev->dev, "Failed to prepare enable clk_tdm_apb\n");
		return ret;
	}

	sf_tdm_config(dev, substream);

	return 0;
}

static int sf_tdm_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_dai *dai)
{
	struct sf_tdm_dev *dev = snd_soc_dai_get_drvdata(dai);
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		dev->active++;
		sf_tdm_start(dev, substream);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		dev->active--;
		sf_tdm_stop(dev, substream);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int sf_tdm_set_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	struct sf_tdm_dev *dev = snd_soc_dai_get_drvdata(cpu_dai);
	int ret = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		dev->ms_mode = TDM_AS_SLAVE;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		dev->ms_mode = TDM_AS_MASTER;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
	case SND_SOC_DAIFMT_CBS_CFM:
		ret = -EINVAL;
		break;
	default:
		dev_dbg(dev->dev, "tdm : Invalid master/slave format\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static const struct snd_soc_dai_ops sf_tdm_dai_ops = {
	.hw_params	= sf_tdm_hw_params,
	.trigger	= sf_tdm_trigger,
	.set_fmt	= sf_tdm_set_fmt,
};

static int sf_tdm_dai_probe(struct snd_soc_dai *dai)
{
	struct sf_tdm_dev *dev = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_init_dma_data(dai, &dev->play_dma_data, &dev->capture_dma_data);
	snd_soc_dai_set_drvdata(dai, dev);
	return 0;
}

#define SF_TDM_RATES SNDRV_PCM_RATE_8000_48000

#define SF_TDM_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
			SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver sf_tdm_dai = {
	.name = "sf_tdm",
	.id = 0,
	.playback = {
		.stream_name    = "Playback",
		.channels_min   = 1,
		.channels_max   = 8,
		.rates          = SF_TDM_RATES,
		.formats        = SF_TDM_FORMATS,
	},
	.capture = {
		.stream_name    = "Capture",
		.channels_min   = 1,
		.channels_max   = 8,
		.rates          = SF_TDM_RATES,
		.formats        = SF_TDM_FORMATS,
	},
	.ops = &sf_tdm_dai_ops,
	.probe = sf_tdm_dai_probe,
	.symmetric_rate = 1,
};

static const struct snd_pcm_hardware jh71xx_pcm_hardware = {
	.info			= (SNDRV_PCM_INFO_MMAP		|
				   SNDRV_PCM_INFO_MMAP_VALID	|
				   SNDRV_PCM_INFO_INTERLEAVED	|
				   SNDRV_PCM_INFO_BLOCK_TRANSFER),
	.buffer_bytes_max	= 192512,
	.period_bytes_min	= 4096,
	.period_bytes_max	= 32768,
	.periods_min		= 1,
	.periods_max		= 48,
	.fifo_size		= 16,
};

static const struct snd_dmaengine_pcm_config jh71xx_dmaengine_pcm_config = {
	.pcm_hardware = &jh71xx_pcm_hardware,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	.prealloc_buffer_size = 192512,
};

static void tdm_init_params(struct sf_tdm_dev *dev)
{
	dev->clkpolity = TDM_TX_RASING_RX_FALLING;
	if (dev->frame_mode == SHORT_LATER) {
		dev->elm = TDM_ELM_LATE;
		dev->syncm = TDM_SYNCM_SHORT;
	} else if (dev->frame_mode == SHORT_EARLY) {
		dev->elm = TDM_ELM_EARLY;
		dev->syncm = TDM_SYNCM_SHORT;
	} else {
		dev->elm = TDM_ELM_EARLY;
		dev->syncm = TDM_SYNCM_LONG;
	}

	dev->ms_mode = TDM_AS_SLAVE;
	dev->rx.ifl = dev->tx.ifl = TDM_FIFO_HALF;
	dev->rx.wl = dev->tx.wl = TDM_16BIT_WORD_LEN;
	dev->rx.sscale = dev->tx.sscale = 2;
	dev->rx.lrj = dev->tx.lrj = TDM_LEFT_JUSTIFT;

	dev->play_dma_data.addr = TDM_FIFO;
	dev->play_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	dev->play_dma_data.fifo_size = TDM_FIFO_DEPTH/2;
	dev->play_dma_data.maxburst = 16;

	dev->capture_dma_data.addr = TDM_FIFO;
	dev->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	dev->capture_dma_data.fifo_size = TDM_FIFO_DEPTH/2;
	dev->capture_dma_data.maxburst = 8;
}

static int sf_tdm_clk_reset_init(struct platform_device *pdev, struct sf_tdm_dev *dev)
{
	int ret;

	static struct clk_bulk_data clks[] = {
		{ .id = "clk_ahb0" },
		{ .id = "clk_tdm_ahb" },
		{ .id = "clk_apb0" },
		{ .id = "clk_tdm_apb" },
		{ .id = "clk_tdm_internal" },
		{ .id = "clk_tdm_ext" },
		{ .id = "clk_tdm" },
		{ .id = "mclk_inner" },
	};

	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(clks), clks);
	if (ret) {
		dev_err(&pdev->dev, "failed to get tdm clocks\n");
		goto exit;
	}

	dev->clk_ahb0 = clks[0].clk;
	dev->clk_tdm_ahb = clks[1].clk;
	dev->clk_apb0 = clks[2].clk;
	dev->clk_tdm_apb = clks[3].clk;
	dev->clk_tdm_internal = clks[4].clk;
	dev->clk_tdm_ext = clks[5].clk;
	dev->clk_tdm = clks[6].clk;
	dev->clk_mclk_inner = clks[7].clk;

	dev->resets = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(dev->resets)) {
		ret = PTR_ERR(dev->resets);
		dev_err(&pdev->dev, "Failed to get tdm resets");
		goto exit;
	}

	ret = reset_control_assert(dev->resets);
	if (ret) {
		dev_err(&pdev->dev, "Failed to assert tdm resets\n");
		goto exit;
	}

	ret = clk_prepare_enable(dev->clk_mclk_inner);
	if (ret) {
		dev_err(&pdev->dev, "failed to prepare enable clk_mclk_inner\n");
		goto exit;
	}

	ret = clk_prepare_enable(dev->clk_ahb0);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare enable clk_ahb0\n");
		goto err_dis_ahb0;
	}

	ret = clk_prepare_enable(dev->clk_tdm_ahb);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare enable clk_tdm_ahb\n");
		goto err_dis_tdm_ahb;
	}

	ret = clk_prepare_enable(dev->clk_apb0);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare enable clk_apb0\n");
		goto err_dis_apb0;
	}

	ret = clk_prepare_enable(dev->clk_tdm_apb);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare enable clk_tdm_apb\n");
		goto err_dis_tdm_apb;
	}

	ret = clk_prepare_enable(dev->clk_tdm_internal);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare enable clk_tdm_intl\n");
		goto err_dis_tdm_internal;
	}

	ret = clk_prepare_enable(dev->clk_tdm_ext);
	if (ret) {
		dev_err(&pdev->dev, "failed to prepare enable clk_tdm_ext\n");
		goto err_dis_tdm_ext;
	}

	ret = reset_control_deassert(dev->resets);
	if (ret) {
		dev_err(&pdev->dev, "Failed to deassert tdm resets\n");
		goto err_clk_disable;
	}

	return 0;

err_clk_disable:
	clk_disable_unprepare(dev->clk_tdm_ext);
err_dis_tdm_ext:
	clk_disable_unprepare(dev->clk_tdm_internal);
err_dis_tdm_internal:
	clk_disable_unprepare(dev->clk_tdm_apb);
err_dis_tdm_apb:
	clk_disable_unprepare(dev->clk_apb0);
err_dis_apb0:
	clk_disable_unprepare(dev->clk_tdm_ahb);
err_dis_tdm_ahb:
	clk_disable_unprepare(dev->clk_ahb0);
err_dis_ahb0:
	clk_disable_unprepare(dev->clk_mclk_inner);
exit:
	return ret;
}

static int sf_tdm_probe(struct platform_device *pdev)
{
	struct sf_tdm_dev *dev;
	struct resource *res;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dev->tdm_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dev->tdm_base))
		return PTR_ERR(dev->tdm_base);

	dev->dev = &pdev->dev;

	ret = sf_tdm_clk_reset_init(pdev, dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable audio-tdm clock\n");
		return ret;
	}

	dev->frame_mode = SHORT_LATER;
	tdm_init_params(dev);

	dev_set_drvdata(&pdev->dev, dev);
	ret = devm_snd_soc_register_component(&pdev->dev, &sf_tdm_component,
					 &sf_tdm_dai, 1);
	if (ret != 0) {
		dev_err(&pdev->dev, "failed to register dai\n");
		return ret;
	}

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev,
					&jh71xx_dmaengine_pcm_config,
					SND_DMAENGINE_PCM_FLAG_COMPAT);
	if (ret) {
		dev_err(&pdev->dev, "could not register pcm: %d\n", ret);
		return ret;
	}

	return 0;
}

static int sf_tdm_dev_remove(struct platform_device *pdev)
{
	return 0;
}
static const struct of_device_id sf_tdm_of_match[] = {
	{.compatible = "starfive,jh7110-tdm",},
	{}
};
MODULE_DEVICE_TABLE(of, sf_tdm_of_match);

static struct platform_driver sf_tdm_driver = {
	.driver = {
		.name = "jh7110-tdm",
		.of_match_table = sf_tdm_of_match,
	},
	.probe = sf_tdm_probe,
	.remove = sf_tdm_dev_remove,
};
module_platform_driver(sf_tdm_driver);

MODULE_AUTHOR("Walker Chen <walker.chen@starfivetech.com>");
MODULE_DESCRIPTION("Starfive TDM Controller Driver");
MODULE_LICENSE("GPL v2");
