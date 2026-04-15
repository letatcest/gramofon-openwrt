// SPDX-License-Identifier: GPL-2.0-only
/*
 * ath79-i2s-drv.c — AR9341 I2S / STEREO ASoC driver
 *
 * Copyright (c) 2012-2013 Qualcomm Atheros, Inc.  (original)
 * Copyright (c) 2024 Krijn Soeteman  (kernel 5.15/6.6 port, DTS probing)
 *
 * Combines the old ath79-i2s.c (DAI) and ath79-pcm.c (platform) into a
 * single snd_soc_component_driver, as required by Linux 5.4+.
 *
 * GPIO mux is set directly by writing the AR9341 GPIO OUT FUNCTION
 * registers; pinctrl does not support the multi-bit I2S mux values.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "ath79-i2s.h"

#define DRV_NAME	"ath79-i2s"

/* Empirical limits — same as original Qualcomm driver */
#define BUFFER_BYTES_MAX	(16 * 4095 * 16)
#define PERIOD_BYTES_MIN	64
#define PERIOD_BYTES_MAX	4095

/* Module-level device pointer (one I2S controller per SoC) */
static struct ath79_i2s_dev *ath79_i2s_dev_g;

/* ── Stereo block helpers ───────────────────────────────────────────── */

void ath79_stereo_reset(struct ath79_i2s_dev *adev)
{
	u32 t;

	spin_lock(&adev->stereo_lock);
	t = stereo_rr(adev, AR934X_STEREO_REG_CONFIG);
	t |= AR934X_STEREO_CONFIG_RESET;
	stereo_wr(adev, AR934X_STEREO_REG_CONFIG, t);
	udelay(10);
	t &= ~AR934X_STEREO_CONFIG_RESET;
	stereo_wr(adev, AR934X_STEREO_REG_CONFIG, t);
	spin_unlock(&adev->stereo_lock);
}

/* ── GPIO mux setup ─────────────────────────────────────────────────── */

/*
 * Each AR9341 GPIO OUT FUNCTION register covers four GPIOs; each GPIO
 * occupies an 8-bit field.  We write the I2S mux value (12-15) into
 * the appropriate field.
 *
 * AR934X_GPIO_OUT_FUNCTION0 is at offset 0x2c from gpio_base (0x18040000),
 * i.e. the same as AR934X_GPIO_OUT_FUNCTION[n/4] = 0x2c + (n/4)*4.
 */
static void gpio_set_i2s_mux(struct ath79_i2s_dev *adev,
			     u32 gpio, u8 mux_val)
{
	u32 reg_off = AR934X_GPIO_OUT_FUNCTION0 + (gpio / 4) * 4;
	u32 shift   = (gpio % 4) * 8;
	u32 t;

	t = __raw_readl(adev->gpio_base + reg_off);
	t &= ~(0xffu << shift);
	t |= ((u32)mux_val << shift);
	__raw_writel(t, adev->gpio_base + reg_off);
}

static void ath79_i2s_gpio_mux_setup(struct ath79_i2s_dev *adev)
{
	gpio_set_i2s_mux(adev, adev->gpio_mclk, AR934X_GPIO_OUT_MUX_I2S_MCK);
	gpio_set_i2s_mux(adev, adev->gpio_bick, AR934X_GPIO_OUT_MUX_I2S_CLK);
	gpio_set_i2s_mux(adev, adev->gpio_lrck, AR934X_GPIO_OUT_MUX_I2S_WS);
	gpio_set_i2s_mux(adev, adev->gpio_sdto, AR934X_GPIO_OUT_MUX_I2S_SD);
}

/* ── DAI ops ────────────────────────────────────────────────────────── */

static int ath79_i2s_startup(struct snd_pcm_substream *ss,
			     struct snd_soc_dai *dai)
{
	struct ath79_i2s_dev *adev = snd_soc_dai_get_drvdata(dai);

