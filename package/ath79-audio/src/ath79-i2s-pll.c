// SPDX-License-Identifier: GPL-2.0-only
/*
 * ath79-i2s-pll.c — AR9341 audio PLL management
 *
 * Copyright (c) 2012 Qualcomm Atheros, Inc.  (original)
 * Copyright (c) 2024 Krijn Soeteman  (kernel 5.15/6.6 port)
 *
 * Configures the AR934X audio PLL (DPLL-based) for the requested
 * sample rate.  Two tables are provided: one for a 25 MHz reference
 * crystal (standard AR9341) and one for 40 MHz boards.
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <sound/pcm_params.h>

#include "ath79-i2s.h"

struct ath79_pll_config {
	unsigned int rate;
	u32 divint;
	u32 divfrac;
	u32 postpllpwd;
	u32 bypass;
	u32 extdiv;
	u32 refdiv;
	u32 posedge;
	u32 ki;
	u32 kd;
	u32 shift;
};

/*
 * MCLK-ratio aangepast voor de AK4430: die accepteert in Normal Speed Mode
 * (t/m 48 kHz) alleen 512fs/768fs/1152fs — géén 256fs zoals de originele
 * QCA-tabel levert.  Daarom extdiv 6→3 (MCLK ×2 = 512fs) en posedge ×2
 * zodat BICK op 64fs blijft.  88.2/96 kHz (Double Speed) mag wél 256fs.
 */
static const struct ath79_pll_config pll_cfg_25MHz[] = {
	/* rate: extdiv/posedge → MCLK-ratio, BICK altijd 64fs */
	{ 22050, 0x15, 0x2B442, 0x3, 0, 0x6, 0x1, 4, 0x4, 0x3d, 0x6 }, /* 512fs */
	{ 32000, 0x17, 0x24F76, 0x3, 0, 0x3, 0x1, 6, 0x4, 0x3d, 0x6 }, /* 768fs */
	/* 44.1k-familie: VCO 541,9 MHz (divint 0x15) geeft op de AR9341 een
	 * instabiele klok (DAC-ratio-detect flipt, "motorgeluid") hoewel hij
	 * binnen het 400-750 MHz-bereik valt.  VCO 722,53 MHz met even
	 * EXT_DIV=4 (50% duty, vereist per datasheet) is wél stabiel. */
	{ 44100, 0x1c, 0x39B02, 0x3, 0, 0x4, 0x1, 4, 0x4, 0x3d, 0x6 }, /* 512fs */
	{ 48000, 0x17, 0x24F76, 0x3, 0, 0x3, 0x1, 4, 0x4, 0x3d, 0x6 }, /* 512fs */
	{ 88200, 0x15, 0x2B442, 0x3, 0, 0x3, 0x1, 2, 0x4, 0x3d, 0x6 }, /* 256fs */
	{ 96000, 0x17, 0x24F76, 0x3, 0, 0x3, 0x1, 2, 0x4, 0x3d, 0x6 }, /* 256fs */
	{ 0,     0,    0,       0,   0, 0,   0,   0, 0,   0,    0   },
};

static const struct ath79_pll_config pll_cfg_40MHz[] = {
	{ 22050, 0x1b, 0x6152,  0x3, 0, 0x6, 0x2, 3, 0x4, 0x32, 0x6 },
	{ 32000, 0x1d, 0x1F6FD, 0x3, 0, 0x6, 0x2, 3, 0x4, 0x32, 0x6 },
	{ 44100, 0x1b, 0x6152,  0x3, 0, 0x6, 0x2, 2, 0x4, 0x32, 0x6 },
	{ 48000, 0x1d, 0x1F6FD, 0x3, 0, 0x6, 0x2, 2, 0x4, 0x32, 0x6 },
	{ 88200, 0x1b, 0x6152,  0x3, 0, 0x6, 0x2, 1, 0x4, 0x32, 0x6 },
	{ 96000, 0x1d, 0x1F6FD, 0x3, 0, 0x6, 0x2, 1, 0x4, 0x32, 0x6 },
	{ 0,     0,    0,       0,   0, 0,   0,   0, 0,   0,    0   },
};

