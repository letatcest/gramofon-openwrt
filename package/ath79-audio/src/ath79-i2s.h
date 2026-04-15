/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AR9341 I2S / STEREO / MBOX ASoC driver — shared header
 *
 * Copyright (c) 2013 Qualcomm Atheros, Inc.  (original code)
 * Copyright (c) 2024 Krijn Soeteman  (kernel 5.15/6.6 port)
 */

#ifndef _ATH79_I2S_H_
#define _ATH79_I2S_H_

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <sound/soc.h>

/* ── Register offsets ─────────────────────────────────────────────────── */

/* STEREO block (ioremap base = stereo_base) */
#define AR934X_STEREO_REG_CONFIG		0x00
#define AR934X_STEREO_REG_VOLUME		0x04
#define AR934X_STEREO_REG_MASTER_CLOCK		0x08

#define AR934X_STEREO_CONFIG_SPDIF_ENABLE	BIT(23)
#define AR934X_STEREO_CONFIG_I2S_ENABLE		BIT(21)
#define AR934X_STEREO_CONFIG_MIC_RESET		BIT(20)
#define AR934X_STEREO_CONFIG_RESET		BIT(19)
#define AR934X_STEREO_CONFIG_I2S_DELAY		BIT(18)
#define AR934X_STEREO_CONFIG_PCM_SWAP		BIT(17)
#define AR934X_STEREO_CONFIG_MIC_WORD_SIZE	BIT(16)
#define AR934X_STEREO_CONFIG_STEREO_MONO_SHIFT	14
#define AR934X_STEREO_CONFIG_STEREO_MONO_MASK	0x03
#define AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT 12
#define AR934X_STEREO_CONFIG_DATA_WORD_SIZE_MASK  0x03
#define AR934X_STEREO_CONFIG_DATA_WORD_8	0
#define AR934X_STEREO_CONFIG_DATA_WORD_16	1
#define AR934X_STEREO_CONFIG_DATA_WORD_24	2
#define AR934X_STEREO_CONFIG_DATA_WORD_32	3
#define AR934X_STEREO_CONFIG_I2S_WORD_SIZE	BIT(11)
#define AR934X_STEREO_CONFIG_MCK_SEL		BIT(10)
#define AR934X_STEREO_CONFIG_SAMPLE_CNT_CLEAR_TYPE BIT(9)
#define AR934X_STEREO_CONFIG_MASTER		BIT(8)
#define AR934X_STEREO_CONFIG_POSEDGE_SHIFT	0
#define AR934X_STEREO_CONFIG_POSEDGE_MASK	0xff

/* DMA / MBOX block (ioremap base = dma_base) */
#define AR934X_DMA_REG_MBOX_FIFO		0x00
#define AR934X_DMA_REG_MBOX_FIFO_STATUS		0x08
#define AR934X_DMA_REG_MBOX_DMA_POLICY		0x10
#define AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM	BIT(1)
#define AR934X_DMA_MBOX_DMA_POLICY_TX_QUANTUM	BIT(0)
#define AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT 4

#define AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE 0x18
#define AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL	0x1c
#define AR934X_DMA_REG_MBOX0_DMA_TX_DESCRIPTOR_BASE 0x20
#define AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL	0x24
#define AR934X_DMA_MBOX_DMA_CONTROL_START	BIT(0)
#define AR934X_DMA_MBOX_DMA_CONTROL_STOP	BIT(1)
#define AR934X_DMA_MBOX_DMA_CONTROL_RESUME	BIT(2)

#define AR934X_DMA_REG_MBOX_FIFO_RESET		0x58
#define AR934X_DMA_MBOX0_FIFO_RESET_RX		BIT(0)
#define AR934X_DMA_MBOX0_FIFO_RESET_TX		BIT(4)

#define AR934X_DMA_REG_MBOX_INT_STATUS		0x60
#define AR934X_DMA_REG_MBOX_INT_ENABLE		0x64
#define AR934X_DMA_MBOX0_INT_RX_COMPLETE	BIT(0)
#define AR934X_DMA_MBOX0_INT_TX_COMPLETE	BIT(4)
#define AR934X_DMA_MBOX_INT_STATUS_RX_DMA_COMPLETE BIT(0)
#define AR934X_DMA_MBOX_INT_STATUS_TX_DMA_COMPLETE BIT(4)

/* Reset module register (offset from base 0x18060000) — used via reset framework */
#define AR934X_RESET_MBOX			BIT(1)
#define AR934X_RESET_I2S			BIT(0)