	if (!snd_soc_dai_active(dai)) {
		stereo_wr(adev, AR934X_STEREO_REG_CONFIG,
			  AR934X_STEREO_CONFIG_SPDIF_ENABLE |
			  AR934X_STEREO_CONFIG_I2S_ENABLE   |
			  AR934X_STEREO_CONFIG_SAMPLE_CNT_CLEAR_TYPE |
			  AR934X_STEREO_CONFIG_MASTER);
		ath79_stereo_reset(adev);
	}
	return 0;
}

static void ath79_i2s_shutdown(struct snd_pcm_substream *ss,
				struct snd_soc_dai *dai)
{
	struct ath79_i2s_dev *adev = snd_soc_dai_get_drvdata(dai);

	if (!snd_soc_dai_active(dai))
		stereo_wr(adev, AR934X_STEREO_REG_CONFIG, 0);
}

static int ath79_i2s_hw_params(struct snd_pcm_substream *ss,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct ath79_i2s_dev *adev = snd_soc_dai_get_drvdata(dai);
	u32 mask = 0, t;

	ath79_audio_set_freq(adev, params_rate(params));

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S8:
		mask |= AR934X_STEREO_CONFIG_DATA_WORD_8
			<< AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		mask |= AR934X_STEREO_CONFIG_PCM_SWAP;
		fallthrough;
	case SNDRV_PCM_FORMAT_S16_BE:
		mask |= AR934X_STEREO_CONFIG_DATA_WORD_16
			<< AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		mask |= AR934X_STEREO_CONFIG_PCM_SWAP;
		fallthrough;
	case SNDRV_PCM_FORMAT_S24_BE:
		mask |= AR934X_STEREO_CONFIG_DATA_WORD_24
			<< AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		mask |= AR934X_STEREO_CONFIG_PCM_SWAP;
		fallthrough;
	case SNDRV_PCM_FORMAT_S32_BE:
		mask |= AR934X_STEREO_CONFIG_DATA_WORD_32
			<< AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
		break;
	default:
		dev_err(adev->dev, "unsupported PCM format %d\n",
			params_format(params));
		return -EINVAL;
	}

	spin_lock(&adev->stereo_lock);
	t = stereo_rr(adev, AR934X_STEREO_REG_CONFIG);
	t &= ~(AR934X_STEREO_CONFIG_DATA_WORD_SIZE_MASK
	       << AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT);
	t &= ~AR934X_STEREO_CONFIG_PCM_SWAP;
	t |= mask;
	stereo_wr(adev, AR934X_STEREO_REG_CONFIG, t);
	spin_unlock(&adev->stereo_lock);

	return 0;
}

static int ath79_i2s_trigger(struct snd_pcm_substream *ss, int cmd,
			     struct snd_soc_dai *dai)
{
	return 0;
}

static const struct snd_soc_dai_ops ath79_i2s_dai_ops = {
	.startup   = ath79_i2s_startup,
	.shutdown  = ath79_i2s_shutdown,
	.hw_params = ath79_i2s_hw_params,
	.trigger   = ath79_i2s_trigger,
};