/* Runtime-schakelbare 44,1kHz-variant (A/B-test zonder herbuild):
 * 0 = VCO 722,53 MHz, postpll /8, extdiv 4 (huidige rij)
 * 1 = VCO 541,90 MHz, postpll /4, extdiv 6 (bewezen VCO, even extdiv)
 * 2 = VCO 541,90 MHz, postpll /8, extdiv 3 (QSDK-conform, oneven extdiv)
 * Alle drie: MCLK = 512fs = 22,5792 MHz, posedge 4 → BICK 64fs. */
static int pll_44k_variant;
module_param(pll_44k_variant, int, 0644);
MODULE_PARM_DESC(pll_44k_variant, "44,1kHz PLL-variant: 0=722M/ext4 1=541M/div4/ext6 2=541M/div8/ext3");

/* ── Low-level PLL register helpers ─────────────────────────────────── */

static void pll_set_target_div(struct ath79_i2s_dev *adev,
			       u32 div_int, u32 div_frac)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_MOD_REG);
	if (t & AR934X_PLL_AUDIO_MOD_START)
		dev_info(adev->dev,
			 "audio PLL: START-bit stond AAN (MOD=0x%08x) — gewist, PLL volgt TGT nu direct\n",
			 t);
	t &= ~((AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_MASK
		<< AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_SHIFT) |
	       (AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_MASK
		<< AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_SHIFT) |
	       AR934X_PLL_AUDIO_MOD_START);
	t |= (div_int  & AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_MASK)
		<< AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_SHIFT;
	t |= (div_frac & AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_MASK)
		<< AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_SHIFT;
	pll_wr(adev, AR934X_PLL_AUDIO_MOD_REG, t);
	spin_unlock(&adev->pll_lock);
}

static void pll_set_refdiv(struct ath79_i2s_dev *adev, u32 refdiv)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG);
	t &= ~(AR934X_PLL_AUDIO_CONFIG_REFDIV_MASK
	       << AR934X_PLL_AUDIO_CONFIG_REFDIV_SHIFT);
	t |= (refdiv & AR934X_PLL_AUDIO_CONFIG_REFDIV_MASK)
		<< AR934X_PLL_AUDIO_CONFIG_REFDIV_SHIFT;
	pll_wr(adev, AR934X_PLL_AUDIO_CONFIG_REG, t);
	spin_unlock(&adev->pll_lock);
}

static void pll_set_ext_div(struct ath79_i2s_dev *adev, u32 ext_div)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG);
	t &= ~(AR934X_PLL_AUDIO_CONFIG_EXT_DIV_MASK
	       << AR934X_PLL_AUDIO_CONFIG_EXT_DIV_SHIFT);
	t |= (ext_div & AR934X_PLL_AUDIO_CONFIG_EXT_DIV_MASK)
		<< AR934X_PLL_AUDIO_CONFIG_EXT_DIV_SHIFT;
	pll_wr(adev, AR934X_PLL_AUDIO_CONFIG_REG, t);
	spin_unlock(&adev->pll_lock);
}

static void pll_set_postpllpwd(struct ath79_i2s_dev *adev, u32 postpllpwd)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG);
	t &= ~(AR934X_PLL_AUDIO_CONFIG_POSTPLLPWD_MASK
	       << AR934X_PLL_AUDIO_CONFIG_POSTPLLPWD_SHIFT);
	t |= (postpllpwd & AR934X_PLL_AUDIO_CONFIG_POSTPLLPWD_MASK)
		<< AR934X_PLL_AUDIO_CONFIG_POSTPLLPWD_SHIFT;
	pll_wr(adev, AR934X_PLL_AUDIO_CONFIG_REG, t);
	spin_unlock(&adev->pll_lock);
}