/* PLL — audio config registers (offset from pll_base 0x18050000) */
#define AR934X_PLL_AUDIO_CONFIG_REG		0x30
#define AR934X_PLL_AUDIO_MOD_REG		0x34
#define AR934X_PLL_AUDIO_CONFIG_EXT_DIV_SHIFT	12
#define AR934X_PLL_AUDIO_CONFIG_EXT_DIV_MASK	0x7
#define AR934X_PLL_AUDIO_CONFIG_POSTPLLPWD_SHIFT 7
#define AR934X_PLL_AUDIO_CONFIG_POSTPLLPWD_MASK 0x7
#define AR934X_PLL_AUDIO_CONFIG_REFDIV_SHIFT	0
#define AR934X_PLL_AUDIO_CONFIG_REFDIV_MASK	0x1f
#define AR934X_PLL_AUDIO_CONFIG_PLLPWD		BIT(5)
#define AR934X_PLL_AUDIO_CONFIG_BYPASS		BIT(6)
#define AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_SHIFT 0
#define AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_MASK	0x1ffff
#define AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_SHIFT	17
#define AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_MASK	0x3f

/* Audio DPLL registers (offset from dpll_base 0x18116200) */
#define AR934X_DPLL_REG_2			0x04
#define AR934X_DPLL_REG_3			0x08
#define AR934X_DPLL_REG_4			0x0c
#define AR934X_DPLL_2_KD_SHIFT			10
#define AR934X_DPLL_2_KD_MASK			0x3f
#define AR934X_DPLL_2_KI_SHIFT			6
#define AR934X_DPLL_2_KI_MASK			0xf
#define AR934X_DPLL_2_RANGE			BIT(2)
#define AR934X_DPLL_3_PHASESH_SHIFT		23
#define AR934X_DPLL_3_PHASESH_MASK		0x3
#define AR934X_DPLL_3_DO_MEAS			BIT(0)
#define AR934X_DPLL_3_SQSUM_DVC_SHIFT		3
#define AR934X_DPLL_3_SQSUM_DVC_MASK		0x7ffff
#define AR934X_DPLL_4_MEAS_DONE			BIT(0)

/* GPIO OUT FUNCTION registers (offset from gpio_base 0x18040000) */
#define AR934X_GPIO_OUT_FUNCTION0		0x2c   /* GPIOs  0-3  */
#define AR934X_GPIO_OUT_FUNCTION1		0x30   /* GPIOs  4-7  */
#define AR934X_GPIO_OUT_FUNCTION2		0x34   /* GPIOs  8-11 */
#define AR934X_GPIO_OUT_FUNCTION3		0x38   /* GPIOs 12-15 */
#define AR934X_GPIO_OUT_FUNCTION4		0x3c   /* GPIOs 16-19 */
#define AR934X_GPIO_OUT_FUNCTION5		0x40   /* GPIOs 20-23 */

/* I2S output function mux values */
#define AR934X_GPIO_OUT_MUX_I2S_CLK		12
#define AR934X_GPIO_OUT_MUX_I2S_WS		13
#define AR934X_GPIO_OUT_MUX_I2S_SD		14
#define AR934X_GPIO_OUT_MUX_I2S_MCK		15

/* ── Device state ─────────────────────────────────────────────────────── */

/* Per-card platform state — defined before ath79_i2s_dev which embeds it */
struct ath79_pcm_pltfm_priv {
	struct snd_pcm_substream *playback;
	struct snd_pcm_substream *capture;
};

struct ath79_i2s_dev {
	struct device		*dev;

	void __iomem		*stereo_base;
	void __iomem		*dma_base;
	void __iomem		*pll_base;	 /* devm_ioremap, no resource claim */
	void __iomem		*dpll_base;
	void __iomem		*gpio_base;	 /* devm_ioremap, no resource claim */

	u32			gpio_mclk;
	u32			gpio_bick;
	u32			gpio_lrck;
	u32			gpio_sdto;

	int			irq;
	struct clk		*ref_clk;

	spinlock_t		stereo_lock;
	spinlock_t		pll_lock;

	/*
	 * PCM per-card state embedded here so the ISR and component ops can
	 * always reach it without going through snd_soc_component_get_drvdata
	 * (which shares dev->driver_data with platform_set_drvdata and would
	 * be overwritten on first open()).
	 */
	struct ath79_pcm_pltfm_priv pcm_priv;
};

