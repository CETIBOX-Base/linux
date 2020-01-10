/*
 * Driver for Analog Devices ADV748X HDMI receiver and Component Processor (CP)
 *
 * Copyright (C) 2017 Renesas Electronics Corp.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/mutex.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-ioctl.h>

#include <uapi/linux/v4l2-dv-timings.h>

#include "adv748x.h"

#define HDMI_AOUT_NONE 0
#define HDMI_AOUT_I2S 1
#define HDMI_AOUT_I2S_TDM 2

static int default_audio_out;
module_param_named(aout, default_audio_out, int, 0444);

/* -----------------------------------------------------------------------------
 * HDMI and CP
 */

#define ADV748X_HDMI_MIN_WIDTH		640
#define ADV748X_HDMI_MAX_WIDTH		1920
#define ADV748X_HDMI_MIN_HEIGHT		480
#define ADV748X_HDMI_MAX_HEIGHT		1200

/* V4L2_DV_BT_CEA_720X480I59_94 - 0.5 MHz */
#define ADV748X_HDMI_MIN_PIXELCLOCK	13000000
/* V4L2_DV_BT_DMT_1600X1200P60 */
#define ADV748X_HDMI_MAX_PIXELCLOCK	162000000

static const struct v4l2_dv_timings_cap adv748x_hdmi_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	/* keep this initialization for compatibility with GCC < 4.4.6 */
	.reserved = { 0 },

	V4L2_INIT_BT_TIMINGS(ADV748X_HDMI_MIN_WIDTH, ADV748X_HDMI_MAX_WIDTH,
			     ADV748X_HDMI_MIN_HEIGHT, ADV748X_HDMI_MAX_HEIGHT,
			     ADV748X_HDMI_MIN_PIXELCLOCK,
			     ADV748X_HDMI_MAX_PIXELCLOCK,
			     V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT,
			     V4L2_DV_BT_CAP_PROGRESSIVE)
};

struct adv748x_hdmi_video_standards {
	struct v4l2_dv_timings timings;
	u8 vid_std;
	u8 v_freq;
};

static const struct adv748x_hdmi_video_standards
adv748x_hdmi_video_standards[] = {
	{ V4L2_DV_BT_CEA_720X480P59_94, 0x4a, 0x00 },
	{ V4L2_DV_BT_CEA_720X576P50, 0x4b, 0x00 },
	{ V4L2_DV_BT_CEA_1280X720P60, 0x53, 0x00 },
	{ V4L2_DV_BT_CEA_1280X720P50, 0x53, 0x01 },
	{ V4L2_DV_BT_CEA_1280X720P30, 0x53, 0x02 },
	{ V4L2_DV_BT_CEA_1280X720P25, 0x53, 0x03 },
	{ V4L2_DV_BT_CEA_1280X720P24, 0x53, 0x04 },
	{ V4L2_DV_BT_CEA_1920X1080P60, 0x5e, 0x00 },
	{ V4L2_DV_BT_CEA_1920X1080P50, 0x5e, 0x01 },
	{ V4L2_DV_BT_CEA_1920X1080P30, 0x5e, 0x02 },
	{ V4L2_DV_BT_CEA_1920X1080P25, 0x5e, 0x03 },
	{ V4L2_DV_BT_CEA_1920X1080P24, 0x5e, 0x04 },
	/* SVGA */
	{ V4L2_DV_BT_DMT_800X600P56, 0x80, 0x00 },
	{ V4L2_DV_BT_DMT_800X600P60, 0x81, 0x00 },
	{ V4L2_DV_BT_DMT_800X600P72, 0x82, 0x00 },
	{ V4L2_DV_BT_DMT_800X600P75, 0x83, 0x00 },
	{ V4L2_DV_BT_DMT_800X600P85, 0x84, 0x00 },
	/* SXGA */
	{ V4L2_DV_BT_DMT_1280X1024P60, 0x85, 0x00 },
	{ V4L2_DV_BT_DMT_1280X1024P75, 0x86, 0x00 },
	/* VGA */
	{ V4L2_DV_BT_DMT_640X480P60, 0x88, 0x00 },
	{ V4L2_DV_BT_DMT_640X480P72, 0x89, 0x00 },
	{ V4L2_DV_BT_DMT_640X480P75, 0x8a, 0x00 },
	{ V4L2_DV_BT_DMT_640X480P85, 0x8b, 0x00 },
	/* XGA */
	{ V4L2_DV_BT_DMT_1024X768P60, 0x8c, 0x00 },
	{ V4L2_DV_BT_DMT_1024X768P70, 0x8d, 0x00 },
	{ V4L2_DV_BT_DMT_1024X768P75, 0x8e, 0x00 },
	{ V4L2_DV_BT_DMT_1024X768P85, 0x8f, 0x00 },
	/* UXGA */
	{ V4L2_DV_BT_DMT_1600X1200P60, 0x96, 0x00 },
};

static void adv748x_hdmi_fill_format(struct adv748x_hdmi *hdmi,
				     struct v4l2_mbus_framefmt *fmt)
{
	memset(fmt, 0, sizeof(*fmt));

	fmt->code = MEDIA_BUS_FMT_RGB888_1X24;
	fmt->field = hdmi->timings.bt.interlaced ?
			V4L2_FIELD_ALTERNATE : V4L2_FIELD_NONE;

	/* TODO: The colorspace depends on the AVI InfoFrame contents */
	fmt->colorspace = V4L2_COLORSPACE_SRGB;

	fmt->width = hdmi->timings.bt.width;
	fmt->height = hdmi->timings.bt.height;
}

static void adv748x_fill_optional_dv_timings(struct v4l2_dv_timings *timings)
{
	v4l2_find_dv_timings_cap(timings, &adv748x_hdmi_timings_cap,
				 250000, NULL, NULL);
}

static bool adv748x_hdmi_has_signal(struct adv748x_state *state)
{
	int val;

	/* Check that VERT_FILTER and DE_REGEN is locked */
	val = hdmi_read(state, ADV748X_HDMI_LW1);
	return (val & ADV748X_HDMI_LW1_VERT_FILTER) &&
	       (val & ADV748X_HDMI_LW1_DE_REGEN);
}