static void pll_bypass(struct ath79_i2s_dev *adev, bool val)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG);
	if (val)
		t |= AR934X_PLL_AUDIO_CONFIG_BYPASS;
	else
		t &= ~AR934X_PLL_AUDIO_CONFIG_BYPASS;
	pll_wr(adev, AR934X_PLL_AUDIO_CONFIG_REG, t);
	spin_unlock(&adev->pll_lock);
}

static void pll_powerup(struct ath79_i2s_dev *adev)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG);
	t &= ~AR934X_PLL_AUDIO_CONFIG_PLLPWD;
	pll_wr(adev, AR934X_PLL_AUDIO_CONFIG_REG, t);
	spin_unlock(&adev->pll_lock);
}

static void pll_powerdown(struct ath79_i2s_dev *adev)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG);
	t |= AR934X_PLL_AUDIO_CONFIG_PLLPWD;
	pll_wr(adev, AR934X_PLL_AUDIO_CONFIG_REG, t);
	spin_unlock(&adev->pll_lock);
}

/* ── DPLL helpers ────────────────────────────────────────────────────── */

static void dpll_set_gains(struct ath79_i2s_dev *adev, u32 kd, u32 ki)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = dpll_rr(adev, AR934X_DPLL_REG_2);
	t &= ~((AR934X_DPLL_2_KD_MASK << AR934X_DPLL_2_KD_SHIFT) |
	       (AR934X_DPLL_2_KI_MASK << AR934X_DPLL_2_KI_SHIFT));
	t |= (kd & AR934X_DPLL_2_KD_MASK) << AR934X_DPLL_2_KD_SHIFT;
	t |= (ki & AR934X_DPLL_2_KI_MASK) << AR934X_DPLL_2_KI_SHIFT;
	dpll_wr(adev, AR934X_DPLL_REG_2, t);
	spin_unlock(&adev->pll_lock);
}

static void dpll_phase_shift_set(struct ath79_i2s_dev *adev, u32 phase)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = dpll_rr(adev, AR934X_DPLL_REG_3);
	t &= ~(AR934X_DPLL_3_PHASESH_MASK << AR934X_DPLL_3_PHASESH_SHIFT);
	t |= (phase & AR934X_DPLL_3_PHASESH_MASK)
		<< AR934X_DPLL_3_PHASESH_SHIFT;
	dpll_wr(adev, AR934X_DPLL_REG_3, t);
	spin_unlock(&adev->pll_lock);
}

static void dpll_range_set(struct ath79_i2s_dev *adev)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = dpll_rr(adev, AR934X_DPLL_REG_2);
	t &= ~AR934X_DPLL_2_RANGE;
	dpll_wr(adev, AR934X_DPLL_REG_2, t);
	t |= AR934X_DPLL_2_RANGE;
	dpll_wr(adev, AR934X_DPLL_REG_2, t);
	spin_unlock(&adev->pll_lock);
}

static void dpll_do_meas_set(struct ath79_i2s_dev *adev)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = dpll_rr(adev, AR934X_DPLL_REG_3);
	t |= AR934X_DPLL_3_DO_MEAS;
	dpll_wr(adev, AR934X_DPLL_REG_3, t);
	spin_unlock(&adev->pll_lock);
}

static void dpll_do_meas_clear(struct ath79_i2s_dev *adev)
{
	u32 t;

	spin_lock(&adev->pll_lock);
	t = dpll_rr(adev, AR934X_DPLL_REG_3);
	t &= ~AR934X_DPLL_3_DO_MEAS;
	dpll_wr(adev, AR934X_DPLL_REG_3, t);
	spin_unlock(&adev->pll_lock);
}

