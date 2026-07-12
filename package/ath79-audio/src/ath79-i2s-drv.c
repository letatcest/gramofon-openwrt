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
#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <asm/addrspace.h>
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
#define PERIOD_BYTES_MAX	2048

/* Module-level device pointer (one I2S controller per SoC) */
static struct ath79_i2s_dev *ath79_i2s_dev_g;

/*
 * Diagnostiek: zet kandidaat-GPIO's (mogelijk SMUTE/amp-enable van de
 * AK4430) op een vast niveau tijdens playback.  -1 = niet aanraken,
 * 0 = alle kandidaten laag, 1 = alle kandidaten hoog.
 * Schrijfbaar via /sys/module/ath79_i2s/parameters/force_gpio_level.
 */
static int force_gpio_level = -1;
module_param(force_gpio_level, int, 0644);
MODULE_PARM_DESC(force_gpio_level,
		 "Kandidaat-GPIO's forceren: -1=uit, 0=laag, 1=hoog");

/*
 * Bitmask van GPIO's die geforceerd mogen worden (bit N = GPIO N).
 * Zo kan de test per subset lopen zonder herbouwen.  GPIO's waarvan de
 * output-mux niet 0 (gewone GPIO) is worden ALTIJD overgeslagen: die
 * dragen een actieve functie (SPI-flash mux 9/10/11, I2S 12-15, ...)
 * en aanraken daarvan sloopt het rootfs of de audio-pads zelf.
 */
static uint force_gpio_mask;
module_param(force_gpio_mask, uint, 0644);
MODULE_PARM_DESC(force_gpio_mask,
		 "Bitmask van te forceren GPIO's (0 = geen)");

/*
 * Pinnen in dit mask (én in force_gpio_mask) worden ook geforceerd als
 * hun output-mux niet 0 is: de mux wordt dan eerst naar 0 (GPIO) gezet.
 * Bedoeld voor GPIO16 (mux 77 → vermoedelijk SMUTE van de AK4430).
 */
static uint force_gpio_unmux_mask;
module_param(force_gpio_unmux_mask, uint, 0644);
MODULE_PARM_DESC(force_gpio_unmux_mask,
		 "Bitmask van GPIO's waarvan de mux naar 0 mag (override guard)");

/*
 * Direct (bij schrijven) een output-mux op een GPIO zetten:
 *   echo "13 15" > /sys/module/ath79_i2s/parameters/mux_set
 * zet GPIO13 op mux 15 (I2S_MCK) en maakt hem output.  Voor het
 * opsporen van de werkelijke I2S-routing naar de AK4430.
 */
static int mux_set_set(const char *val, const struct kernel_param *kp)
{
	struct ath79_i2s_dev *adev = ath79_i2s_dev_g;
	unsigned int gpio, mux;
	u32 reg_off, shift, t, oe;

	if (!adev)
		return -ENODEV;
	if (sscanf(val, "%u %u", &gpio, &mux) != 2 || gpio >= 24 || mux > 0xff)
		return -EINVAL;
	reg_off = AR934X_GPIO_OUT_FUNCTION0 + (gpio / 4) * 4;
	shift = (gpio % 4) * 8;
	t = __raw_readl(adev->gpio_base + reg_off);
	t = (t & ~(0xffu << shift)) | (mux << shift);
	__raw_writel(t, adev->gpio_base + reg_off);
	oe = __raw_readl(adev->gpio_base + 0x00);
	oe &= ~BIT(gpio);	/* 0 = output enabled */
	__raw_writel(oe, adev->gpio_base + 0x00);
	dev_info(adev->dev, "mux_set: GPIO%u -> mux %u\n", gpio, mux);
	return 0;
}

static const struct kernel_param_ops mux_set_ops = {
	.set = mux_set_set,
};
module_param_cb(mux_set, &mux_set_ops, NULL, 0200);
MODULE_PARM_DESC(mux_set, "Schrijf 'GPIO MUX' om een output-mux direct te zetten");

/* echo 1 > .../dump_gpio logt alle GPIO-registers naar dmesg */
static int dump_gpio_set(const char *val, const struct kernel_param *kp)
{
	struct ath79_i2s_dev *adev = ath79_i2s_dev_g;
	int i;

	if (!adev)
		return -ENODEV;
	for (i = 0; i < 6; i++)
		dev_info(adev->dev, "dump: FUNC%d(GPIO%d-%d)=0x%08x\n",
			 i, i * 4, i * 4 + 3,
			 __raw_readl(adev->gpio_base +
				     AR934X_GPIO_OUT_FUNCTION0 + i * 4));
	dev_info(adev->dev, "dump: OE=0x%08x IN=0x%08x OUT=0x%08x\n",
		 __raw_readl(adev->gpio_base + 0x00),
		 __raw_readl(adev->gpio_base + 0x04),
		 __raw_readl(adev->gpio_base + 0x08));
	return 0;
}