static int adv748x_hdmi_read_pixelclock(struct adv748x_state *state)
{
	int a, b;

	a = hdmi_read(state, ADV748X_HDMI_TMDS_1);
	b = hdmi_read(state, ADV748X_HDMI_TMDS_2);
	if (a < 0 || b < 0)
		return -ENODATA;

	/*
	 * The high 9 bits store TMDS frequency measurement in MHz
	 * The low 7 bits of TMDS_2 store the 7-bit TMDS fractional frequency
	 * measurement in 1/128 MHz
	 */
	return ((a << 1) | (b >> 7)) * 1000000 + (b & 0x7f) * 1000000 / 128;
}

/*
 * adv748x_hdmi_set_de_timings: Adjust horizontal picture offset through DE
 *
 * HDMI CP uses a Data Enable synchronisation timing reference
 *
 * Vary the leading and trailing edge position of the DE signal output by the CP
 * core. Values are stored as signed-twos-complement in one-pixel-clock units
 *
 * The start and end are shifted equally by the 10-bit shift value.
 */
static void adv748x_hdmi_set_de_timings(struct adv748x_state *state, int shift)
{
	u8 high, low;

	/* POS_HIGH stores bits 8 and 9 of both the start and end */
	high = ADV748X_CP_DE_POS_HIGH_SET;
	high |= (shift & 0x300) >> 8;
	low = shift & 0xff;

	/* The sequence of the writes is important and must be followed */
	cp_write(state, ADV748X_CP_DE_POS_HIGH, high);
	cp_write(state, ADV748X_CP_DE_POS_END_LOW, low);

	high |= (shift & 0x300) >> 6;

	cp_write(state, ADV748X_CP_DE_POS_HIGH, high);
	cp_write(state, ADV748X_CP_DE_POS_START_LOW, low);
}

static int adv748x_hdmi_set_video_timings(struct adv748x_state *state,
					  const struct v4l2_dv_timings *timings)
{
	const struct adv748x_hdmi_video_standards *stds =
		adv748x_hdmi_video_standards;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(adv748x_hdmi_video_standards); i++) {
		if (!v4l2_match_dv_timings(timings, &stds[i].timings, 250000,
					   false))
			continue;
	}

	if (i >= ARRAY_SIZE(adv748x_hdmi_video_standards))
		return -EINVAL;

	/*
	 * When setting cp_vid_std to either 720p, 1080i, or 1080p, the video
	 * will get shifted horizontally to the left in active video mode.
	 * The de_h_start and de_h_end controls are used to centre the picture
	 * correctly
	 */
	switch (stds[i].vid_std) {
	case 0x53: /* 720p */
		adv748x_hdmi_set_de_timings(state, -40);
		break;
	case 0x54: /* 1080i */
	case 0x5e: /* 1080p */
		adv748x_hdmi_set_de_timings(state, -44);
		break;
	default:
		adv748x_hdmi_set_de_timings(state, 0);
		break;
	}

	io_write(state, ADV748X_IO_VID_STD, stds[i].vid_std);
	io_clrset(state, ADV748X_IO_DATAPATH, ADV748X_IO_DATAPATH_VFREQ_M,
		  stds[i].v_freq << ADV748X_IO_DATAPATH_VFREQ_SHIFT);

	return 0;
}

/* -----------------------------------------------------------------------------
 * v4l2_subdev_video_ops
 */

static int adv748x_hdmi_s_dv_timings(struct v4l2_subdev *sd,
				     struct v4l2_dv_timings *timings)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	int ret;

	if (!timings)
		return -EINVAL;

	if (v4l2_match_dv_timings(&hdmi->timings, timings, 0, false))
		return 0;

	if (!v4l2_valid_dv_timings(timings, &adv748x_hdmi_timings_cap,
				   NULL, NULL))
		return -ERANGE;

	adv748x_fill_optional_dv_timings(timings);

	mutex_lock(&state->mutex);

	ret = adv748x_hdmi_set_video_timings(state, timings);
	if (ret)
		goto error;

	hdmi->timings = *timings;

	cp_clrset(state, ADV748X_CP_VID_ADJ_2, ADV748X_CP_VID_ADJ_2_INTERLACED,
		  timings->bt.interlaced ?
				  ADV748X_CP_VID_ADJ_2_INTERLACED : 0);

	mutex_unlock(&state->mutex);

	return 0;

error:
	mutex_unlock(&state->mutex);
	return ret;
}

static int adv748x_hdmi_g_dv_timings(struct v4l2_subdev *sd,
				     struct v4l2_dv_timings *timings)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);

	mutex_lock(&state->mutex);

	*timings = hdmi->timings;

	mutex_unlock(&state->mutex);

	return 0;
}