static bool dpll_meas_done(struct ath79_i2s_dev *adev)
{
	return !!(dpll_rr(adev, AR934X_DPLL_REG_4) & AR934X_DPLL_4_MEAS_DONE);
}

static u32 dpll_sqsum_dvc(struct ath79_i2s_dev *adev)
{
	return (dpll_rr(adev, AR934X_DPLL_REG_3) >> AR934X_DPLL_3_SQSUM_DVC_SHIFT)
		& AR934X_DPLL_3_SQSUM_DVC_MASK;
}

/* Logt target- vs. fysiek-gebruikte divider (CURRENT_AUDIO_PLL_MODULATION).
 * Als CUR niet gelijk is aan TGT gebruikt de VCO een andere frequentie dan
 * geprogrammeerd — precies wat we bij 44,1 kHz verdenken. */
static void pll_log_current(struct ath79_i2s_dev *adev, const char *tag)
{
	u32 mod = pll_rr(adev, AR934X_PLL_AUDIO_MOD_REG);
	u32 step = pll_rr(adev, AR934X_PLL_AUDIO_MOD_STEP_REG);
	u32 cur = pll_rr(adev, AR934X_PLL_AUDIO_CUR_MOD_REG);
	u32 tgt_int = (mod >> AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_SHIFT)
			& AR934X_PLL_AUDIO_MOD_TGT_DIV_INT_MASK;
	u32 tgt_frac = (mod >> AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_SHIFT)
			& AR934X_PLL_AUDIO_MOD_TGT_DIV_FRAC_MASK;
	u32 cur_int = (cur >> AR934X_PLL_AUDIO_CUR_INT_SHIFT)
			& AR934X_PLL_AUDIO_CUR_INT_MASK;
	u32 cur_frac = (cur >> AR934X_PLL_AUDIO_CUR_FRAC_SHIFT)
			& AR934X_PLL_AUDIO_CUR_FRAC_MASK;

	dev_info(adev->dev,
		 "PLL %s: TGT=%u+%u/262144 CUR=%u+%u/262144 (MOD=0x%08x STEP=0x%08x CURREG=0x%08x START=%lu)\n",
		 tag, tgt_int, tgt_frac, cur_int, cur_frac, mod, step, cur,
		 mod & AR934X_PLL_AUDIO_MOD_START);
}

/* ── Stereo posedge ──────────────────────────────────────────────────── */

static void stereo_set_posedge(struct ath79_i2s_dev *adev, u32 posedge)
{
	u32 t;

	spin_lock(&adev->stereo_lock);
	t = stereo_rr(adev, AR934X_STEREO_REG_CONFIG);
	t &= ~(AR934X_STEREO_CONFIG_POSEDGE_MASK
	       << AR934X_STEREO_CONFIG_POSEDGE_SHIFT);
	t |= (posedge & AR934X_STEREO_CONFIG_POSEDGE_MASK)
		<< AR934X_STEREO_CONFIG_POSEDGE_SHIFT;
	stereo_wr(adev, AR934X_STEREO_REG_CONFIG, t);
	spin_unlock(&adev->stereo_lock);
}

/* ── Top-level frequency setter ──────────────────────────────────────── */

