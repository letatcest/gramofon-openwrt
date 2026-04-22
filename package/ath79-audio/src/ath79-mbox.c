// SPDX-License-Identifier: GPL-2.0-only
/*
 * ath79-mbox.c — AR9341 MBOX DMA management
 *
 * Copyright (c) 2013 Qualcomm Atheros, Inc.  (original)
 * Copyright (c) 2024 Krijn Soeteman  (kernel 5.15/6.6 port)
 *
 * The AR9341 MBOX DMA controller uses linked-list descriptors.
 * Each descriptor is 4 words (OWN/size/length, BufPtr, NextPtr, padding)
 * plus six sets of voice-channel sample fields.  For I2S playback we only
 * care about OWN, size, BufPtr, NextPtr.
 */

#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm.h>

#include "ath79-i2s.h"

static DEFINE_SPINLOCK(ath79_mbox_lock);
static struct dma_pool *ath79_pcm_cache;

/* ── Reset ─────────────────────────────────────────────────────────────── */

void ath79_mbox_fifo_reset(struct ath79_i2s_dev *adev, u32 mask)
{
	dma_wr(adev, AR934X_DMA_REG_MBOX_FIFO_RESET, mask);
	udelay(50);
	/* Clear reset bits so the FIFO comes out of reset */
	dma_wr(adev, AR934X_DMA_REG_MBOX_FIFO_RESET, 0);
	/* Datasheet: reset stereo controller whenever MBOX DMA is reset */
	ath79_stereo_reset(adev);
}

/* ── Interrupts ────────────────────────────────────────────────────────── */

void ath79_mbox_interrupt_enable(struct ath79_i2s_dev *adev, u32 mask)
{
	u32 before, to_write, after;

	spin_lock(&ath79_mbox_lock);
	before = dma_rr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE);
	to_write = before | mask;
	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE, to_write);
	after = dma_rr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE);
	spin_unlock(&ath79_mbox_lock);

	pr_alert("ath79 INT_ENABLE mask=0x%08x before=0x%08x wrote=0x%08x after=0x%08x\n",
		 mask, before, to_write, after);
}

void ath79_mbox_interrupt_ack(struct ath79_i2s_dev *adev, u32 mask)
{
	/*
	 * Ack the interrupt at the DMA level.  The MISC interrupt controller
	 * ack is handled automatically by the irqchip when we return
	 * IRQ_HANDLED — no need to touch the MISC_INT_STATUS register.
	 */
	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_STATUS, mask);
	/* Read-back to flush the write */
	dma_rr(adev, AR934X_DMA_REG_MBOX_INT_STATUS);
}

/* ── DMA start / stop ──────────────────────────────────────────────────── */

void ath79_mbox_dma_start(struct ath79_i2s_dev *adev,
			  struct ath79_pcm_rt_priv *rtpriv)
{
	/*
	 * AR9341 MBOX naming from the MBOX controller's perspective:
	 *   RX DMA = MBOX receives from host memory → FIFO → I2S (playback)
	 *   TX DMA = MBOX transmits from I2S → FIFO → host memory (capture)
	 */
	if (rtpriv->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_START);
		pr_alert("ath79 dma_start: RX_CONTROL wrote START, rb=0x%08x INT_EN=0x%08x\n",
			 dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL),
			 dma_rr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE));
	} else {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_START);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL);
	}
}

void ath79_mbox_dma_stop(struct ath79_i2s_dev *adev,
			 struct ath79_pcm_rt_priv *rtpriv)
{
	/* Disable all interrupts first to prevent ISR from issuing RESUME
	 * after STOP, which would cause DMA to run with freed descriptors. */
	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE, 0);

	if (rtpriv->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_STOP);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL);
	} else {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_STOP);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL);
	}
}

void ath79_mbox_dma_reset(struct ath79_i2s_dev *adev)
{
	/* MBOX hardware reset is done via reset framework in probe();
	 * here we only reset the FIFOs and stereo block. */
	ath79_mbox_fifo_reset(adev,
			      AR934X_DMA_MBOX0_FIFO_RESET_RX |
			      AR934X_DMA_MBOX0_FIFO_RESET_TX);
}

/* ── Descriptor ring setup ─────────────────────────────────────────────── */

void ath79_mbox_dma_prepare(struct ath79_i2s_dev *adev,
			    struct ath79_pcm_rt_priv *rtpriv)
{
	struct ath79_pcm_desc *desc;
	u32 t;