static const struct kernel_param_ops dump_gpio_ops = {
	.set = dump_gpio_set,
};
module_param_cb(dump_gpio, &dump_gpio_ops, NULL, 0200);
MODULE_PARM_DESC(dump_gpio, "Schrijf 1 om GPIO-registers naar dmesg te loggen");

/* echo <gpio> > .../measure_freq telt flanken op GPIO_IN over een
 * ktime-venster en logt de gemeten frequentie.  Bruikbaar tot enkele
 * honderden kHz (sampling ~5-10 MHz); bedoeld om de echte LRCK (fs)
 * te verifiëren tegen de geconfigureerde samplerate. */
static int measure_freq_set(const char *val, const struct kernel_param *kp)
{
	struct ath79_i2s_dev *adev = ath79_i2s_dev_g;
	unsigned int gpio;
	u32 prev, cur, mask;
	u64 edges = 0, ns, hz;
	ktime_t t0, t1;
	int i;

	if (!adev)
		return -ENODEV;
	if (kstrtouint(val, 0, &gpio) || gpio > 23)
		return -EINVAL;

	mask = BIT(gpio);
	prev = __raw_readl(adev->gpio_base + 0x04) & mask;
	t0 = ktime_get();
	for (i = 0; i < 2000000; i++) {
		cur = __raw_readl(adev->gpio_base + 0x04) & mask;
		if (cur != prev) {
			edges++;
			prev = cur;
		}
	}
	t1 = ktime_get();
	ns = ktime_to_ns(ktime_sub(t1, t0));
	hz = ns ? div64_u64(edges * (u64)NSEC_PER_SEC, 2 * ns) : 0;
	dev_info(adev->dev,
		 "measure_freq: GPIO%u %llu flanken in %llu ns -> %llu Hz\n",
		 gpio, edges, ns, hz);
	return 0;
}

static const struct kernel_param_ops measure_freq_ops = {
	.set = measure_freq_set,
};
module_param_cb(measure_freq, &measure_freq_ops, NULL, 0200);
MODULE_PARM_DESC(measure_freq, "Schrijf GPIO-nummer om de frequentie op die pin te meten");

static void ath79_force_candidate_gpios(struct ath79_i2s_dev *adev, int level)
{
	u32 oe, out, applied = 0, skipped = 0;
	int gpio;

	for (gpio = 0; gpio < 24; gpio++) {
		u32 reg_off = AR934X_GPIO_OUT_FUNCTION0 + (gpio / 4) * 4;
		u32 shift = (gpio % 4) * 8;
		u32 mux;

		if (!(force_gpio_mask & BIT(gpio)))
			continue;

		mux = (__raw_readl(adev->gpio_base + reg_off) >> shift) & 0xff;
		if (mux != 0) {
			u32 t;

			if (!(force_gpio_unmux_mask & BIT(gpio))) {
				skipped |= BIT(gpio);
				dev_info(adev->dev,
					 "force_gpio: GPIO%d overgeslagen (mux=%u in gebruik)\n",
					 gpio, mux);
				continue;
			}
			t = __raw_readl(adev->gpio_base + reg_off);
			t &= ~(0xffu << shift);
			__raw_writel(t, adev->gpio_base + reg_off);
			dev_info(adev->dev,
				 "force_gpio: GPIO%d mux %u -> 0 (unmux-override)\n",
				 gpio, mux);
		}
		applied |= BIT(gpio);
	}

	out = __raw_readl(adev->gpio_base + 0x08);
	oe = __raw_readl(adev->gpio_base + 0x00);
	if (level)
		out |= applied;
	else
		out &= ~applied;
	oe &= ~applied;	/* 0 = output enabled */
	__raw_writel(out, adev->gpio_base + 0x08);
	__raw_writel(oe, adev->gpio_base + 0x00);

	dev_info(adev->dev,
		 "force_gpio_level=%d mask=0x%08x: applied=0x%08x skipped=0x%08x OUT=0x%08x OE=0x%08x\n",
		 level, force_gpio_mask, applied, skipped,
		 __raw_readl(adev->gpio_base + 0x08),
		 __raw_readl(adev->gpio_base + 0x00));
}