static struct snd_soc_dai_driver ath79_i2s_dai = {
	.name    = "ath79-i2s",
	.playback = {
		.stream_name  = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates        = SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
				SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
				SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
		.formats      = SNDRV_PCM_FMTBIT_S8    |
				SNDRV_PCM_FMTBIT_S16_BE | SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_BE | SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_BE | SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &ath79_i2s_dai_ops,
};

/* ── PCM hardware constraints ────────────────────────────────────────── */

static const struct snd_pcm_hardware ath79_pcm_hardware = {
	.info           = SNDRV_PCM_INFO_MMAP        |
			  SNDRV_PCM_INFO_MMAP_VALID  |
			  SNDRV_PCM_INFO_INTERLEAVED |
			  SNDRV_PCM_INFO_NO_PERIOD_WAKEUP,
	.formats        = SNDRV_PCM_FMTBIT_S8    |
			  SNDRV_PCM_FMTBIT_S16_BE | SNDRV_PCM_FMTBIT_S16_LE |
			  SNDRV_PCM_FMTBIT_S24_BE | SNDRV_PCM_FMTBIT_S24_LE |
			  SNDRV_PCM_FMTBIT_S32_BE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates          = SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
			  SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
			  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
	.rate_min       = 22050,
	.rate_max       = 96000,
	.channels_min   = 2,
	.channels_max   = 2,
	.buffer_bytes_max = BUFFER_BYTES_MAX,
	.period_bytes_min = PERIOD_BYTES_MIN,
	.period_bytes_max = PERIOD_BYTES_MAX,
	.periods_min    = 16,
	.periods_max    = 256,
	.fifo_size      = 0,
};

/* ── DMA interrupt handler ───────────────────────────────────────────── */

static irqreturn_t ath79_pcm_interrupt(int irq, void *dev_id)
{
	struct ath79_i2s_dev *adev = dev_id;
	struct ath79_pcm_pltfm_priv *prdata = &adev->pcm_priv;
	struct ath79_pcm_rt_priv *rtpriv;
	u32 status;
	unsigned int period_bytes, played_size;

	status = dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS);

	if (status & AR934X_DMA_MBOX_INT_STATUS_TX_DMA_COMPLETE) {
		/* TX = memory→I2S = playback */
		ath79_mbox_interrupt_ack(adev,
					 AR934X_DMA_MBOX_INT_STATUS_TX_DMA_COMPLETE);
		if (prdata->playback) {
			rtpriv = prdata->playback->runtime->private_data;
			rtpriv->last_played = ath79_pcm_get_last_played(rtpriv);
			period_bytes = snd_pcm_lib_period_bytes(prdata->playback);
			played_size  = ath79_pcm_set_own_bits(rtpriv);
			rtpriv->elapsed_size += played_size;
			if (rtpriv->elapsed_size >= period_bytes) {
				rtpriv->elapsed_size %= period_bytes;
				snd_pcm_period_elapsed(prdata->playback);
			}
		}
	}

	if (status & AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE) {
		/* RX = I2S→memory = capture */
		ath79_mbox_interrupt_ack(adev,
					 AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE);
		if (prdata->capture) {
			rtpriv = prdata->capture->runtime->private_data;
			rtpriv->last_played = ath79_pcm_get_last_played(rtpriv);
			ath79_pcm_set_own_bits(rtpriv);
			snd_pcm_period_elapsed(prdata->capture);
		}
	}

	return IRQ_HANDLED;
}

/* ── Component PCM ops ───────────────────────────────────────────────── */

static int ath79_pcm_open(struct snd_soc_component *component,
			  struct snd_pcm_substream *ss)
{
	struct ath79_pcm_pltfm_priv *prdata = &ath79_i2s_dev_g->pcm_priv;
	struct ath79_pcm_rt_priv *rtpriv;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		prdata->playback = ss;
	else
		prdata->capture = ss;

	rtpriv = kzalloc(sizeof(*rtpriv), GFP_KERNEL);
	if (!rtpriv)
		return -ENOMEM;

	ss->runtime->private_data = rtpriv;
	INIT_LIST_HEAD(&rtpriv->dma_head);
	rtpriv->direction = ss->stream;

	snd_soc_set_runtime_hwparams(ss, &ath79_pcm_hardware);
	return 0;
}

static int ath79_pcm_close(struct snd_soc_component *component,
			   struct snd_pcm_substream *ss)
{
	struct ath79_pcm_pltfm_priv *prdata = &ath79_i2s_dev_g->pcm_priv;
	struct ath79_pcm_rt_priv *rtpriv;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		prdata->playback = NULL;
	else
		prdata->capture = NULL;

	rtpriv = ss->runtime->private_data;
	kfree(rtpriv);
	ss->runtime->private_data = NULL;

	return 0;
}

