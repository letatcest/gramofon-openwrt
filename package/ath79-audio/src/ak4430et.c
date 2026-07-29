// SPDX-License-Identifier: GPL-2.0-only
/*
 * AKM AK4430ET minimal ASoC codec driver
 *
 * The AK4430ET has no control bus (no I2C/SPI).
 * On the Gramofon (FON2415), the DIF pin is hardwired HIGH, selecting
 * I2S 24-bit mode.  This driver declares the supported audio formats;
 * there are no register operations.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

static struct snd_soc_dai_driver ak4430et_dai = {
	.name = "ak4430et-hifi",
	.playback = {
		.stream_name  = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates        = SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
				SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
				SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
		/* Only electrically verified formats (2026-07-15):
		 * S16 and S32_BE produce a clean, linearly scaling sine.
		 * S24 (both LE and BE) has been removed: the 24-bit FIFO
		 * path produces hash and can hang the device in an
		 * interrupt storm.  S32_LE has been removed: PCM_SWAP
		 * swaps per 16-bit half-word and thus garbles 32-bit LE
		 * containers. */
		.formats      = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S16_BE |
				SNDRV_PCM_FMTBIT_S32_BE,
	},
};

static const struct snd_soc_component_driver ak4430et_component = {
	.name = "ak4430et",
};

static int ak4430et_probe(struct platform_device *pdev)
{
	return devm_snd_soc_register_component(&pdev->dev,
					       &ak4430et_component,
					       &ak4430et_dai, 1);
}

static const struct of_device_id ak4430et_of_match[] = {
	{ .compatible = "akm,ak4430et" },
	{},
};
MODULE_DEVICE_TABLE(of, ak4430et_of_match);

static struct platform_driver ak4430et_driver = {
	.driver = {
		.name           = "ak4430et",
		.of_match_table = ak4430et_of_match,
	},
	.probe = ak4430et_probe,
};
module_platform_driver(ak4430et_driver);

MODULE_AUTHOR("Krijn Soeteman");
MODULE_DESCRIPTION("AKM AK4430ET ASoC codec driver (no control bus)");
MODULE_LICENSE("GPL");