/* ── Stereo block helpers ───────────────────────────────────────────── */

void ath79_stereo_reset(struct ath79_i2s_dev *adev)
{
	u32 t;

	/* RESET-bit is self-clearing; de QCA-referentie schrijft hem alleen
	 * en wist hem niet handmatig. */
	spin_lock(&adev->stereo_lock);
	t = stereo_rr(adev, AR934X_STEREO_REG_CONFIG);
	t |= AR934X_STEREO_CONFIG_RESET;
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
	u32 oe;

	gpio_set_i2s_mux(adev, adev->gpio_mclk, AR934X_GPIO_OUT_MUX_I2S_MCK);
	gpio_set_i2s_mux(adev, adev->gpio_bick, AR934X_GPIO_OUT_MUX_I2S_CLK);
	gpio_set_i2s_mux(adev, adev->gpio_lrck, AR934X_GPIO_OUT_MUX_I2S_WS);
	gpio_set_i2s_mux(adev, adev->gpio_sdto, AR934X_GPIO_OUT_MUX_I2S_SD);

	/*
	 * AR934X GPIO_OE register (offset 0x00): bit=0 means output enabled.
	 * The peripheral mux alone is not enough — the output driver must also
	 * be enabled or the I2S signals never leave the SoC.
	 */
	oe = __raw_readl(adev->gpio_base + 0x00);
	oe &= ~(BIT(adev->gpio_mclk) | BIT(adev->gpio_bick) |
		BIT(adev->gpio_lrck) | BIT(adev->gpio_sdto));
	__raw_writel(oe, adev->gpio_base + 0x00);
}

/* ── DAI ops ────────────────────────────────────────────────────────── */

static int ath79_i2s_startup(struct snd_pcm_substream *ss,
			     struct snd_soc_dai *dai)
{
	struct ath79_i2s_dev *adev = snd_soc_dai_get_drvdata(dai);