static int adv748x_hdmi_query_dv_timings(struct v4l2_subdev *sd,
					 struct v4l2_dv_timings *timings)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	struct v4l2_bt_timings *bt = &timings->bt;
	int pixelclock;
	int polarity;

	if (!timings)
		return -EINVAL;

	memset(timings, 0, sizeof(struct v4l2_dv_timings));

	if (!adv748x_hdmi_has_signal(state))
		return -ENOLINK;

	pixelclock = adv748x_hdmi_read_pixelclock(state);
	if (pixelclock < 0)
		return -ENODATA;

	timings->type = V4L2_DV_BT_656_1120;

	bt->pixelclock = pixelclock;
	bt->interlaced = hdmi_read(state, ADV748X_HDMI_F1H1) &
				ADV748X_HDMI_F1H1_INTERLACED ?
				V4L2_DV_INTERLACED : V4L2_DV_PROGRESSIVE;
	bt->width = hdmi_read16(state, ADV748X_HDMI_LW1,
				ADV748X_HDMI_LW1_WIDTH_MASK);
	bt->height = hdmi_read16(state, ADV748X_HDMI_F0H1,
				 ADV748X_HDMI_F0H1_HEIGHT_MASK);
	bt->hfrontporch = hdmi_read16(state, ADV748X_HDMI_HFRONT_PORCH,
				      ADV748X_HDMI_HFRONT_PORCH_MASK);
	bt->hsync = hdmi_read16(state, ADV748X_HDMI_HSYNC_WIDTH,
				ADV748X_HDMI_HSYNC_WIDTH_MASK);
	bt->hbackporch = hdmi_read16(state, ADV748X_HDMI_HBACK_PORCH,
				     ADV748X_HDMI_HBACK_PORCH_MASK);
	bt->vfrontporch = hdmi_read16(state, ADV748X_HDMI_VFRONT_PORCH,
				      ADV748X_HDMI_VFRONT_PORCH_MASK) / 2;
	bt->vsync = hdmi_read16(state, ADV748X_HDMI_VSYNC_WIDTH,
				ADV748X_HDMI_VSYNC_WIDTH_MASK) / 2;
	bt->vbackporch = hdmi_read16(state, ADV748X_HDMI_VBACK_PORCH,
				     ADV748X_HDMI_VBACK_PORCH_MASK) / 2;

	polarity = hdmi_read(state, 0x05);
	bt->polarities = (polarity & BIT(4) ? V4L2_DV_VSYNC_POS_POL : 0) |
		(polarity & BIT(5) ? V4L2_DV_HSYNC_POS_POL : 0);

	if (bt->interlaced == V4L2_DV_INTERLACED) {
		bt->height += hdmi_read16(state, 0x0b, 0x1fff);
		bt->il_vfrontporch = hdmi_read16(state, 0x2c, 0x3fff) / 2;
		bt->il_vsync = hdmi_read16(state, 0x30, 0x3fff) / 2;
		bt->il_vbackporch = hdmi_read16(state, 0x34, 0x3fff) / 2;
	}

	adv748x_fill_optional_dv_timings(timings);

	/*
	 * No interrupt handling is implemented yet.
	 * There should be an IRQ when a cable is plugged and the new timings
	 * should be figured out and stored to state.
	 */
	hdmi->timings = *timings;

	return 0;
}

static int adv748x_hdmi_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);

	mutex_lock(&state->mutex);

	*status = adv748x_hdmi_has_signal(state) ? 0 : V4L2_IN_ST_NO_SIGNAL;

	mutex_unlock(&state->mutex);

	return 0;
}

static int adv748x_hdmi_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	int ret;

	mutex_lock(&state->mutex);

	ret = adv748x_txa_power(state, enable);
	if (ret)
		goto done;

	if (adv748x_hdmi_has_signal(state))
		adv_dbg(state, "Detected HDMI signal\n");
	else
		adv_dbg(state, "Couldn't detect HDMI video signal\n");

done:
	mutex_unlock(&state->mutex);
	return ret;
}

static int adv748x_hdmi_g_pixelaspect(struct v4l2_subdev *sd,
				      struct v4l2_fract *aspect)
{
	aspect->numerator = 1;
	aspect->denominator = 1;

	return 0;
}

static const struct v4l2_subdev_video_ops adv748x_video_ops_hdmi = {
	.s_dv_timings = adv748x_hdmi_s_dv_timings,
	.g_dv_timings = adv748x_hdmi_g_dv_timings,
	.query_dv_timings = adv748x_hdmi_query_dv_timings,
	.g_input_status = adv748x_hdmi_g_input_status,
	.s_stream = adv748x_hdmi_s_stream,
	.g_pixelaspect = adv748x_hdmi_g_pixelaspect,
};

/* -----------------------------------------------------------------------------
 * v4l2_subdev_pad_ops
 */

static int adv748x_hdmi_propagate_pixelrate(struct adv748x_hdmi *hdmi)
{
	struct v4l2_subdev *tx;
	struct v4l2_dv_timings timings;

	tx = adv748x_get_remote_sd(&hdmi->pads[ADV748X_HDMI_SOURCE]);
	if (!tx)
		return -ENOLINK;

	adv748x_hdmi_query_dv_timings(&hdmi->sd, &timings);
	if (timings.bt.pixelclock == 0)
		return -EINVAL;

	return adv748x_csi2_set_pixelrate(tx, timings.bt.pixelclock);
}

static int adv748x_hdmi_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_RGB888_1X24;

	return 0;
}

static int adv748x_hdmi_get_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_format *sdformat)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct v4l2_mbus_framefmt *mbusformat;

	if (sdformat->pad != ADV748X_HDMI_SOURCE)
		return -EINVAL;

	if (sdformat->which == V4L2_SUBDEV_FORMAT_TRY) {
		mbusformat = v4l2_subdev_get_try_format(sd, cfg, sdformat->pad);
		sdformat->format = *mbusformat;
	} else {
		struct v4l2_dv_timings timings;

		adv748x_hdmi_query_dv_timings(&hdmi->sd, &timings);
		hdmi->timings = timings;

		adv748x_hdmi_fill_format(hdmi, &sdformat->format);
		adv748x_hdmi_propagate_pixelrate(hdmi);
	}

	return 0;
}

static int adv748x_hdmi_set_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_format *sdformat)
{
	struct v4l2_mbus_framefmt *mbusformat;

	if (sdformat->pad != ADV748X_HDMI_SOURCE)
		return -EINVAL;

	if (sdformat->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return adv748x_hdmi_get_format(sd, cfg, sdformat);

	mbusformat = v4l2_subdev_get_try_format(sd, cfg, sdformat->pad);
	*mbusformat = sdformat->format;

	return 0;
}

static int adv748x_hdmi_get_edid(struct v4l2_subdev *sd, struct v4l2_edid *edid)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);

	memset(edid->reserved, 0, sizeof(edid->reserved));

	if (!hdmi->edid.present)
		return -ENODATA;

	if (edid->start_block == 0 && edid->blocks == 0) {
		edid->blocks = hdmi->edid.blocks;
		return 0;
	}

	if (edid->start_block >= hdmi->edid.blocks)
		return -EINVAL;

	if (edid->start_block + edid->blocks > hdmi->edid.blocks)
		edid->blocks = hdmi->edid.blocks - edid->start_block;

	memcpy(edid->edid, hdmi->edid.edid + edid->start_block * 128,
			edid->blocks * 128);

	return 0;
}

