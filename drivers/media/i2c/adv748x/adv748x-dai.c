// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Analog Devices ADV748X HDMI receiver with AFE
 * The implementation of DAI.
 */

#include "adv748x.h"

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <sound/pcm_params.h>

#define state_of(soc_dai) \
	adv748x_dai_to_state(container_of((soc_dai)->driver, \
					  struct adv748x_dai, drv))
#define mclk_of(state) ((state)->dai.mclk_hw->clk)

static const char ADV748X_DAI_NAME[] = "adv748x-i2s";

static int set_audio_pads_state(struct adv748x_state *state, int on)
{
	return io_clrset(state, ADV748X_IO_PAD_CONTROLS,
			 ADV748X_IO_PAD_CONTROLS_TRI_AUD |
			 ADV748X_IO_PAD_CONTROLS_PDN_AUD,
			 on ? 0 : 0xff);
}

static int set_dpll_mclk_fs(struct adv748x_state *state, int fs)
{
	return dpll_clrset(state, ADV748X_DPLL_MCLK_FS,
			   ADV748X_DPLL_MCLK_FS_N_MASK, (fs / 128) - 1);
}

static int set_i2s_format(struct adv748x_state *state, uint outmode,
			  uint bitwidth)
{
	return hdmi_clrset(state, ADV748X_HDMI_I2S,
			   ADV748X_HDMI_I2SBITWIDTH_MASK |
			   ADV748X_HDMI_I2SOUTMODE_MASK,
			   (outmode << ADV748X_HDMI_I2SOUTMODE_SHIFT) |
			   bitwidth);
}

static int set_i2s_tdm_mode(struct adv748x_state *state, int is_tdm)
{
	int ret;

	ret = hdmi_clrset(state, ADV748X_HDMI_AUDIO_MUTE_SPEED,
			  ADV748X_MAN_AUDIO_DL_BYPASS |
			  ADV748X_AUDIO_DELAY_LINE_BYPASS,
			  is_tdm ? 0xff : 0);
	if (ret < 0)
		return ret;
	ret = hdmi_clrset(state, ADV748X_HDMI_REG_6D,
			  ADV748X_I2S_TDM_MODE_ENABLE,
			  is_tdm ? 0xff : 0);
	return ret;
}

static int set_audio_mute(struct adv748x_state *state, int enable)
{
	return hdmi_clrset(state, ADV748X_HDMI_MUTE_CTRL,
			   ADV748X_HDMI_MUTE_CTRL_MUTE_AUDIO,
			   enable ? 0xff : 0);
}

static int adv748x_dai_set_sysclk(struct snd_soc_dai *dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct adv748x_state *state = state_of(dai);

	/* currently supporting only one fixed rate clock */
	if (clk_id != 0 || freq != clk_get_rate(mclk_of(state))) {
		dev_err(dai->dev, "invalid clock (%d) or frequency (%u, dir %d)\n",
			clk_id, freq, dir);
		return -EINVAL;
	}
	state->dai.freq = freq;
	return 0;
}

static int adv748x_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct adv748x_state *state = state_of(dai);
	int ret = 0;

	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBM_CFM) {
		dev_err(dai->dev, "only I2S master clock mode supported\n");
		ret = -EINVAL;
		goto done;
	}
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAI_FORMAT_I2S:
		state->dai.tdm = 0;
		state->dai.fmt = ADV748X_I2SOUTMODE_I2S;
		break;
	case SND_SOC_DAI_FORMAT_RIGHT_J:
		state->dai.tdm = 1;
		state->dai.fmt = ADV748X_I2SOUTMODE_RIGHT_J;
		break;
	case SND_SOC_DAI_FORMAT_LEFT_J:
		state->dai.tdm = 1;
		state->dai.fmt = ADV748X_I2SOUTMODE_LEFT_J;
		break;
	default:
		dev_err(dai->dev, "only i2s, left_j and right_j supported\n");
		ret = -EINVAL;
		goto done;
	}
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF) {
		dev_err(dai->dev, "only normal bit clock + frame supported\n");
		ret = -EINVAL;
	}
done:
	return ret;
}