	if (!snd_soc_dai_active(dai)) {
		u32 vol_before = stereo_rr(adev, AR934X_STEREO_REG_VOLUME);

		/* I2S_DELAY MOET AAN: de AK4430 staat via DIF (pull-up) in
		 * I²S-modus = data 1 BICK na de LRCK-flank.  4a1671a haalde
		 * dit bit weg ("conform QSDK") — maar de QSDK-referentie
		 * stuurt de ínterne codec aan, niet een externe I²S-DAC.
		 * Zonder delay leest de DAC elk sample 1 bit verschoven =
		 * de zware signaal-gecorreleerde vervorming sinds dinsdag.
		 * Geen SPDIF_ENABLE (idem 8fd3200, de schone build). */
		stereo_wr(adev, AR934X_STEREO_REG_CONFIG,
			  AR934X_STEREO_CONFIG_I2S_ENABLE   |
			  AR934X_STEREO_CONFIG_I2S_DELAY     |
			  AR934X_STEREO_CONFIG_SAMPLE_CNT_CLEAR_TYPE |
			  AR934X_STEREO_CONFIG_MASTER);
		/* VOLUME (0x04): sign-magnitude per kanaal (bit 4 = negatief,
		 * zie QSDK internal-codec), reset 0x0 = 0 dB.  De eerdere
		 * write van 8 per kanaal (sinds 4a1671a) bleek fors te
		 * verzwakken (empirisch: verwijderen maakte alles veel
		 * luider); niet de bron van de vervorming.  Op 0 dB houden. */
		stereo_wr(adev, AR934X_STEREO_REG_VOLUME, 0);
		dev_info(adev->dev,
			 "startup: VOLUME was=0x%08x nu=0x0 (0 dB)\n",
			 vol_before);
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
	int ret;

	ret = ath79_audio_set_freq(adev, params_rate(params));
	if (ret)
		return ret;

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
		mask |= AR934X_STEREO_CONFIG_I2S_WORD_SIZE;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		mask |= AR934X_STEREO_CONFIG_PCM_SWAP;
		fallthrough;
	case SNDRV_PCM_FORMAT_S32_BE:
		mask |= AR934X_STEREO_CONFIG_DATA_WORD_32
			<< AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
		mask |= AR934X_STEREO_CONFIG_I2S_WORD_SIZE;
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
	t &= ~(AR934X_STEREO_CONFIG_I2S_WORD_SIZE |
	       AR934X_STEREO_CONFIG_PCM_SWAP);
	t |= mask;
	stereo_wr(adev, AR934X_STEREO_REG_CONFIG, t);
	spin_unlock(&adev->stereo_lock);

	/* QCA-referentie: stereo-blok resetten na elke format-wijziging */
	ath79_stereo_reset(adev);

	dev_info(adev->dev, "hw_params: rate=%u stereo_cfg=0x%08x posedge=%u\n",
		 params_rate(params),
		 stereo_rr(adev, AR934X_STEREO_REG_CONFIG),
		 stereo_rr(adev, AR934X_STEREO_REG_CONFIG) &
		 AR934X_STEREO_CONFIG_POSEDGE_MASK);

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
			  SNDRV_PCM_INFO_INTERLEAVED,
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

/* ── DMA poll worker (diagnostiek, elke 100 ms) ─────────────────────── */

static void ath79_poll_worker(struct work_struct *work)
{
	struct ath79_i2s_dev *adev =
		container_of(work, struct ath79_i2s_dev, poll_work.work);
	struct ath79_pcm_pltfm_priv *prdata = &adev->pcm_priv;
	struct ath79_pcm_rt_priv *rtpriv;
	struct ath79_pcm_desc *desc;
	int own0 = 0, own1 = 0;
	u32 fifo_st, desc_w0 = 0, desc_w1 = 0, desc_w2 = 0;
	dma_addr_t d0_phys = 0;

	if (prdata->playback && prdata->playback->runtime) {
		rtpriv = prdata->playback->runtime->private_data;
		if (rtpriv) {
			list_for_each_entry(desc, &rtpriv->dma_head, list) {
				if (desc->OWN == 0)
					own0++;
				else
					own1++;
			}
			if (!list_empty(&rtpriv->dma_head)) {
				struct ath79_pcm_desc *d0 =
					list_first_entry(&rtpriv->dma_head,
							 struct ath79_pcm_desc, list);
				d0_phys = d0->phys;
				desc_w0 = __raw_readl(
					(void __iomem *)CKSEG1ADDR((unsigned long)d0_phys));
				desc_w1 = __raw_readl(
					(void __iomem *)CKSEG1ADDR((unsigned long)d0_phys + 4));
				desc_w2 = __raw_readl(
					(void __iomem *)CKSEG1ADDR((unsigned long)d0_phys + 8));
			}
		}
	}

	fifo_st = dma_rr(adev, AR934X_DMA_REG_MBOX_FIFO_STATUS);
	pr_alert("ath79 poll: isr=%d rxbase=%08x rxctl=%08x intst=%08x inten=%08x own0=%d own1=%d fifo=%08x cfg=%08x policy=%08x\n",
		 atomic_read(&adev->irq_count),
		 dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE),
		 dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL),
		 dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS),
		 dma_rr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE),
		 own0, own1,
		 fifo_st,
		 stereo_rr(adev, AR934X_STEREO_REG_CONFIG),
		 dma_rr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY));
	pr_alert("ath79 poll: d0_phys=%08x w0=%08x w1=%08x w2=%08x\n",
		 (u32)d0_phys, desc_w0, desc_w1, desc_w2);

	if (adev->poll_active)
		schedule_delayed_work(&adev->poll_work, msecs_to_jiffies(100));
}

/* ── DMA interrupt handler ───────────────────────────────────────────── */