static inline int adv748x_hdmi_edid_write_block(struct adv748x_hdmi *hdmi,
					unsigned int total_len, const u8 *val)
{
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	int err = 0;
	int i = 0;
	int len = 0;

	adv_dbg(state, "%s: write EDID block (%d byte)\n",
				__func__, total_len);

	while (!err && i < total_len) {
		len = (total_len - i) > I2C_SMBUS_BLOCK_MAX ?
				I2C_SMBUS_BLOCK_MAX :
				(total_len - i);

		err = adv748x_write_block(state, ADV748X_PAGE_EDID,
				i, val + i, len);
		i += len;
	}

	return err;
}

static const __u8 g_edid_data[256] = {
	/* Header information(0-19th byte) */
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
		0x04, 0x89, /* manufacturer ID (ADI) */
		0x80, 0x74, /* product code, 7480 */
		0x00, 0x00, 0x00, 0x00, /* serial number */
		0x2b, /* week of manufacture */
		0x18, /* Model year (1990 + 0x18 = 2014) */
		0x01, 0x03, /* EDID revision (1.3) */
	/* Basic display parameters(20-24th byte) */
		0x80, 0x31, 0x1c, 0xa0, 0x0a,
	/* Chromaticity coordinates(25-34th byte) */
		0xaa, 0x33, 0xa4, 0x55, 0x48, 0x93, 0x25, 0x10, 0x45, 0x47,
	/* Established timing bitmap(35-37th byte) */
		0x20, 0x00, 0x00,
	/* Standard timing information(38-53th byte) */
	/* Because they are unused, in this field, all values are 0101h. */
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	/* Descriptor blocks of Descriptor 1(54-71th byte) */
		0x8c, 0x0a, 0xd0, 0x8a, 0x20, 0xe0, 0x2d, 0x10, 0x10,
		0x3e, 0x96, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x18,
	/* Descriptor blocks of Descriptor 2(72-89th byte) */
		0xd8, 0x09, 0x80, 0xa0, 0x20, 0xe0, 0x2d, 0x10, 0x10,
		0x60, 0xa2, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x18,
	/* Descriptor blocks of Descriptor 3(90-107th byte) */
		0x00, 0x00, 0x00, 0xfc, 0x00,
		0x41, 0x44, 0x56, 0x37, 0x34, 0x38, 0x78, 0x0a, /* Monitor name, terminated by 0x0a */
		0x20, 0x20, 0x20, 0x20, 0x20, /* padding */
	/* Descriptor blocks of Descriptor 4(108-125th byte) */
		0x00, 0x00, 0x00, 0xfd, /* display range limits descriptor */
		0x00, /* offsets (all 0) */
		0x18, 0x4b, /* vertical rate, Hz */
		0x0f, 0x6f, /* horizontal rate, KHz */
		0x10, /* max pixel clock rate, multiple of 10 MHz */
		/*
		 * pixclk rate set to 160MHz because of the format limitation.
		 * The device itself is capable of up to 165MHz.
		 */
		0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	/* Number of extensions to follow(126th byte) */
		0x01,
	/* Checksum(127th byte) */
		0x59,
	/* CEA EDID Timing Extension Version 3 */
		0x02,	/* extension tag */
		0x03,	/* revision number */
		0x1f,	/* block offset to the 18 byte DTDs */
		0x40,	/* v2+ info and number of native DTDs present (bit3:0) */
	 /* Video data block: */
		0x48,
		0x10,	 /* 1920x1080p @ 59.94/60 Hz */
		0x05,	 /* 1920x1080i @ 59.94/60 Hz */
		0x04,	 /* 1280x720p @ 59.94/60 Hz */
		0x01,	 /* 640x480p @ 59.94/60 Hz */
		0x02,	 /* 720x480p @ 59.94/60 Hz */
		0x06,	 /* 720(1440)x480i @ 59.94/60 Hz */
		0x15,	 /* 720(1440)x576i @ 50 Hz */
		0x11,	 /* 720x576p @ 50 Hz */
	/* Audio data block: */
		0x26,
		/*
		 * The first block is specifically for broken hardware which
		 * analyses only the first format. It forces the 8x24 format.
		 */
		0x0f,	/* Format and number of channels (L-PCM, 8ch) */
		0x04,	/* Sampling frequencies (48kHz) */
		0x04,	/* Sample size for L-PCM (24bit), bit rate divided by 8000 for other formats */
		0x09,	/* L-PCM, 2ch */
		0x7f,	/* 192 176.4 96 88.2 48 44.1 32 */
		0x05,	/* 24 and 16 bits */
	/* Speaker allocation block: */
		0x83,
		0x7F,	/* assume all speakers */
		0x00,
		0x00,
	/* Vendor specific data block (OUI 000c03 (HDMI), 1.0.0.0, AI */
		0x66, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x80,
		/* 18 byte DTDs (none defined, padding only) */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x3b, /* checksum */
};

static int adv748x_hdmi_set_edid(struct v4l2_subdev *sd, struct v4l2_edid *edid)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	int err;

	memset(edid->reserved, 0, sizeof(edid->reserved));

	if (edid->start_block != 0)
		return -EINVAL;

	if (edid->blocks == 0) {
		hdmi->edid.blocks = 0;
		hdmi->edid.present = 0;

		/* Fall back to a 16:9 aspect ratio */
		hdmi->aspect_ratio.numerator = 16;
		hdmi->aspect_ratio.denominator = 9;

		/* Disable the EDID */
		repeater_write(state, ADV748X_REPEATER_EDID_SZ,
			       edid->blocks << ADV748X_REPEATER_EDID_SZ_SHIFT);

		repeater_write(state, ADV748X_REPEATER_EDID_CTL, 0);

		return 0;
	}

	if (edid->blocks > 4) {
		edid->blocks = 4;
		return -E2BIG;
	}

	memcpy(hdmi->edid.edid, edid->edid, 128 * edid->blocks);
	hdmi->edid.blocks = edid->blocks;
	hdmi->edid.present = true;

	hdmi->aspect_ratio = v4l2_calc_aspect_ratio(edid->edid[0x15],
			edid->edid[0x16]);

	err = adv748x_hdmi_edid_write_block(hdmi, 128 * edid->blocks,
			hdmi->edid.edid);
	if (err < 0) {
		v4l2_err(sd, "error %d writing edid pad %d\n", err, edid->pad);
		return err;
	}

	repeater_write(state, ADV748X_REPEATER_EDID_SZ,
		       edid->blocks << ADV748X_REPEATER_EDID_SZ_SHIFT);

	repeater_write(state, ADV748X_REPEATER_EDID_CTL,
		       ADV748X_REPEATER_EDID_CTL_EN);

	return 0;
}

