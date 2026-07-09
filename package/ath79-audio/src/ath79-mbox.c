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
 *
 * Direction convention (AR9341 MBOX from host perspective):
 * RX channel (0x18/0x1c): DDR -> MBOX FIFO -> I2S out = playback.
 * TX channel (0x20/0x24): I2S in -> MBOX FIFO -> DDR = capture.
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

	dev_info(adev->dev,
		 "INT_ENABLE mask=0x%08x before=0x%08x wrote=0x%08x readback=0x%08x\n",
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
	u32 gate0, gate1, rxctl, txctl;

	gate0 = dma_rr(adev, 0x0c);
	gate1 = dma_rr(adev, 0x14);
	rxctl = dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL);
	txctl = dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL);
	dev_info(adev->dev,
		 "dma_start pre: gate0(0c)=0x%08x gate1(14)=0x%08x rxctl=0x%08x txctl=0x%08x reset=0x%08x\n",
		 gate0, gate1, rxctl, txctl,
		 adev->reset_base ? __raw_readl(adev->reset_base + 0x1c) : 0);

	/* START only, conform QCA-referentie (nu op het juiste bit, BIT(1)) */
	if (rtpriv->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_START);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL);
	} else {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_START);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL);
	}

	dev_info(adev->dev,
		 "dma_start post: gate0(0c)=0x%08x gate1(14)=0x%08x rxctl=0x%08x txctl=0x%08x\n",
		 dma_rr(adev, 0x0c), dma_rr(adev, 0x14),
		 dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL),
		 dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL));
}

void ath79_mbox_dma_stop(struct ath79_i2s_dev *adev,
			 struct ath79_pcm_rt_priv *rtpriv)
{
	if (rtpriv->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_STOP);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL);
	} else {
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL,
		       AR934X_DMA_MBOX_DMA_CONTROL_STOP);
		dma_rr(adev, AR934X_DMA_REG_MBOX0_DMA_TX_CONTROL);
	}
	/*
	 * Wait long enough for the DMA engine to finish the current
	 * transfer.  delay_time is calculated from sample rate + margin.
	 */
	mdelay(rtpriv->delay_time);
}

void ath79_mbox_dma_reset(struct ath79_i2s_dev *adev)
{
	u32 t;

	/*
	 * Volledige MBOX-modulereset, zoals de QCA-referentie doet bij elke
	 * eerste stream (prepare).  Alleen een FIFO-reset laat de DMA-engine
	 * in een onbruikbare staat achter na een STOP.
	 */
	if (adev->reset_base) {
		spin_lock(&ath79_mbox_lock);
		t = __raw_readl(adev->reset_base + AR934X_RESET_REG_RESET_MODULE);
		pr_alert("ath79 dma_reset: assert MBOX reset (reg=0x%08x)\n", t);
		__raw_writel(t | AR934X_RESET_MBOX,
			     adev->reset_base + AR934X_RESET_REG_RESET_MODULE);
		__raw_readl(adev->reset_base + AR934X_RESET_REG_RESET_MODULE);
		udelay(50);
		__raw_writel(t & ~AR934X_RESET_MBOX,
			     adev->reset_base + AR934X_RESET_REG_RESET_MODULE);
		__raw_readl(adev->reset_base + AR934X_RESET_REG_RESET_MODULE);
		udelay(50);
		spin_unlock(&ath79_mbox_lock);
		pr_alert("ath79 dma_reset: MBOX reset deasserted (reg=0x%08x)\n",
			 __raw_readl(adev->reset_base + AR934X_RESET_REG_RESET_MODULE));
	}

	ath79_mbox_fifo_reset(adev,
			      AR934X_DMA_MBOX0_FIFO_RESET_RX |
			      AR934X_DMA_MBOX0_FIFO_RESET_TX);
	pr_alert("ath79 dma_reset: FIFO reset klaar\n");
}

/* ── Descriptor ring setup ─────────────────────────────────────────────── */

void ath79_mbox_dma_prepare(struct ath79_i2s_dev *adev,
			    struct ath79_pcm_rt_priv *rtpriv)
{
	struct ath79_pcm_desc *desc;
	u32 t;

	dma_wr(adev, AR934X_DMA_REG_MBOX_INT_STATUS,
	       AR934X_DMA_MBOX0_INT_RX_COMPLETE |
	       AR934X_DMA_MBOX0_INT_TX_COMPLETE);

	/*
	 * POLICY conform QCA-referentie: QUANTUM-bit aan voor de actieve
	 * richting, FIFO-threshold 6 op bits 7:4 (TX_FIFO_THRESH_SHIFT wordt
	 * in het origineel voor beide richtingen gebruikt).  De eerdere
	 * "QUANTUM blokkeert alles"-bevinding stamt uit de periode dat de
	 * START/STOP-bits omgewisseld waren en is daarmee ongeldig.
	 */
	if (rtpriv->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		t = dma_rr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY);
		dma_wr(adev, AR934X_DMA_REG_MBOX_DMA_POLICY,
		       t | AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM |
		       (6 << AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT));

		desc = list_first_entry(&rtpriv->dma_head,
					struct ath79_pcm_desc, list);
		dma_wr(adev, AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE,
		       (u32)desc->phys);
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

		desc->size = (bufsize >= offset + period_bytes)
			? period_bytes
			: bufsize - offset;
		desc->length = desc->size;
		((u32 *)desc)[0] = ATH79_PCM_DESC_OWN |
			(desc->size << 12) | desc->length;
		((u32 *)desc)[1] = baseaddr + offset;

		if (desc->list.prev != &rtpriv->dma_head) {
			prev = list_entry(desc->list.prev,
					  struct ath79_pcm_desc, list);
			((u32 *)prev)[2] = desc->phys;
		}

		offset += desc->size;
	} while (offset < bufsize);

	/* Close the ring */
	desc  = list_first_entry(&rtpriv->dma_head, struct ath79_pcm_desc, list);
	prev  = list_entry(rtpriv->dma_head.prev,   struct ath79_pcm_desc, list);
	((u32 *)prev)[2] = desc->phys;

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