static void ath79_pcm_handle_rx_complete(struct ath79_i2s_dev *adev,
					 struct snd_pcm_substream *playback,
					 const char *source)
{
	struct ath79_pcm_rt_priv *rtpriv;
	unsigned int period_bytes, played_size;
	struct ath79_pcm_desc *desc;
	u32 status_before, status_after, first[4] = { 0 };
	int own0 = 0, own1 = 0, idx = 0;
	static int rx_debug_left = 12;

	status_before = dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS);
	ath79_mbox_interrupt_ack(adev,
				 AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE);
	status_after = dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS);
	if (!playback)
		return;

	rtpriv = playback->runtime->private_data;
	list_for_each_entry(desc, &rtpriv->dma_head, list) {
		u32 word = *(u32 *)desc;

		if (idx < ARRAY_SIZE(first))
			first[idx] = word;
		if (word & ATH79_PCM_DESC_OWN)
			own1++;
		else
			own0++;
		idx++;
	}
	rtpriv->last_played = ath79_pcm_get_last_played(rtpriv);
	period_bytes = snd_pcm_lib_period_bytes(playback);
	played_size = ath79_pcm_set_own_bits(rtpriv);
	if (!played_size) {
		if (rx_debug_left > 0) {
			struct ath79_pcm_desc *d0 =
				list_first_entry(&rtpriv->dma_head,
						 struct ath79_pcm_desc, list);
			u32 fifo_st  = dma_rr(adev, AR934X_DMA_REG_MBOX_FIFO_STATUS);
			u32 rxbase   = dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE);
			u32 d0_kseg1 = __raw_readl(
				(void __iomem *)CKSEG1ADDR((unsigned long)d0->phys));

			rx_debug_left--;
			dev_info(adev->dev,
				 "%s no-prog: st=%08x->%08x own0=%d own1=%d first=[%08x %08x %08x %08x]\n",
				 source, status_before, status_after, own0, own1,
				 first[0], first[1], first[2], first[3]);
			dev_info(adev->dev,
				 "%s no-prog: fifo_st=%08x rxbase=%08x d0_phys=%08x d0_cached=%08x d0_kseg1=%08x\n",
				 source, fifo_st, rxbase, (u32)d0->phys,
				 first[0], d0_kseg1);
		}
		/* Géén RESUME schrijven — de QCA-referentie doet dat nergens.
		 * Een RESUME na TRIGGER_STOP herstart de engine, die dan
		 * eeuwig door de (straks vrijgegeven) ring blijft lopen. */
		return;
	}

	rtpriv->elapsed_size += played_size;
	if (atomic_read(&adev->irq_count) <= 5)
		dev_info(adev->dev,
			 "%s rx_complete: played=%u elapsed=%u period=%u last=%p intst=0x%08x\n",
			 source, played_size, rtpriv->elapsed_size, period_bytes,
			 rtpriv->last_played, dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS));
	if (rtpriv->elapsed_size >= period_bytes) {
		rtpriv->elapsed_size %= period_bytes;
		snd_pcm_period_elapsed(playback);
	}
}