int adv748x_hdmi_set_resume_edid(struct adv748x_hdmi *hdmi)
{
	struct v4l2_edid g_edid;
	int err;

	g_edid.pad = 0;
	g_edid.start_block = 0;
	g_edid.blocks = 2;
	g_edid.edid = (__u8 *)g_edid_data;

	err = adv748x_hdmi_set_edid(&hdmi->sd, &g_edid);
	if (err < 0) {
		v4l2_err(&hdmi->sd, "edid set error %d\n", err);
		return err;
	}

	return 0;
}

static bool adv748x_hdmi_check_dv_timings(const struct v4l2_dv_timings *timings,
					  void *hdl)
{
	const struct adv748x_hdmi_video_standards *stds =
		adv748x_hdmi_video_standards;
	unsigned int i;

	for (i = 0; stds[i].timings.bt.width; i++)
		if (v4l2_match_dv_timings(timings, &stds[i].timings, 0, false))
			return true;

	return false;
}

static int adv748x_hdmi_enum_dv_timings(struct v4l2_subdev *sd,
					struct v4l2_enum_dv_timings *timings)
{
	return v4l2_enum_dv_timings_cap(timings, &adv748x_hdmi_timings_cap,
					adv748x_hdmi_check_dv_timings, NULL);
}

static int adv748x_hdmi_dv_timings_cap(struct v4l2_subdev *sd,
				       struct v4l2_dv_timings_cap *cap)
{
	*cap = adv748x_hdmi_timings_cap;
	return 0;
}

static const struct v4l2_subdev_pad_ops adv748x_pad_ops_hdmi = {
	.enum_mbus_code = adv748x_hdmi_enum_mbus_code,
	.set_fmt = adv748x_hdmi_set_format,
	.get_fmt = adv748x_hdmi_get_format,
	.get_edid = adv748x_hdmi_get_edid,
	.set_edid = adv748x_hdmi_set_edid,
	.dv_timings_cap = adv748x_hdmi_dv_timings_cap,
	.enum_dv_timings = adv748x_hdmi_enum_dv_timings,
};

static int adv748x_hdmi_audio_mute(struct adv748x_hdmi *hdmi, int enable)
{
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);

	v4l2_dbg(0, 0, &hdmi->sd, "audio %smute (%d)\n", enable ? "" : "de", enable);
	return hdmi_update(state, ADV748X_HDMI_MUTE_CTRL,
			   ADV748X_HDMI_MUTE_CTRL_MUTE_AUDIO,
			   enable ? 0xff : 0);
}

struct tmds_params
{
	u32 cts, n;
	u16 tmdsfreq, tmdsfreq_frac;
};