/* Accessor macros — take the ath79_i2s_dev pointer as first argument */
#define stereo_rr(d, reg)	__raw_readl((d)->stereo_base + (reg))
#define stereo_wr(d, reg, v)	__raw_writel((v), (d)->stereo_base + (reg))
#define dma_rr(d, reg)		__raw_readl((d)->dma_base + (reg))
#define dma_wr(d, reg, v)	__raw_writel((v), (d)->dma_base + (reg))
#define pll_rr(d, reg)		__raw_readl((d)->pll_base + (reg))
#define pll_wr(d, reg, v)	__raw_writel((v), (d)->pll_base + (reg))
#define dpll_rr(d, reg)		__raw_readl((d)->dpll_base + (reg))
#define dpll_wr(d, reg, v)	__raw_writel((v), (d)->dpll_base + (reg))

/* ── DMA descriptor ───────────────────────────────────────────────────── */

struct ath79_pcm_desc {
	/* Hardware descriptor fields — exact layout required by MBOX DMA */
	unsigned int	OWN	:  1,	/* bit 31 */
			EOM	:  1,	/* bit 30 */
			rsvd1	:  6,	/* bits 29-24 */
			size	: 12,	/* bits 23-12: buffer size in bytes */
			length	: 12;	/* bits 11-0: bytes filled */
	unsigned int	rsvd2	:  4,
			BufPtr	: 28;	/* bits 27-0: physical buffer addr >> 0 */
	unsigned int	rsvd3	:  4,
			NextPtr	: 28;	/* bits 27-0: physical next desc addr */

	/* Voice-channel samples (not used for I2S output, kept for layout) */
	unsigned int Va[6];
	unsigned int Ua[6];
	unsigned int Ca[6];
	unsigned int Vb[6];
	unsigned int Ub[6];
	unsigned int Cb[6];

	/* Software bookkeeping (not seen by hardware) */
	struct list_head list;
	dma_addr_t phys;
};

/* Per-substream runtime state */
struct ath79_pcm_rt_priv {
	struct list_head	 dma_head;
	struct ath79_pcm_desc	*last_played;
	unsigned int		 elapsed_size;
	unsigned int		 delay_time;
	int			 direction;
};

/* ── Function prototypes ──────────────────────────────────────────────── */

/* ath79-mbox.c */
void ath79_mbox_fifo_reset(struct ath79_i2s_dev *adev, u32 mask);
void ath79_mbox_interrupt_enable(struct ath79_i2s_dev *adev, u32 mask);
void ath79_mbox_interrupt_ack(struct ath79_i2s_dev *adev, u32 mask);
void ath79_mbox_dma_start(struct ath79_i2s_dev *adev,
			  struct ath79_pcm_rt_priv *rtpriv);
void ath79_mbox_dma_stop(struct ath79_i2s_dev *adev,
			 struct ath79_pcm_rt_priv *rtpriv);
void ath79_mbox_dma_reset(struct ath79_i2s_dev *adev);
void ath79_mbox_dma_prepare(struct ath79_i2s_dev *adev,
			    struct ath79_pcm_rt_priv *rtpriv);
int  ath79_mbox_dma_map(struct ath79_pcm_rt_priv *rtpriv,
			dma_addr_t baseaddr, int period_bytes, int bufsize);
void ath79_mbox_dma_unmap(struct ath79_pcm_rt_priv *rtpriv);
int  ath79_mbox_dma_init(struct device *dev);
void ath79_mbox_dma_exit(void);

/* ath79-i2s-pll.c */
int  ath79_audio_set_freq(struct ath79_i2s_dev *adev, int freq);

/* ath79-i2s-drv.c — stereo block */
void ath79_stereo_reset(struct ath79_i2s_dev *adev);

/* Inline helpers for DMA ring management */
static inline unsigned int
ath79_pcm_set_own_bits(struct ath79_pcm_rt_priv *rtpriv)
{
	struct ath79_pcm_desc *desc;
	unsigned int size_played = 0;

	list_for_each_entry(desc, &rtpriv->dma_head, list) {
		if (desc->OWN == 0) {
			desc->OWN = 1;
			size_played += desc->size;
		}
	}
	return size_played;
}

static inline struct ath79_pcm_desc *
ath79_pcm_get_last_played(struct ath79_pcm_rt_priv *rtpriv)
{
	struct ath79_pcm_desc *desc, *prev;

	prev = list_entry(rtpriv->dma_head.prev, struct ath79_pcm_desc, list);
	list_for_each_entry(desc, &rtpriv->dma_head, list) {
		if (desc->OWN == 1 && prev->OWN == 0)
			return desc;
		prev = desc;
	}
	return NULL;
}

#endif /* _ATH79_I2S_H_ */