static irqreturn_t ath79_pcm_interrupt(int irq, void *dev_id)
{
	struct ath79_i2s_dev *adev = dev_id;
	struct ath79_pcm_pltfm_priv *prdata = &adev->pcm_priv;
	struct ath79_pcm_rt_priv *rtpriv;
	u32 status;

	atomic_inc(&adev->irq_count);
	status = dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS);

	/*
	 * Ack ALL set bits immediately.  Bits 0/2/5 are undocumented but remain
	 * set permanently if not cleared — they keep the MISC interrupt line
	 * asserted and prevent the DMA engine from re-firing RX_COMPLETE.
	 */
	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_STATUS, status);
	dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS);  /* flush */

	/* Alleen de eerste paar interrupts loggen — per-ISR logging over de
	 * seriële console verstikt de CPU volledig (ISR elke ~5 ms). */
	if (atomic_read(&adev->irq_count) <= 5)
		dev_info(adev->dev,
			 "ISR #%d: status=0x%08x rx=%d tx=%d playback=%p\n",
			 atomic_read(&adev->irq_count), status,
			 !!(status & AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE),
			 !!(status & AR934X_DMA_MBOX_INT_STATUS_TX_DMA_COMPLETE),
			 prdata->playback);

	if (status & AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE)
		ath79_pcm_handle_rx_complete(adev, prdata->playback, "irq");

	if (status & AR934X_DMA_MBOX_INT_STATUS_TX_DMA_COMPLETE) {
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
	case SNDRV_PCM_TRIGGER_START: {
		dev_info(adev->dev,
			 "trigger START: stereo_cfg=0x%08x mclk_reg=0x%08x clkdiv_1c=0x%08x pll_cfg=0x%08x\n",
			 stereo_rr(adev, AR934X_STEREO_REG_CONFIG),
			 stereo_rr(adev, AR934X_STEREO_REG_MASTER_CLOCK),
			 stereo_rr(adev, 0x1c),
			 pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG));
		/* Verifieer dat de I2S GPIO-mux niet is overschreven */
		dev_info(adev->dev,
			 "trigger START: GPIO_OE=0x%08x FUNC4(16-19)=0x%08x FUNC5(20-23)=0x%08x GPIO_IN=0x%08x\n",
			 __raw_readl(adev->gpio_base + 0x00),
			 __raw_readl(adev->gpio_base + AR934X_GPIO_OUT_FUNCTION4),
			 __raw_readl(adev->gpio_base + AR934X_GPIO_OUT_FUNCTION5),
			 __raw_readl(adev->gpio_base + 0x04));
		/* Re-arm INT_ENABLE in case something cleared the bit since prepare() */
		ath79_mbox_interrupt_enable(adev, AR934X_DMA_MBOX0_INT_RX_COMPLETE);
		if (force_gpio_level >= 0 && force_gpio_mask)
			ath79_force_candidate_gpios(adev, force_gpio_level);
		ath79_mbox_dma_start(adev, rtpriv);
		/* Diagnostiek: bemonster GPIO_IN en rapporteer welke pads
		 * fysiek toggelen — bewijst of de I2S-signalen het SoC verlaten. */
		{
			u32 first = __raw_readl(adev->gpio_base + 0x04);
			u32 toggled = 0;
			int i;

			for (i = 0; i < 20000; i++)
				toggled |= __raw_readl(adev->gpio_base + 0x04) ^ first;
			dev_info(adev->dev,
				 "GPIO toggle-mask tijdens playback: 0x%08x (verwacht bits 18/20/21/22)\n",
				 toggled);
		}
		break;
	}
	case SNDRV_PCM_TRIGGER_STOP:
		adev->poll_active = false;
		cancel_delayed_work_sync(&adev->poll_work);
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
			       ((u32 *)rtpriv->last_played)[1] -
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
#define AR9341_RESET_BASE	0x18060000UL

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
	atomic_set(&adev->irq_count, 0);
	INIT_DELAYED_WORK(&adev->poll_work, ath79_poll_worker);

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

	dev_info(dev, "probe: INT_ENABLE at boot=0x%08x INT_STATUS=0x%08x\n",
		 dma_rr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE),
		 dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS));

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
	dev_info(dev, "probe: PLL_AUDIO_CFG reset=0x%08x PLL_AUDIO_MOD reset=0x%08x\n",
		 pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG),
		 pll_rr(adev, AR934X_PLL_AUDIO_MOD_REG));

	adev->gpio_base = devm_ioremap(dev, AR9341_GPIO_BASE, 0x48);
	if (!adev->gpio_base) {
		dev_err(dev, "cannot map GPIO registers\n");
		return -ENOMEM;
	}

	adev->reset_base = devm_ioremap(dev, AR9341_RESET_BASE, 0x20);
	if (!adev->reset_base) {
		dev_err(dev, "cannot map reset registers\n");
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
	ret = clk_prepare_enable(adev->ref_clk);
	if (ret) {
		dev_err(dev, "cannot enable ref clock: %d\n", ret);
		return ret;
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

	/* Gramofon FON2515A02: de DTS bevat nog de CUS227-pinnen (22/21/20/18),
	 * maar de echte routing is multimeter-bewezen (2026-07-08):
	 * MCLK=GPIO14 -> DAC-pin 4, BICK=GPIO13 -> pin 5,
	 * LRCK=GPIO12 -> pin 7, SDTO=GPIO15 -> pin 6.
	 * TODO: qca9341_fon_fon2415.dts aanpassen en deze override weghalen. */
	adev->gpio_mclk = 14;
	adev->gpio_bick = 13;
	adev->gpio_lrck = 12;
	adev->gpio_sdto = 15;

	/* Reset I2S and MBOX controllers via the reset framework */
	rst_i2s = devm_reset_control_get_exclusive(dev, "i2s");
	if (!IS_ERR(rst_i2s))
		reset_control_reset(rst_i2s);

	rst_mbox = devm_reset_control_get_exclusive(dev, "mbox");
	if (!IS_ERR(rst_mbox))
		reset_control_reset(rst_mbox);

	/* The reset framework pulses reset, but on this board the MBOX control
	 * register still ignores START writes unless the module reset bits are
	 * explicitly deasserted afterwards. */
	{
		u32 rst = __raw_readl(adev->reset_base + 0x1c);

		dev_info(dev, "RESET_MODULE before audio deassert: 0x%08x\n", rst);
		rst &= ~(AR934X_RESET_MBOX | AR934X_RESET_I2S | BIT(10));
		__raw_writel(rst, adev->reset_base + 0x1c);
		rst = __raw_readl(adev->reset_base + 0x1c);
		dev_info(dev, "RESET_MODULE after audio deassert:  0x%08x\n", rst);
	}

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
	struct ath79_i2s_dev *adev = platform_get_drvdata(pdev);

	if (adev && !IS_ERR_OR_NULL(adev->ref_clk))
		clk_disable_unprepare(adev->ref_clk);
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