static int adv748x_dai_startup(struct snd_pcm_substream *sub, struct snd_soc_dai *dai)
{
	int ret;
	struct adv748x_state *state = state_of(dai);

	if (sub->stream != SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;
	ret = set_audio_pads_state(state, 1);
	if (ret)
		goto fail;
	ret = clk_prepare_enable(mclk_of(state));
	if (ret)
		goto fail_pwdn;
	return 0;
fail_pwdn:
	set_audio_pads_state(state, 0);
fail:
	return ret;
}

static int adv748x_dai_hw_params(struct snd_pcm_substream *sub,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	int ret;
	struct adv748x_state *state = state_of(dai);
	uint fs = state->dai.freq / params_rate(params);

	dev_dbg(dai->dev, "dai %s substream %s rate=%u (fs=%u), channels=%u sample width=%u(%u)\n",
		dai->name, sub->name,
		params_rate(params), fs,
		params_channels(params),
		params_width(params),
		params_physical_width(params));
	switch (fs) {
	case 128:
	case 256:
	case 384:
	case 512:
	case 640:
	case 768:
		break;
	default:
		ret = -EINVAL;
		dev_err(dai->dev, "invalid clock frequency (%u) or rate (%u)\n",
			state->dai.freq, params_rate(params));
		goto done;
	}
	ret = set_dpll_mclk_fs(state, fs);
	if (ret)
		goto done;
	ret = set_i2s_tdm_mode(state, state->dai.tdm);
	if (ret)
		goto done;
	ret = set_i2s_format(state, state->dai.fmt, params_width(params));
done:
	return ret;
}

static int adv748x_dai_mute_stream(struct snd_soc_dai *dai, int mute, int dir)
{
	struct adv748x_state *state = state_of(dai);

	return set_audio_mute(state, mute);
}

static void adv748x_dai_shutdown(struct snd_pcm_substream *sub, struct snd_soc_dai *dai)
{
	struct adv748x_state *state = state_of(dai);

	clk_disable_unprepare(mclk_of(state));
	set_audio_pads_state(state, 0);
}

static const struct snd_soc_dai_ops adv748x_dai_ops = {
	.set_sysclk = adv748x_dai_set_sysclk,
	.set_fmt = adv748x_dai_set_fmt,
	.startup = adv748x_dai_startup,
	.hw_params = adv748x_dai_hw_params,
	.mute_stream = adv748x_dai_mute_stream,
	.shutdown = adv748x_dai_shutdown,
};

static	int adv748x_of_xlate_dai_name(struct snd_soc_component *component,
				      struct of_phandle_args *args,
				      const char **dai_name)
{
	if (dai_name)
		*dai_name = ADV748X_DAI_NAME;
	return 0;
}

static const struct snd_soc_component_driver adv748x_codec = {
	.of_xlate_dai_name = adv748x_of_xlate_dai_name,
};

int adv748x_dai_init(struct adv748x_dai *dai)
{
	int ret;
	struct adv748x_state *state = adv748x_dai_to_state(dai);

	if (!state->endpoints[ADV748X_PORT_I2S]) {
		adv_info(state, "no I2S port, DAI disabled\n");
		ret = 0;
		goto fail;
	}
	dai->mclk_name = kasprintf(GFP_KERNEL, "%s.%s-i2s-mclk",
				   state->dev->driver->name,
				   dev_name(state->dev));
	if (!dai->mclk_name) {
		ret = -ENOMEM;
		adv_err(state, "No memory for MCLK\n");
		goto fail;
	}
	dai->mclk_hw = clk_hw_register_fixed_rate(state->dev, dai->mclk_name,
						  NULL, 0, 12288000);
	if (IS_ERR(dai->mclk_hw)) {
		ret = PTR_ERR(dai->mclk_hw);
		adv_err(state, "Failed to register MCLK (%d)\n", ret);
		goto fail;
	}
	ret = of_clk_add_hw_provider(state->dev->of_node, of_clk_hw_simple_get,
				     dai->mclk_hw->clk);
	if (ret < 0) {
		adv_err(state, "Failed to add MCLK provider (%d)\n", ret);
		goto unreg_mclk;
	}
	dai->drv.name = ADV748X_DAI_NAME;
	dai->drv.ops = &adv748x_dai_ops;
	dai->drv.capture = (const struct snd_soc_pcm_stream){
		.stream_name	= "Capture",
		.channels_min	= 8,
		.channels_max	= 8,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_U24_LE,
	};

	ret = devm_snd_soc_register_component(state->dev, &adv748x_codec,
					      &dai->drv, 1);
	if (ret < 0) {
		adv_err(state, "Failed to register the codec (%d)\n", ret);
		goto cleanup_mclk;
	}
	return 0;

cleanup_mclk:
	of_clk_del_provider(state->dev->of_node);
unreg_mclk:
	clk_hw_unregister_fixed_rate(dai->mclk_hw);
fail:
	return ret;
}

void adv748x_dai_cleanup(struct adv748x_dai *dai)
{
	struct adv748x_state *state = adv748x_dai_to_state(dai);

	of_clk_del_provider(state->dev->of_node);
	clk_hw_unregister_fixed_rate(dai->mclk_hw);
	kfree(dai->mclk_name);
}