int ath79_audio_set_freq(struct ath79_i2s_dev *adev, int freq)
{
	const struct ath79_pll_config *cfg;
	struct ath79_pll_config vcfg;
	unsigned long ref_rate;
	int retries;

	ref_rate = clk_get_rate(adev->ref_clk);
	dev_info(adev->dev, "audio PLL: ref_rate=%lu Hz, target=%d Hz\n",
		 ref_rate, freq);

	switch (ref_rate) {
	case 25000000:
		cfg = pll_cfg_25MHz;
		break;
	case 40000000:
		cfg = pll_cfg_40MHz;
		break;
	default:
		dev_err(adev->dev, "unsupported ref clock %lu Hz\n", ref_rate);
		return -EIO;
	}

	while (cfg->rate && cfg->rate != (unsigned int)freq)
		cfg++;

	if (!cfg->rate) {
		dev_err(adev->dev, "unsupported sample rate %d Hz\n", freq);
		return -EINVAL;
	}

	if (freq == 44100 && ref_rate == 25000000 && pll_44k_variant) {
		vcfg = *cfg;
		switch (pll_44k_variant) {
		case 1:	/* VCO 541,9008 MHz / postpll 4 / extdiv 6 (even) */
			vcfg.divint = 0x15;
			vcfg.divfrac = 0x2B442;
			vcfg.postpllpwd = 0x2;
			vcfg.extdiv = 0x6;
			break;
		case 2:	/* QSDK-conform: VCO 541,9008 / postpll 8 / extdiv 3 */
			vcfg.divint = 0x15;
			vcfg.divfrac = 0x2B442;
			vcfg.postpllpwd = 0x3;
			vcfg.extdiv = 0x3;
			break;
		}
		cfg = &vcfg;
		dev_info(adev->dev, "44,1kHz-variant %d actief (divint=0x%x extdiv=%u postpll=%u)\n",
			 pll_44k_variant, cfg->divint, cfg->extdiv, cfg->postpllpwd);
	}

	pll_log_current(adev, "vóór herprogrammering");

	/* Converge the DPLL — both loops are bounded to avoid starving
	 * the single-core MIPS CPU with infinite udelay() spins. */
	retries = 20;

	do {
		int meas_timeout = 1000;  /* 1000 × 10 µs = 10 ms */

		dpll_do_meas_clear(adev);
		pll_powerdown(adev);
		udelay(100);

		pll_set_postpllpwd(adev, cfg->postpllpwd);
		pll_bypass(adev, cfg->bypass);
		pll_set_ext_div(adev, cfg->extdiv);
		pll_set_refdiv(adev, cfg->refdiv);
		pll_set_target_div(adev, cfg->divint, cfg->divfrac);
		dpll_range_set(adev);
		dpll_phase_shift_set(adev, cfg->shift);
		dpll_set_gains(adev, cfg->kd, cfg->ki);
		stereo_set_posedge(adev, cfg->posedge);

		pll_powerup(adev);
		/* Give the PLL time to stabilise before triggering measurement */
		mdelay(10);
		dpll_do_meas_clear(adev);
		dpll_do_meas_set(adev);

		dev_info(adev->dev,
			 "PLL CFG=0x%08x MOD=0x%08x DPLL2=0x%08x DPLL3=0x%08x DPLL4=0x%08x\n",
			 pll_rr(adev, AR934X_PLL_AUDIO_CONFIG_REG),
			 pll_rr(adev, AR934X_PLL_AUDIO_MOD_REG),
			 dpll_rr(adev, AR934X_DPLL_REG_2),
			 dpll_rr(adev, AR934X_DPLL_REG_3),
			 dpll_rr(adev, AR934X_DPLL_REG_4));

		while (!dpll_meas_done(adev) && --meas_timeout > 0)
			udelay(10);

		dev_info(adev->dev,
			 "after poll: DPLL4=0x%08x timeout_left=%d\n",
			 dpll_rr(adev, AR934X_DPLL_REG_4), meas_timeout);

		/* If the DPLL measurement hardware is unresponsive (DPLL4
		 * MEAS_DONE never fires), fall through anyway — the PLL tables
		 * are derived from the reference crystal and should be correct
		 * without closed-loop verification. */
		if (!meas_timeout)
			dev_warn(adev->dev, "DPLL measurement timeout — continuing\n");

	} while (dpll_sqsum_dvc(adev) >= 0x40000 && --retries > 0);

	dev_info(adev->dev, "audio PLL configured: sqsum_dvc=0x%x retries_left=%d\n",
		 dpll_sqsum_dvc(adev), retries);

	pll_log_current(adev, "na herprogrammering");

	return 0;
}