static int adv748x_hdmi_log_status(struct v4l2_subdev *sd)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	u8 rv, i2s_tdm_mode_enable;
	u8 cts_n[5];
	u8 cs_data[0x3a - 0x36 + 1];
	u8 tmdsfreq[2]; /* both tmdsfreq and tmdsfreq_frac */
	struct tmds_params tmds_params;

	/* Audio control and configuration */
	rv = io_read(state, 0x71);
	pr_info("cable_det_a_raw         %s\n", rv & BIT(6) ? "detected" : "no cable");
	pr_info("tmds_clk_a_raw          %s\n", rv & BIT(3) ? "detected" : "no TMDS clock");
	pr_info("tmdspll_lck_a_raw       %s\n", rv & BIT(7) ? "locked to incoming clock" : "not locked");
	pr_info("hdmi_encrpt_a_raw       %s\n", rv & BIT(5) ? "current frame encrypted" : "not encrypted");
	rv = hdmi_read(state, 0x04);
	pr_info("audio_pll_locked        0x%02lx\n", rv & BIT(0));
	pr_info("tmds_pll_locked         0x%02lx\n", rv & BIT(1));
	rv = io_read(state, 0x6c);
	pr_info("gamut_mdata_raw         %s\n", rv & BIT(0) ? "received" : "-");
	pr_info("audio_c_pckt_raw        %s\n", rv & BIT(1) ? "ACR received" : "-");
	pr_info("gen_ctl_pckt_raw        %s\n", rv & BIT(2) ? "received" : "-");
	pr_info("hdmi_mode_raw           %s\n", rv & BIT(3) ? "HDMI/MHL" : "-");
	pr_info("audio_ch_md_raw         %s\n", rv & BIT(4) ? "multichannel" : "-");
	pr_info("av_mute_raw             %s\n", rv & BIT(5) ? "received" : "-");
	pr_info("internal_mute_raw       %s\n", rv & BIT(6) ? "asserted" : "-");
	pr_info("cs_data_valid_raw       %s\n", rv & BIT(7) ? "valid" : "-");
	rv = hdmi_read(state, 0x6d);
	pr_info("i2s_tdm_mode_enable     %s\n", rv & BIT(7) ? "TDM (multichannel)" : "I2S (stereo)");
	i2s_tdm_mode_enable = rv & BIT(7);

	/* i2s_tdm_mode_enable must be unset */
	if (adv748x_read_block(state, ADV748X_PAGE_HDMI, 0x36, cs_data, ARRAY_SIZE(cs_data)) == 0) {
		pr_info("... cs_data %s\n", cs_data[0] & BIT(0) ? "pro" : "consumer");
		pr_info("... cs_data %s\n", cs_data[0] & BIT(1) ? "other" : "L-PCM");
		pr_info("... cs_data %s\n", cs_data[0] & BIT(2) ? "no copyright" : "copyright asserted");
		pr_info("... cs_data %s (%lu)\n", cs_data[0] & GENMASK(5, 3) ? "50/15" : "no pre-emphasis",
			(cs_data[0] & GENMASK(5, 3)) >> 4);
		pr_info("... cs_data channels status mode %lu\n",
			(cs_data[0] & GENMASK(7, 6)) >> 7);
		pr_info("... cs_data category code 0x%02x\n", cs_data[1]);
		pr_info("... cs_data source number %u\n", cs_data[2] & 0xf);
		pr_info("... cs_data channel number %u\n", (cs_data[2] & 0xf0) >> 4);
		pr_info("... cs_data sampling frequency %s (%u)\n",
			({
			 const char *s;
			 switch (cs_data[3] & 0xf) {
			 case 0: s = "44.1"; break;
			 case 2: s = "48"; break;
			 case 3: s = "32"; break;
			 case 8: s = "88.2"; break;
			 case 10: s = "96"; break;
			 case 12: s = "176"; break;
			 case 14: s = "192"; break;
			 default: s = "reserved"; break;
			 }
			 s;
			 }), cs_data[3] & 0xf);
		pr_info("... cs_data clock accuracy %s\n",
			({
			 const char *s;
			 switch (cs_data[3] & 0x30) {
			 case 0: s = "Level II"; break;
			 case 1: s = "Level I"; break;
			 case 2: s = "Level III, variable pitch shifted"; break;
			 default: s = "reserved";
			 }
			 s;
			 }));
	}
	rv = hdmi_read(state, ADV748X_HDMI_I2S);
	pr_info("i2soutmode              %s\n", ({
					 const char *r = "";
					 switch (rv & ADV748X_HDMI_I2SOUTMODE_MASK) {
					 case 0 << ADV748X_HDMI_I2SOUTMODE_SHIFT: r = "I2S"; break;
					 case 1 << ADV748X_HDMI_I2SOUTMODE_SHIFT: r = "right"; break;
					 case 2 << ADV748X_HDMI_I2SOUTMODE_SHIFT: r = "left"; break;
					 case 3 << ADV748X_HDMI_I2SOUTMODE_SHIFT: r = "spdif"; break;
					 }
					 r; }));
	pr_info("i2sbitwidth             %u\n", rv & 0x1fu);
	rv = hdmi_read(state, 0x05);
	pr_info("hdmi_mode               %s\n", rv & BIT(7) ? "HDMI" : "DVI");
	rv = hdmi_read(state, 0x07);
	pr_info("audio_channel_mode      %s\n", rv & BIT(6) ? "multichannel" : "stereo or compressed");
	rv = hdmi_read(state, 0x0f);
	pr_info("man_audio_dl_bypass     0x%02lx\n", rv & BIT(7)); /* must be 1 if tdm */
	pr_info("audio_delay_line_bypass 0x%02lx\n", rv & BIT(6)); /* must be 1 if tdm */
	rv = hdmi_read(state, 0x6e);
	pr_info("mux_spdif_to_i2s_enable %s\n", rv & BIT(3) ? "SPDIF" : "I2S");
	rv = dpll_read(state, ADV748X_DPLL_MCLK_FS);
	pr_info("mclk_fs_n               %lu\n", ((rv & ADV748X_DPLL_MCLK_FS_N_MASK) + 1) * 128);

	/* i2s_tdm_mode_enable must be set */
	memset(&tmds_params, 0, sizeof(tmds_params));
	if (adv748x_read_block(state, ADV748X_PAGE_HDMI, 0x5b, cts_n, 5) == 0) {
		tmds_params.cts  = cts_n[0] << 12;
		tmds_params.cts |= cts_n[1] << 4;
		tmds_params.cts |= cts_n[2] >> 4;
		tmds_params.n  = (cts_n[2] & 0xf) << 16;
		tmds_params.n |= cts_n[3] << 8;
		tmds_params.n |= cts_n[4];
		pr_info("... TDM: ACR cts  %u\n", tmds_params.cts);
		pr_info("... TDM: ACR n    %u\n", tmds_params.n);
	}
	if (adv748x_read_block(state, ADV748X_PAGE_HDMI, 0x51, tmdsfreq, 2) == 0) {
		tmds_params.tmdsfreq  = tmdsfreq[0] << 1;
		tmds_params.tmdsfreq |= tmdsfreq[1] >> 7;
		tmds_params.tmdsfreq_frac = tmdsfreq[1] & 0x7f;
		pr_info("... TDM: tmdsfreq       %d MHz\n", tmds_params.tmdsfreq);
		pr_info("... TDM: tmdsfreq_frac  %d 1/128\n", tmds_params.tmdsfreq_frac);
	}
	if (i2s_tdm_mode_enable) {
		pr_info("... TDM: sampling frequency %u Hz\n",
			tmds_params.cts ?
			(tmds_params.tmdsfreq * tmds_params.n +
			 tmds_params.tmdsfreq_frac * tmds_params.n / 128) *
			1000 / (128 * tmds_params.cts / 1000) :
			UINT_MAX);
	}
	return 0;
}