static int ath79_pcm_hw_params(struct snd_soc_component *component,
			       struct snd_pcm_substream *ss,
			       struct snd_pcm_hw_params *hw_params)
{
	struct ath79_pcm_rt_priv *rtpriv = ss->runtime->private_data;
	unsigned int period_size, sample_size, sample_rate, channels, frames;
	int ret;

	ret = ath79_mbox_dma_map(rtpriv, ss->dma_buffer.addr,
				 params_period_bytes(hw_params),
				 params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;

	period_size = params_period_bytes(hw_params);
	sample_size = snd_pcm_format_size(params_format(hw_params), 1);
	sample_rate = params_rate(hw_params);
	channels    = params_channels(hw_params);
	frames      = period_size / (sample_size * channels);
	/* Worst-case DMA stop time: one period + 10 ms margin */
	rtpriv->delay_time = (frames * 1000) / sample_rate + 10;

	snd_pcm_set_runtime_buffer(ss, &ss->dma_buffer);
	ss->runtime->dma_bytes = params_buffer_bytes(hw_params);
	return 0;
}

static int ath79_pcm_hw_free(struct snd_soc_component *component,
			     struct snd_pcm_substream *ss)
{
	struct ath79_pcm_rt_priv *rtpriv = ss->runtime->private_data;

	ath79_mbox_dma_unmap(rtpriv);
	snd_pcm_set_runtime_buffer(ss, NULL);
	return 0;
}

static int ath79_pcm_prepare(struct snd_soc_component *component,
			     struct snd_pcm_substream *ss)
{
	struct ath79_i2s_dev *adev = ath79_i2s_dev_g;
	struct ath79_pcm_rt_priv *rtpriv = ss->runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(ss);

	if (snd_soc_dai_active(asoc_rtd_to_cpu(rtd, 0)) == 1)
		ath79_mbox_dma_reset(adev);

	ath79_mbox_dma_prepare(adev, rtpriv);
	ath79_pcm_set_own_bits(rtpriv);
	rtpriv->last_played = NULL;
	return 0;
}

static int ath79_pcm_trigger(struct snd_soc_component *component,
			     struct snd_pcm_substream *ss, int cmd)
{
	struct ath79_i2s_dev *adev = ath79_i2s_dev_g;
	struct ath79_pcm_rt_priv *rtpriv = ss->runtime->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		ath79_mbox_dma_start(adev, rtpriv);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		ath79_mbox_dma_stop(adev, rtpriv);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t ath79_pcm_pointer(struct snd_soc_component *component,
					   struct snd_pcm_substream *ss)
{
	struct ath79_pcm_rt_priv *rtpriv = ss->runtime->private_data;

	if (!rtpriv->last_played)
		return 0;