	/* Full register dump of MBOX DMA space to verify layout */
	pr_alert("ath79 DMA-A: 04=%08x 08=%08x 0c=%08x 10=%08x 14=%08x 18=%08x\n",
		 dma_rr(adev, 0x04), dma_rr(adev, 0x08),
		 dma_rr(adev, 0x0c), dma_rr(adev, 0x10),
		 dma_rr(adev, 0x14), dma_rr(adev, 0x18));
	pr_alert("ath79 DMA-B: 1c=%08x 20=%08x 24=%08x 28=%08x 2c=%08x 30=%08x\n",
		 dma_rr(adev, 0x1c), dma_rr(adev, 0x20),
		 dma_rr(adev, 0x24), dma_rr(adev, 0x28),
		 dma_rr(adev, 0x2c), dma_rr(adev, 0x30));
	pr_alert("ath79 DMA-C: 34=%08x 38=%08x 3c=%08x 40=%08x 44=%08x 48=%08x\n",
		 dma_rr(adev, 0x34), dma_rr(adev, 0x38),
		 dma_rr(adev, 0x3c), dma_rr(adev, 0x40),
		 dma_rr(adev, 0x44), dma_rr(adev, 0x48));
	pr_alert("ath79 DMA-D: 4c=%08x 50=%08x 54=%08x 58=%08x 5c=%08x 60=%08x\n",
		 dma_rr(adev, 0x4c), dma_rr(adev, 0x50),
		 dma_rr(adev, 0x54), dma_rr(adev, 0x58),
		 dma_rr(adev, 0x5c), dma_rr(adev, 0x60));
	pr_alert("ath79 DMA-E: 64=%08x 68=%08x\n",
		 dma_rr(adev, 0x64), dma_rr(adev, 0x68));

	/* Clear enables and pending DMA-complete status before reprogramming.
	 * Only clear known W1C bits — writing BIT(0)/BIT(2) (FIFO threshold
	 * status, always set) has unknown side-effects. */
	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_ENABLE, 0);
	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_STATUS,
	       AR934X_DMA_MBOX0_INT_TX_COMPLETE |
	       AR934X_DMA_MBOX0_INT_RX_COMPLETE);

	if (rtpriv->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		t = dma_rr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY);
		dma_wr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY,
		       t | AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM |
		       (6 << AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT));

		desc = list_first_entry(&rtpriv->dma_head,
					struct ath79_pcm_desc, list);
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE,
		       (u32)desc->phys);
		pr_alert("ath79 prepare RX: pol_b=0x%08x pol_a=0x%08x base=0x%08x rb=0x%08x w0=0x%08x w1=0x%08x w2=0x%08x stereo=0x%08x\n",
			 t,
			 dma_rr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY),
			 (u32)desc->phys,
			 dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE),
			 ((u32 *)desc)[0], ((u32 *)desc)[1], ((u32 *)desc)[2],
			 stereo_rr(adev, AR934X_STEREO_REG_CONFIG));
		ath79_mbox_interrupt_enable(adev,
					    AR934X_DMA_MBOX0_INT_RX_COMPLETE);
	} else {
		t = dma_rr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY);
		dma_wr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY,
		       t | AR934X_DMA_MBOX_DMA_POLICY_TX_QUANTUM |
		       (6 << AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT));

		desc = list_first_entry(&rtpriv->dma_head,
					struct ath79_pcm_desc, list);
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_DESCRIPTOR_BASE,
		       (u32)desc->phys);
		ath79_mbox_interrupt_enable(adev,
					    AR934X_DMA_MBOX0_INT_TX_COMPLETE);
	}
}

int ath79_mbox_dma_map(struct ath79_pcm_rt_priv *rtpriv,
		       dma_addr_t baseaddr, int period_bytes, int bufsize)
{
	struct ath79_pcm_desc *desc, *prev;
	dma_addr_t desc_p;
	unsigned int offset = 0;

	rtpriv->elapsed_size = 0;

	do {
		desc = dma_pool_alloc(ath79_pcm_cache, GFP_KERNEL, &desc_p);
		if (!desc)
			return -ENOMEM;

		memset(desc, 0, sizeof(*desc));
		desc->phys = desc_p;
		list_add_tail(&desc->list, &rtpriv->dma_head);

		desc->OWN = 1;
		desc->EOM = 0;
		desc->rsvd1 = desc->rsvd2 = desc->rsvd3 = 0;

		desc->size = (bufsize >= offset + period_bytes)
			? period_bytes
			: bufsize - offset;
		desc->BufPtr = baseaddr + offset;
		desc->length = desc->size;

		if (desc->list.prev != &rtpriv->dma_head) {
			prev = list_entry(desc->list.prev,
					  struct ath79_pcm_desc, list);
			prev->NextPtr = desc->phys;
		}

		offset += desc->size;
	} while (offset < bufsize);

	/* Close the ring */
	desc  = list_first_entry(&rtpriv->dma_head, struct ath79_pcm_desc, list);
	prev  = list_entry(rtpriv->dma_head.prev,   struct ath79_pcm_desc, list);
	prev->NextPtr = desc->phys;

	pr_alert("ath79 dma_map: period=%u buf=%u ndesc=%u\n",
		 period_bytes, bufsize, bufsize / period_bytes);

	return 0;
}

void ath79_mbox_dma_unmap(struct ath79_pcm_rt_priv *rtpriv)
{
	struct ath79_pcm_desc *desc, *n;

	list_for_each_entry_safe(desc, n, &rtpriv->dma_head, list) {
		list_del(&desc->list);
		dma_pool_free(ath79_pcm_cache, desc, desc->phys);
	}
}

int ath79_mbox_dma_init(struct device *dev)
{
	ath79_pcm_cache = dma_pool_create("ath79_pcm_pool", dev,
					  sizeof(struct ath79_pcm_desc), 4, 0);
	return ath79_pcm_cache ? 0 : -ENOMEM;
}

void ath79_mbox_dma_exit(void)
{
	dma_pool_destroy(ath79_pcm_cache);
	ath79_pcm_cache = NULL;
}