static int adv748x_hdmi_enumaudout(struct adv748x_hdmi *hdmi, struct v4l2_audioout *a)
{
	switch (a->index) {
	case HDMI_AOUT_NONE:
		strlcpy(a->name, "None", sizeof(a->name));
		break;
	case HDMI_AOUT_I2S:
		strlcpy(a->name, "I2S/stereo", sizeof(a->name));
		break;
	case HDMI_AOUT_I2S_TDM:
		strlcpy(a->name, "I2S-TDM/multichannel", sizeof(a->name));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int adv748x_hdmi_g_audout(struct adv748x_hdmi *hdmi, struct v4l2_audioout *a)
{
	a->index = hdmi->audio_out;
	return adv748x_hdmi_enumaudout(hdmi, a);
}

static int set_audio_pads_state(struct adv748x_state *state, int on)
{
	v4l2_dbg(0, 0, &state->hdmi.sd, "set audio pads %s\n", on ? "on" : "off");
	return io_update(state, ADV748X_IO_PAD_CONTROLS,
			 ADV748X_IO_PAD_CONTROLS_TRI_AUD | ADV748X_IO_PAD_CONTROLS_PDN_AUD,
			 on ? 0 : 0xff);
}

static int set_dpll_mclk_fs(struct adv748x_state *state, int fs)
{
	if (fs % 128 || fs > 768)
		return -EINVAL;
	return dpll_update(state, ADV748X_DPLL_MCLK_FS, ADV748X_DPLL_MCLK_FS_N_MASK, (fs / 128) - 1);
}

static int set_i2s_format(struct adv748x_state *state, uint outmode, uint bitwidth)
{
	return hdmi_update(state, ADV748X_HDMI_I2S,
			   ADV748X_HDMI_I2SBITWIDTH_MASK | ADV748X_HDMI_I2SOUTMODE_MASK,
			   (outmode << ADV748X_HDMI_I2SOUTMODE_SHIFT) | bitwidth);
}

static int set_i2s_tdm_mode(struct adv748x_state *state, int is_tdm)
{
	int ret;

	ret = hdmi_update(state, ADV748X_HDMI_AUDIO_MUTE_SPEED,
			  ADV748X_MAN_AUDIO_DL_BYPASS | ADV748X_AUDIO_DELAY_LINE_BYPASS,
			  is_tdm ? 0xff : 0);
	if (ret < 0)
		goto fail;
	ret = hdmi_update(state, ADV748X_HDMI_REG_6D, ADV748X_I2S_TDM_MODE_ENABLE,
			  is_tdm ? 0xff : 0);
	if (ret < 0)
		goto fail;
	ret = set_i2s_format(state, ADV748X_I2SOUTMODE_LEFT_J, 24);
fail:
	return ret;
}

static int set_audio_out(struct adv748x_state *state, int aout)
{
	int ret;

	switch (aout) {
	case HDMI_AOUT_NONE:
		v4l2_dbg(0, 0, &state->hdmi.sd, "selecting no audio\n");
		ret = set_audio_pads_state(state, 0);
		break;
	case HDMI_AOUT_I2S:
		v4l2_dbg(0, 0, &state->hdmi.sd, "selecting I2S audio\n");
		ret = set_dpll_mclk_fs(state, 256);
		if (ret < 0)
			goto fail;
		ret = set_i2s_tdm_mode(state, 1);
		if (ret < 0)
			goto fail;
		ret = set_audio_pads_state(state, 1);
		if (ret < 0)
			goto fail;
		break;
	case HDMI_AOUT_I2S_TDM:
		v4l2_dbg(0, 0, &state->hdmi.sd, "selecting I2S/TDM audio\n");
		ret = set_dpll_mclk_fs(state, 256);
		if (ret < 0)
			goto fail;
		ret = set_i2s_tdm_mode(state, 1);
		if (ret < 0)
			goto fail;
		ret = set_audio_pads_state(state, 1);
		if (ret < 0)
			goto fail;
		break;
	default:
		ret = -EINVAL;
		goto fail;
	}
	return 0;
fail:
	return ret;
}

static int adv748x_hdmi_s_audout(struct adv748x_hdmi *hdmi, const struct v4l2_audioout *a)
{
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	int ret = set_audio_out(state, a->index);

	if (ret == 0)
		hdmi->audio_out = a->index;
	return ret;
}

static long adv748x_hdmi_querycap(struct adv748x_hdmi *hdmi, struct v4l2_capability *cap)
{
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);

	cap->version = LINUX_VERSION_CODE;
	strlcpy(cap->driver, state->dev->driver->name, sizeof(cap->driver));
	strlcpy(cap->card, "hdmi", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "i2c:%d-%04x",
		 i2c_adapter_id(state->client->adapter),
		 state->client->addr);
	cap->device_caps = V4L2_CAP_AUDIO | V4L2_CAP_VIDEO_CAPTURE;
	cap->capabilities = V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static long adv748x_hdmi_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct adv748x_hdmi *hdmi = adv748x_sd_to_hdmi(sd);

	switch (cmd) {
	case VIDIOC_ENUMAUDOUT:
		return adv748x_hdmi_enumaudout(hdmi, arg);
	case VIDIOC_S_AUDOUT:
		return adv748x_hdmi_s_audout(hdmi, arg);
	case VIDIOC_G_AUDOUT:
		return adv748x_hdmi_g_audout(hdmi, arg);
	case VIDIOC_QUERYCAP:
		return adv748x_hdmi_querycap(hdmi, arg);
	}
	return -ENOTTY;
}

static const struct v4l2_subdev_core_ops adv748x_core_ops_hdmi = {
	.log_status = adv748x_hdmi_log_status,
	.ioctl = adv748x_hdmi_ioctl,
};

/* -----------------------------------------------------------------------------
 * v4l2_subdev_ops
 */

static const struct v4l2_subdev_ops adv748x_ops_hdmi = {
	.core = &adv748x_core_ops_hdmi,
	.video = &adv748x_video_ops_hdmi,
	.pad = &adv748x_pad_ops_hdmi,
};

/* -----------------------------------------------------------------------------
 * Controls
 */

static const char * const hdmi_ctrl_patgen_menu[] = {
	"Disabled",
	"Solid Color",
	"Color Bars",
	"Ramp Grey",
	"Ramp Blue",
	"Ramp Red",
	"Checkered"
};

static int adv748x_hdmi_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct adv748x_hdmi *hdmi = adv748x_ctrl_to_hdmi(ctrl);
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	int ret;
	u8 pattern;

	if (ctrl->id == V4L2_CID_AUDIO_MUTE)
		return adv748x_hdmi_audio_mute(hdmi, ctrl->val);
	/* Enable video adjustment first */
	ret = cp_clrset(state, ADV748X_CP_VID_ADJ,
			ADV748X_CP_VID_ADJ_ENABLE,
			ADV748X_CP_VID_ADJ_ENABLE);
	if (ret < 0)
		return ret;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		ret = cp_write(state, ADV748X_CP_BRI, ctrl->val);
		break;
	case V4L2_CID_HUE:
		ret = cp_write(state, ADV748X_CP_HUE, ctrl->val);
		break;
	case V4L2_CID_CONTRAST:
		ret = cp_write(state, ADV748X_CP_CON, ctrl->val);
		break;
	case V4L2_CID_SATURATION:
		ret = cp_write(state, ADV748X_CP_SAT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		pattern = ctrl->val;

		/* Pattern is 0-indexed. Ctrl Menu is 1-indexed */
		if (pattern) {
			pattern--;
			pattern |= ADV748X_CP_PAT_GEN_EN;
		}

		ret = cp_write(state, ADV748X_CP_PAT_GEN, pattern);

		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static const struct v4l2_ctrl_ops adv748x_hdmi_ctrl_ops = {
	.s_ctrl = adv748x_hdmi_s_ctrl,
};

static int adv748x_hdmi_init_controls(struct adv748x_hdmi *hdmi)
{
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);

	v4l2_ctrl_handler_init(&hdmi->ctrl_hdl, 5);

	/* Use our mutex for the controls */
	hdmi->ctrl_hdl.lock = &state->mutex;

	v4l2_ctrl_new_std(&hdmi->ctrl_hdl, &adv748x_hdmi_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, ADV748X_CP_BRI_MIN,
			  ADV748X_CP_BRI_MAX, 1, ADV748X_CP_BRI_DEF);
	v4l2_ctrl_new_std(&hdmi->ctrl_hdl, &adv748x_hdmi_ctrl_ops,
			  V4L2_CID_CONTRAST, ADV748X_CP_CON_MIN,
			  ADV748X_CP_CON_MAX, 1, ADV748X_CP_CON_DEF);
	v4l2_ctrl_new_std(&hdmi->ctrl_hdl, &adv748x_hdmi_ctrl_ops,
			  V4L2_CID_SATURATION, ADV748X_CP_SAT_MIN,
			  ADV748X_CP_SAT_MAX, 1, ADV748X_CP_SAT_DEF);
	v4l2_ctrl_new_std(&hdmi->ctrl_hdl, &adv748x_hdmi_ctrl_ops,
			  V4L2_CID_HUE, ADV748X_CP_HUE_MIN,
			  ADV748X_CP_HUE_MAX, 1, ADV748X_CP_HUE_DEF);
	v4l2_ctrl_new_std(&hdmi->ctrl_hdl, &adv748x_hdmi_ctrl_ops,
			  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 1);

	/*
	 * Todo: V4L2_CID_DV_RX_POWER_PRESENT should also be supported when
	 * interrupts are handled correctly
	 */

	v4l2_ctrl_new_std_menu_items(&hdmi->ctrl_hdl, &adv748x_hdmi_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hdmi_ctrl_patgen_menu) - 1,
				     0, 0, hdmi_ctrl_patgen_menu);

	hdmi->sd.ctrl_handler = &hdmi->ctrl_hdl;
	if (hdmi->ctrl_hdl.error) {
		v4l2_ctrl_handler_free(&hdmi->ctrl_hdl);
		return hdmi->ctrl_hdl.error;
	}

	return v4l2_ctrl_handler_setup(&hdmi->ctrl_hdl);
}