	return bytes_to_frames(ss->runtime,
			       rtpriv->last_played->BufPtr -
			       ss->runtime->dma_addr);
}

static int ath79_pcm_mmap(struct snd_soc_component *component,
			  struct snd_pcm_substream *ss,
			  struct vm_area_struct *vma)
{
	return remap_pfn_range(vma, vma->vm_start,
			       ss->dma_buffer.addr >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

/* ── Buffer allocation / deallocation ───────────────────────────────── */

static u64 ath79_pcm_dmamask = DMA_BIT_MASK(32);

static int ath79_pcm_construct(struct snd_soc_component *component,
			       struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm  = rtd->pcm;
	struct snd_pcm_substream *ss;
	struct snd_dma_buffer *buf;
	int stream, ret;

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &ath79_pcm_dmamask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	for (stream = 0; stream < 2; stream++) {
		ss = pcm->streams[stream].substream;
		if (!ss)
			continue;
		buf = &ss->dma_buffer;
		buf->dev.type     = SNDRV_DMA_TYPE_DEV;
		buf->dev.dev      = card->dev;
		buf->private_data = NULL;
		buf->bytes        = ath79_pcm_hardware.buffer_bytes_max;
		buf->area = dma_alloc_coherent(card->dev, buf->bytes,
					       &buf->addr, GFP_DMA);
		if (!buf->area) {
			dev_err(card->dev, "cannot allocate DMA buffer\n");
			ret = -ENOMEM;
			goto err;
		}
	}

	return ath79_mbox_dma_init(card->dev);
err:
	return ret;
}

static void ath79_pcm_destruct(struct snd_soc_component *component,
			       struct snd_pcm *pcm)
{
	struct snd_pcm_substream *ss;
	struct snd_dma_buffer *buf;
	int stream;

	for (stream = 0; stream < 2; stream++) {
		ss = pcm->streams[stream].substream;
		if (!ss)
			continue;
		buf = &ss->dma_buffer;
		if (!buf->area)
			continue;
		dma_free_coherent(buf->dev.dev, buf->bytes,
				  buf->area, buf->addr);
		buf->area = NULL;
	}

	ath79_mbox_dma_exit();
}

/* ── Component driver ────────────────────────────────────────────────── */

static const struct snd_soc_component_driver ath79_i2s_component = {
	.name          = DRV_NAME,
	.open          = ath79_pcm_open,
	.close         = ath79_pcm_close,
	.hw_params     = ath79_pcm_hw_params,
	.hw_free       = ath79_pcm_hw_free,
	.prepare       = ath79_pcm_prepare,
	.trigger       = ath79_pcm_trigger,
	.pointer       = ath79_pcm_pointer,
	.mmap          = ath79_pcm_mmap,
	.pcm_construct = ath79_pcm_construct,
	.pcm_destruct  = ath79_pcm_destruct,
};

/* ── Platform driver ─────────────────────────────────────────────────── */

/*
 * AR9341 base addresses — SoC-fixed, used for regions shared with other drivers
 * (we use devm_ioremap without resource claiming to avoid conflicts with the
 * gpio, pinmux, and pll syscon nodes that own these memory regions in DTS).
 */
#define AR9341_GPIO_BASE	0x18040000UL
#define AR9341_PLL_BASE		0x18050000UL

static int ath79_i2s_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct ath79_i2s_dev *adev;
	struct reset_control *rst_i2s, *rst_mbox;
	int ret;

	adev = devm_kzalloc(dev, sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->dev = dev;
	spin_lock_init(&adev->stereo_lock);
	spin_lock_init(&adev->pll_lock);

	/*
	 * Unique regions — claim via platform resource (exclusive).
	 * These are not used by any other DTS node.
	 */
	adev->stereo_base = devm_platform_ioremap_resource_byname(pdev, "stereo");
	if (IS_ERR(adev->stereo_base))
		return PTR_ERR(adev->stereo_base);

	adev->dma_base = devm_platform_ioremap_resource_byname(pdev, "dma");
	if (IS_ERR(adev->dma_base))
		return PTR_ERR(adev->dma_base);

	adev->dpll_base = devm_platform_ioremap_resource_byname(pdev, "dpll");
	if (IS_ERR(adev->dpll_base))
		return PTR_ERR(adev->dpll_base);

	/*
	 * Shared regions — use devm_ioremap (no resource claiming) to avoid
	 * conflicts with the gpio/pinmux and pll syscon nodes in DTS.
	 *
	 * pll_base: 0x18050000 — audio PLL regs at +0x30/+0x34 (shared with
	 *   the "pll" clock syscon which owns the full 0x18050000-0x1805004b
	 *   range, but only uses regs 0x00-0x28 for CPU/DDR clocks).
	 *
	 * gpio_base: 0x18040000 — GPIO OUT FUNCTION regs at +0x2c-+0x40
	 *   (claimed by "gpio" and "pinmux" DTS nodes; we only write the
	 *   I2S output function bits which pinctrl-single doesn't manage).
	 */
	adev->pll_base = devm_ioremap(dev, AR9341_PLL_BASE, 0x40);
	if (!adev->pll_base) {
		dev_err(dev, "cannot map PLL registers\n");
		return -ENOMEM;
	}

	adev->gpio_base = devm_ioremap(dev, AR9341_GPIO_BASE, 0x48);
	if (!adev->gpio_base) {
		dev_err(dev, "cannot map GPIO registers\n");
		return -ENOMEM;
	}

	/* IRQ */
	adev->irq = platform_get_irq(pdev, 0);
	if (adev->irq < 0)
		return adev->irq;

	/* Reference clock — needed to select 25 MHz vs 40 MHz PLL tables */
	adev->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(adev->ref_clk)) {
		dev_err(dev, "cannot get ref clock\n");
		return PTR_ERR(adev->ref_clk);
	}

	/* GPIO pin assignments from DTS */
	ret = of_property_read_u32(np, "qca,i2s-mclk-gpio",
				   &adev->gpio_mclk);
	if (ret) { dev_err(dev, "missing qca,i2s-mclk-gpio\n"); return ret; }
	ret = of_property_read_u32(np, "qca,i2s-bick-gpio",
				   &adev->gpio_bick);
	if (ret) { dev_err(dev, "missing qca,i2s-bick-gpio\n"); return ret; }
	ret = of_property_read_u32(np, "qca,i2s-lrck-gpio",
				   &adev->gpio_lrck);
	if (ret) { dev_err(dev, "missing qca,i2s-lrck-gpio\n"); return ret; }
	ret = of_property_read_u32(np, "qca,i2s-sdto-gpio",
				   &adev->gpio_sdto);
	if (ret) { dev_err(dev, "missing qca,i2s-sdto-gpio\n"); return ret; }

	/* Reset I2S and MBOX controllers via the reset framework */
	rst_i2s = devm_reset_control_get_exclusive(dev, "i2s");
	if (!IS_ERR(rst_i2s))
		reset_control_reset(rst_i2s);

	rst_mbox = devm_reset_control_get_exclusive(dev, "mbox");
	if (!IS_ERR(rst_mbox))
		reset_control_reset(rst_mbox);

	/* Route I2S signals to the GPIO pins via the output function mux */
	ath79_i2s_gpio_mux_setup(adev);

	/* DMA interrupt — handled in our ISR, acked at DMA level */
	ret = devm_request_irq(dev, adev->irq, ath79_pcm_interrupt, 0,
			       DRV_NAME, adev);
	if (ret) {
		dev_err(dev, "cannot request IRQ %d\n", adev->irq);
		return ret;
	}

	platform_set_drvdata(pdev, adev);
	ath79_i2s_dev_g = adev;

	ret = devm_snd_soc_register_component(dev, &ath79_i2s_component,
					      &ath79_i2s_dai, 1);
	if (ret) {
		dev_err(dev, "cannot register ASoC component\n");
		return ret;
	}

	dev_info(dev,
		 "AR9341 I2S ready (MCLK=GPIO%d BICK=GPIO%d LRCK=GPIO%d SDTO=GPIO%d)\n",
		 adev->gpio_mclk, adev->gpio_bick,
		 adev->gpio_lrck, adev->gpio_sdto);
	return 0;
}

static int ath79_i2s_remove(struct platform_device *pdev)
{
	ath79_i2s_dev_g = NULL;
	return 0;
}

static const struct of_device_id ath79_i2s_of_match[] = {
	{ .compatible = "qca,ar9341-i2s" },
	{},
};
MODULE_DEVICE_TABLE(of, ath79_i2s_of_match);

static struct platform_driver ath79_i2s_driver = {
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = ath79_i2s_of_match,
	},
	.probe  = ath79_i2s_probe,
	.remove = ath79_i2s_remove,
};
module_platform_driver(ath79_i2s_driver);

MODULE_AUTHOR("Qualcomm Atheros Inc.");
MODULE_AUTHOR("Krijn Soeteman");
MODULE_DESCRIPTION("AR9341 I2S ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