int adv748x_hdmi_init(struct adv748x_hdmi *hdmi)
{
	struct adv748x_state *state = adv748x_hdmi_to_state(hdmi);
	static const struct v4l2_dv_timings cea1280x720 =
		V4L2_DV_BT_CEA_1280X720P30;
	int ret;
	int err;
	struct v4l2_edid g_edid;

	hdmi->timings = cea1280x720;

	/* Initialise a default 16:9 aspect ratio */
	hdmi->aspect_ratio.numerator = 16;
	hdmi->aspect_ratio.denominator = 9;

	adv748x_subdev_init(&hdmi->sd, state, &adv748x_ops_hdmi,
			    MEDIA_ENT_F_IO_DTV, "hdmi");

	hdmi->pads[ADV748X_HDMI_SINK].flags = MEDIA_PAD_FL_SINK;
	hdmi->pads[ADV748X_HDMI_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&hdmi->sd.entity,
				     ADV748X_HDMI_NR_PADS, hdmi->pads);
	if (ret)
		return ret;

	ret = adv748x_hdmi_init_controls(hdmi);
	if (ret)
		goto err_free_media;

	g_edid.pad = 0;
	g_edid.start_block = 0;
	g_edid.blocks = 2;
	g_edid.edid = (__u8 *)g_edid_data;
	err = adv748x_hdmi_set_edid(&hdmi->sd, &g_edid);
	if (err < 0) {
		v4l2_err(&hdmi->sd, "edid set error %d\n", err);
		return err;
	}

	hdmi->audio_out = default_audio_out;
	if (hdmi->audio_out != HDMI_AOUT_NONE) {
		err = set_audio_out(state, default_audio_out);
		if (err)
			v4l2_err(&hdmi->sd, "selecting audio output error %d\n", err);
		else {
			struct v4l2_ctrl *mute;

			mute = v4l2_ctrl_find(&hdmi->ctrl_hdl, V4L2_CID_AUDIO_MUTE);
			if (mute) {
				err = v4l2_ctrl_s_ctrl(mute, 0);
				if (err)
					v4l2_err(&hdmi->sd, "demuting audio error %d\n", err);
			}
		}
	}
	return 0;

err_free_media:
	media_entity_cleanup(&hdmi->sd.entity);

	return ret;
}

void adv748x_hdmi_cleanup(struct adv748x_hdmi *hdmi)
{
	adv748x_hdmi_audio_mute(hdmi, 1);
	set_audio_out(adv748x_hdmi_to_state(hdmi), HDMI_AOUT_NONE);
	v4l2_device_unregister_subdev(&hdmi->sd);
	media_entity_cleanup(&hdmi->sd.entity);
	v4l2_ctrl_handler_free(&hdmi->ctrl_hdl);
}
