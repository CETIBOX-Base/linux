/*
 * Driver for Analog Devices ADV748X HDMI receiver with AFE
 *
 * Copyright (C) 2017-2019 Renesas Electronics Corp.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * Authors:
 *	Koji Matsuoka <koji.matsuoka.xm@renesas.com>
 *	Niklas Söderlund <niklas.soderlund@ragnatech.se>
 *	Kieran Bingham <kieran.bingham@ideasonboard.com>
 */

#include "adv748x.h"

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/v4l2-dv-timings.h>

#include <media/v4l2-dv-timings.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fwnode.h>
#include <sound/soc.h>

/* -----------------------------------------------------------------------------
 * Register manipulation
 */

#define ADV748X_REGMAP_CONF(n) \
{ \
	.name = n, \
	.reg_bits = 8, \
	.val_bits = 8, \
	.max_register = 0xff, \
	.cache_type = REGCACHE_NONE, \
}

static const struct regmap_config adv748x_regmap_cnf[] = {
	ADV748X_REGMAP_CONF("io"),
	ADV748X_REGMAP_CONF("dpll"),
	ADV748X_REGMAP_CONF("cp"),
	ADV748X_REGMAP_CONF("hdmi"),
	ADV748X_REGMAP_CONF("edid"),
	ADV748X_REGMAP_CONF("repeater"),
	ADV748X_REGMAP_CONF("infoframe"),
	ADV748X_REGMAP_CONF("cbus"),
	ADV748X_REGMAP_CONF("cec"),
	ADV748X_REGMAP_CONF("sdp"),
	ADV748X_REGMAP_CONF("txa"),
	ADV748X_REGMAP_CONF("txb"),
};

static int adv748x_configure_regmap(struct adv748x_state *state, int region)
{
	int err;

	if (!state->i2c_clients[region])
		return -ENODEV;

	state->regmap[region] =
		devm_regmap_init_i2c(state->i2c_clients[region],
				     &adv748x_regmap_cnf[region]);

	if (IS_ERR(state->regmap[region])) {
		err = PTR_ERR(state->regmap[region]);
		adv_err(state,
			"Error initializing regmap %d with error %d\n",
			region, err);
		return -EINVAL;
	}

	return 0;
}
struct adv748x_register_map {
	const char *name;
	u8 default_addr;
};

static const struct adv748x_register_map adv748x_default_addresses[] = {
	[ADV748X_PAGE_IO] = { "main", 0x70 },
	[ADV748X_PAGE_DPLL] = { "dpll", 0x26 },
	[ADV748X_PAGE_CP] = { "cp", 0x22 },
	[ADV748X_PAGE_HDMI] = { "hdmi", 0x34 },
	[ADV748X_PAGE_EDID] = { "edid", 0x36 },
	[ADV748X_PAGE_REPEATER] = { "repeater", 0x32 },
	[ADV748X_PAGE_INFOFRAME] = { "infoframe", 0x31 },
	[ADV748X_PAGE_CBUS] = { "cbus", 0x30 },
	[ADV748X_PAGE_CEC] = { "cec", 0x41 },
	[ADV748X_PAGE_SDP] = { "sdp", 0x79 },
	[ADV748X_PAGE_TXB] = { "txb", 0x48 },
	[ADV748X_PAGE_TXA] = { "txa", 0x4a },
};

int adv748x_read_block(struct adv748x_state *state, u8 client_page, u8 reg, void *val, size_t reg_count)
{
	struct i2c_client *client = state->i2c_clients[client_page];
	int err;

	err = regmap_bulk_read(state->regmap[client_page], reg, val, reg_count);
	if (err) {
		adv_err(state, "error reading %02x, %02x-%02lx: %d\n",
				client->addr, reg, reg + reg_count - 1, err);
		return err;
	}
	adv_dbg(state, "read %s 0x%02x-0x%02x {%02x%c%02x%c%02x%c%02x%c%02x%c\n",
		adv748x_default_addresses[client_page].name, reg, reg + (uint)reg_count - 1,
		reg_count > 0 ?((const u8 *)val)[0] : 0, reg_count > 1 ? ' ' : '}',
		reg_count > 1 ?((const u8 *)val)[1] : 0, reg_count > 2 ? ' ' : reg_count < 2 ? ' ' : '}',
		reg_count > 2 ?((const u8 *)val)[2] : 0, reg_count > 3 ? ' ' : reg_count < 3 ? ' ' : '}',
		reg_count > 3 ?((const u8 *)val)[3] : 0, reg_count > 4 ? ' ' : reg_count < 4 ? ' ' : '}',
		reg_count > 4 ?((const u8 *)val)[4] : 0, reg_count > 5 ? '_' : reg_count < 5 ? ' ' : '}');
	return 0;
}

static int adv748x_read_check(struct adv748x_state *state,
			      int client_page, u8 reg)
{
	struct i2c_client *client = state->i2c_clients[client_page];
	int err;
	unsigned int val;

	err = regmap_read(state->regmap[client_page], reg, &val);

	if (err) {
		adv_err(state, "error reading %02x, %02x\n",
				client->addr, reg);
		return err;
	}
	adv_dbg(state, "read %s 0x%02x {%02x}\n",
		adv748x_default_addresses[client_page].name, reg, val);

	return val;
}

int adv748x_read(struct adv748x_state *state, u8 page, u8 reg)
{
	return adv748x_read_check(state, page, reg);
}

int adv748x_write(struct adv748x_state *state, u8 page, u8 reg, u8 value)
{
	adv_dbg(state, "write %s 0x%02x {%02x}\n",
		adv748x_default_addresses[page].name, reg, value);
	return regmap_write(state->regmap[page], reg, value);
}

int adv748x_update_bits(struct adv748x_state *state, u8 page, u8 reg, u8 mask,
			u8 value)
{
	return regmap_update_bits(state->regmap[page], reg, mask, value);
}

/* adv748x_write_block(): Write raw data with a maximum of I2C_SMBUS_BLOCK_MAX
 * size to one or more registers.
 *
 * A value of zero will be returned on success, a negative errno will
 * be returned in error cases.
 */
int adv748x_write_block(struct adv748x_state *state, int client_page,
			unsigned int init_reg, const void *val,
			size_t val_len)
{
	struct regmap *regmap = state->regmap[client_page];

	if (val_len > I2C_SMBUS_BLOCK_MAX)
		val_len = I2C_SMBUS_BLOCK_MAX;

	adv_dbg(state, "write %s 0x%02x-0x%02x {%02x%c%02x%c%02x%c%02x%c%02x%c\n",
		adv748x_default_addresses[client_page].name, init_reg, init_reg + (uint)val_len - 1,
		val_len > 0 ?((const u8 *)val)[0] : 0, val_len > 1 ? ' ' : '}',
		val_len > 1 ?((const u8 *)val)[1] : 0, val_len > 2 ? ' ' : '}',
		val_len > 2 ?((const u8 *)val)[2] : 0, val_len > 3 ? ' ' : '}',
		val_len > 3 ?((const u8 *)val)[3] : 0, val_len > 4 ? ' ' : '}',
		val_len > 4 ?((const u8 *)val)[4] : 0, val_len > 5 ? '_' : '}');
	return regmap_raw_write(regmap, init_reg, val, val_len);
}

static int adv748x_set_slave_addresses(struct adv748x_state *state)
{
	struct i2c_client *client;
	unsigned int i;
	u8 io_reg;

	for (i = ADV748X_PAGE_DPLL; i < ADV748X_PAGE_MAX; ++i) {
		io_reg = ADV748X_IO_SLAVE_ADDR_BASE + i;
		client = state->i2c_clients[i];

		io_write(state, io_reg, client->addr << 1);
	}

	return 0;
}

static void adv748x_unregister_clients(struct adv748x_state *state)
{
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(state->i2c_clients); ++i) {
		if (state->i2c_clients[i])
			i2c_unregister_device(state->i2c_clients[i]);
	}
}

static int adv748x_initialise_clients(struct adv748x_state *state)
{
	unsigned int i;
	int ret;

	for (i = ADV748X_PAGE_DPLL; i < ADV748X_PAGE_MAX; ++i) {
		state->i2c_clients[i] = i2c_new_secondary_device(
				state->client,
				adv748x_default_addresses[i].name,
				adv748x_default_addresses[i].default_addr);

		if (state->i2c_clients[i] == NULL) {
			adv_err(state, "failed to create i2c client %u\n", i);
			return -ENOMEM;
		}

		ret = adv748x_configure_regmap(state, i);
		if (ret)
			return ret;
	}

	return adv748x_set_slave_addresses(state);
}

/**
 * struct adv748x_reg_value - Register write instruction
 * @page:		Regmap page identifier
 * @reg:		I2C register
 * @value:		value to write to @page at @reg
 */
struct adv748x_reg_value {
	u8 page;
	u8 reg;
	u8 value;
};

static int adv748x_write_regs(struct adv748x_state *state,
			      const struct adv748x_reg_value *regs)
{
	int ret;

	while (regs->page != ADV748X_PAGE_EOR) {
		if (regs->page == ADV748X_PAGE_WAIT) {
			usleep_range(regs->value * 1000,
				     (regs->value * 1000) + 1000);
		} else {
			ret = adv748x_write(state, regs->page, regs->reg,
				      regs->value);
			if (ret < 0) {
				adv_err(state,
					"Error regs page: 0x%02x reg: 0x%02x\n",
					regs->page, regs->reg);
				return ret;
			}
		}
		regs++;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * TXA and TXB
 */

static const struct adv748x_reg_value adv748x_power_up_txa_4lane[] = {

	{ADV748X_PAGE_TXA, 0x00, 0x84},	/* Enable 4-lane MIPI */
	{ADV748X_PAGE_TXA, 0x00, 0xa4},	/* Set Auto DPHY Timing */

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_WAIT, 0x00, 0x02},/* delay 2 */
	{ADV748X_PAGE_TXA, 0x00, 0x24 },/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_up_txa_2lane[] = {

	{ADV748X_PAGE_TXA, 0x00, 0x82},	/* Enable 2-lane MIPI */
	{ADV748X_PAGE_TXA, 0x00, 0xa2},	/* Set Auto DPHY Timing */

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_WAIT, 0x00, 0x02},/* delay 2 */
	{ADV748X_PAGE_TXA, 0x00, 0x22},	/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_up_txa_1lane[] = {

	{ADV748X_PAGE_TXA, 0x00, 0x81},	/* Enable 1-lane MIPI */
	{ADV748X_PAGE_TXA, 0x00, 0xa1},	/* Set Auto DPHY Timing */

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_WAIT, 0x00, 0x02},/* delay 2 */
	{ADV748X_PAGE_TXA, 0x00, 0x21},	/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_down_txa_4lane[] = {

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x00, 0x84},	/* Enable 4-lane MIPI */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x3b},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_down_txa_2lane[] = {

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x00, 0x82},	/* Enable 2-lane MIPI */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x3b},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_down_txa_1lane[] = {

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x00, 0x81},	/* Enable 1-lane MIPI */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x3b},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_up_txb_1lane[] = {

	{ADV748X_PAGE_TXB, 0x00, 0x81},	/* Enable 1-lane MIPI */
	{ADV748X_PAGE_TXB, 0x00, 0xa1},	/* Set Auto DPHY Timing */

	{ADV748X_PAGE_TXB, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_WAIT, 0x00, 0x02},/* delay 2 */
	{ADV748X_PAGE_TXB, 0x00, 0x21 },/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXB, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXB, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_power_down_txb_1lane[] = {

	{ADV748X_PAGE_TXB, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0x1e, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0x00, 0x81},	/* Enable 4-lane MIPI */
	{ADV748X_PAGE_TXB, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_TXB, 0xc1, 0x3b},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

int adv748x_txa_power(struct adv748x_state *state, bool on)
{
	int val;
	const struct adv748x_reg_value *txa_on;
	const struct adv748x_reg_value *txa_off;

	val = txa_read(state, ADV748X_CSI_FS_AS_LS);
	if (val < 0)
		return val;

	/*
	 * This test against BIT(6) is not documented by the datasheet, but was
	 * specified in the downstream driver.
	 * Track with a WARN_ONCE to determine if it is ever set by HW.
	 */
	WARN_ONCE((on && val & ADV748X_CSI_FS_AS_LS_UNKNOWN),
			"Enabling with unknown bit set");

	/* Use the following processing at both hdmi and afe */
	if (on) {
		if (state->hdmi.use_lane == 1)
			txa_on = adv748x_power_up_txa_1lane;
		else if (state->hdmi.use_lane == 2)
			txa_on = adv748x_power_up_txa_2lane;
		else
			txa_on = adv748x_power_up_txa_4lane;

		return adv748x_write_regs(state, txa_on);
	}

	if (state->hdmi.use_lane == 1)
		txa_off = adv748x_power_down_txa_1lane;
	else if (state->hdmi.use_lane == 2)
		txa_off = adv748x_power_down_txa_2lane;
	else
		txa_off = adv748x_power_down_txa_4lane;

	return adv748x_write_regs(state, txa_off);
}

int adv748x_txb_power(struct adv748x_state *state, bool on)
{
	int val;

	val = txb_read(state, ADV748X_CSI_FS_AS_LS);
	if (val < 0)
		return val;

	/*
	 * This test against BIT(6) is not documented by the datasheet, but was
	 * specified in the downstream driver.
	 * Track with a WARN_ONCE to determine if it is ever set by HW.
	 */
	WARN_ONCE((on && val & ADV748X_CSI_FS_AS_LS_UNKNOWN),
			"Enabling with unknown bit set");

	if (on)
		return adv748x_write_regs(state, adv748x_power_up_txb_1lane);

	return adv748x_write_regs(state, adv748x_power_down_txb_1lane);
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations adv748x_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * HW setup
 */

static const struct adv748x_reg_value adv748x_sw_reset[] = {

	{ADV748X_PAGE_IO, 0xff, 0xff},	/* SW reset */
	{ADV748X_PAGE_WAIT, 0x00, 0x05},/* delay 5 */
	{ADV748X_PAGE_IO, 0x01, 0x76},	/* ADI Required Write */
	{ADV748X_PAGE_IO, 0xf2, 0x01},	/* Enable I2C Read Auto-Increment */
	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

/* Supported Formats For Script Below */
/* - 01-29 HDMI to MIPI TxA CSI 4-Lane - RGB888: */
static const struct adv748x_reg_value adv748x_init_txa_4lane[] = {
	/* Disable chip powerdown & Enable HDMI Rx block */
	{ADV748X_PAGE_IO, 0x00, 0x40},

	{ADV748X_PAGE_REPEATER, 0x40, 0x83}, /* Enable HDCP 1.1 */

	{ADV748X_PAGE_HDMI, 0x00, 0x08},/* Foreground Channel = A */
	{ADV748X_PAGE_HDMI, 0x98, 0xff},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x99, 0xa3},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x9a, 0x00},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x9b, 0x0a},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x9d, 0x40},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0xcb, 0x09},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x3d, 0x10},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x3e, 0x7b},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x3f, 0x5e},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x4e, 0xfe},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x4f, 0x18},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x57, 0xa3},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x58, 0x04},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0x85, 0x10},/* ADI Required Write */

	{ADV748X_PAGE_HDMI, 0x83, 0x00},/* Enable All Terminations */
	{ADV748X_PAGE_HDMI, 0xa3, 0x01},/* ADI Required Write */
	{ADV748X_PAGE_HDMI, 0xbe, 0x00},/* ADI Required Write */

	{ADV748X_PAGE_HDMI, 0x6c, 0x01},/* HPA Manual Enable */
	{ADV748X_PAGE_HDMI, 0xf8, 0x01},/* HPA Asserted */
	{ADV748X_PAGE_HDMI, 0x0f, 0x00},/* Audio Mute Speed Set to Fastest */
	/* (Smallest Step Size) */

	{ADV748X_PAGE_IO, 0x04, 0x02},	/* RGB Out of CP */
	{ADV748X_PAGE_IO, 0x12, 0xf0},	/* CSC Depends on ip Packets, SDR 444 */
	{ADV748X_PAGE_IO, 0x17, 0x80},	/* Luma & Chroma can reach 254d */
	{ADV748X_PAGE_IO, 0x03, 0x86},	/* CP-Insert_AV_Code */

	{ADV748X_PAGE_CP, 0x7c, 0x00},	/* ADI Required Write */

	{ADV748X_PAGE_IO, 0x0c, 0xe0},	/* Enable LLC_DLL & Double LLC Timing */
	{ADV748X_PAGE_IO, 0x0e, 0xdd},	/* LLC/PIX/SPI PINS TRISTATED AUD */
	/* Outputs Enabled */
	{ADV748X_PAGE_IO, 0x10, 0xa0},	/* Enable 4-lane CSI Tx & Pixel Port */

	{ADV748X_PAGE_TXA, 0x00, 0x84},	/* Enable 4-lane MIPI */
	{ADV748X_PAGE_TXA, 0x00, 0xa4},	/* Set Auto DPHY Timing */
	{ADV748X_PAGE_TXA, 0xdb, 0x10},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xd6, 0x07},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xc4, 0x0a},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x71, 0x33},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x72, 0x11},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xf0, 0x00},	/* i2c_dphy_pwdn - 1'b0 */

	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_WAIT, 0x00, 0x02},/* delay 2 */
	{ADV748X_PAGE_TXA, 0x00, 0x24 },/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXA, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

/* 02-01 Analog CVBS to MIPI TX-B CSI 1-Lane - */
/* Autodetect CVBS Single Ended In Ain 1 - MIPI Out */
static const struct adv748x_reg_value adv748x_init_txb_1lane[] = {

	{ADV748X_PAGE_IO, 0x00, 0x30},	/* Disable chip powerdown Rx */
	{ADV748X_PAGE_IO, 0xf2, 0x01},	/* Enable I2C Read Auto-Increment */

	{ADV748X_PAGE_IO, 0x0e, 0xff},	/* LLC/PIX/AUD/SPI PINS TRISTATED */

	{ADV748X_PAGE_SDP, 0x0f, 0x00},	/* Exit Power Down Mode */
	{ADV748X_PAGE_SDP, 0x52, 0xcd},	/* ADI Required Write */

	{ADV748X_PAGE_SDP, 0x0e, 0x80},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x9c, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x9c, 0xff},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x0e, 0x00},	/* ADI Required Write */

	/* ADI recommended writes for improved video quality */
	{ADV748X_PAGE_SDP, 0x80, 0x51},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x81, 0x51},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x82, 0x68},	/* ADI Required Write */

	{ADV748X_PAGE_SDP, 0x03, 0x42},	/* Tri-S Output , PwrDwn 656 pads */
	{ADV748X_PAGE_SDP, 0x04, 0xb5},	/* ITU-R BT.656-4 compatible */
	{ADV748X_PAGE_SDP, 0x13, 0x00},	/* ADI Required Write */

	{ADV748X_PAGE_SDP, 0x17, 0x41},	/* Select SH1 */
	{ADV748X_PAGE_SDP, 0x31, 0x12},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0xe6, 0x4f},  /* V bit end pos manually in NTSC */

	/* Enable 1-Lane MIPI Tx, */
	/* enable pixel output and route SD through Pixel port */
	{ADV748X_PAGE_IO, 0x10, 0x70},

	{ADV748X_PAGE_TXB, 0x00, 0x81},	/* Enable 1-lane MIPI */
	{ADV748X_PAGE_TXB, 0x00, 0xa1},	/* Set Auto DPHY Timing */
	{ADV748X_PAGE_TXB, 0xd2, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0xc4, 0x0a},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0x71, 0x33},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0x72, 0x11},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0xf0, 0x00},	/* i2c_dphy_pwdn - 1'b0 */
	{ADV748X_PAGE_TXB, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXB, 0xda, 0x01},	/* i2c_mipi_pll_en - 1'b1 */

	{ADV748X_PAGE_WAIT, 0x00, 0x02},/* delay 2 */
	{ADV748X_PAGE_TXB, 0x00, 0x21 },/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXB, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},/* delay 1 */
	{ADV748X_PAGE_TXB, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static const struct adv748x_reg_value adv748x_init_txa_afe_1lane[] = {
	{ADV748X_PAGE_IO, 0x00, 0x30},	/* Disable chip powerdown Rx */
	{ADV748X_PAGE_IO, 0x0e, 0xff},	/* LLC/PIX/AUD/SPI PINS TRISTATED */
	{ADV748X_PAGE_SDP, 0x0f, 0x00},	/* Exit Power Down Mode */
	{ADV748X_PAGE_SDP, 0x52, 0xcd},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x00, 0x07},	/* INSEL = CVBS in on Ain 8 */
	{ADV748X_PAGE_SDP, 0x0e, 0x80},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x9c, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x9c, 0xff},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x0e, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x80, 0x51},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x81, 0x51},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x82, 0x68},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x03, 0x42},
		/* Tri-S Output Drivers, PwrDwn 656 pads */
	{ADV748X_PAGE_SDP, 0x04, 0xb5},	/* ITU-R BT.656-4 compatible */
	{ADV748X_PAGE_SDP, 0x13, 0x00},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0x17, 0x41},	/* Select SH1 */
	{ADV748X_PAGE_SDP, 0x31, 0x12},	/* ADI Required Write */
	{ADV748X_PAGE_SDP, 0xe6, 0x4f},
		/* Set V bit end position manually in NTSC mode */
	{ADV748X_PAGE_IO, 0x10, 0xb8 },
		/* Enable pixel output and route SD through Pixel port */
	{ADV748X_PAGE_TXA, 0x00, 0x81},
		/* Enable 4-lane MIPI, 1-Lane Configuration */
	{ADV748X_PAGE_TXA, 0x00, 0xa1},	/* Set Auto DPHY Timing */
	{ADV748X_PAGE_TXA, 0xd2, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xc4, 0x0a},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x71, 0x33},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x72, 0x11},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xf0, 0x00},	/* i2c_dphy_pwdn - 1'b0 */
	{ADV748X_PAGE_TXA, 0x31, 0x82},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0x1e, 0x40},	/* ADI Required Write */
	{ADV748X_PAGE_TXA, 0xda, 0x00},	/* i2c_mipi_pll_en - 1'b1 */
	{ADV748X_PAGE_WAIT, 0x00, 0x02},	/* delay 2 */
	{ADV748X_PAGE_TXA, 0x00, 0x21},	/* Power-up CSI-TX */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},	/* delay 1 */
	{ADV748X_PAGE_TXA, 0xc1, 0x2b},	/* ADI Required Write */
	{ADV748X_PAGE_WAIT, 0x00, 0x01},	/* delay 1 */
	{ADV748X_PAGE_TXA, 0x31, 0x80},	/* ADI Required Write */

	{ADV748X_PAGE_EOR, 0xff, 0xff}	/* End of register table */
};

static int adv748x_reset(struct adv748x_state *state)
{
	int ret;
	u8 value;

	ret = adv748x_write_regs(state, adv748x_sw_reset);
	if (ret < 0)
		return ret;

	ret = adv748x_set_slave_addresses(state);
	if (ret < 0)
		return ret;

	/* Init and power down TXA */
	if (state->afe.txa_switch) {
		ret = adv748x_write_regs(state, adv748x_init_txa_afe_1lane);
		if (ret)
			return ret;
		adv748x_txa_power(state, 0);
		value = ADV748X_IO_10_OUT_SD_TXA;
	} else {
		ret = adv748x_write_regs(state, adv748x_init_txa_4lane);
		if (ret)
			return ret;

		adv748x_txa_power(state, 0);

		/* Init and power down TXB */
		ret = adv748x_write_regs(state, adv748x_init_txb_1lane);
		if (ret)
			return ret;

		adv748x_txb_power(state, 0);
		value = 0;
	}

	/* Disable chip powerdown & Enable HDMI Rx block */
	io_write(state, ADV748X_IO_PD, ADV748X_IO_PD_RX_EN);

	/* Enable 4-lane CSI Tx & Pixel Port */
	io_write(state, ADV748X_IO_10, value | ADV748X_IO_10_CSI4_EN |
				       ADV748X_IO_10_CSI1_EN |
				       ADV748X_IO_10_PIX_OUT_EN);

	/* Use vid_std and v_freq as freerun resolution for CP */
	cp_clrset(state, ADV748X_CP_CLMP_POS, ADV748X_CP_CLMP_POS_DIS_AUTO,
					      ADV748X_CP_CLMP_POS_DIS_AUTO);

	return 0;
}

static int adv748x_identify_chip(struct adv748x_state *state)
{
	int msb, lsb;

	lsb = io_read(state, ADV748X_IO_CHIP_REV_ID_1);
	msb = io_read(state, ADV748X_IO_CHIP_REV_ID_2);

	if (lsb < 0 || msb < 0) {
		adv_err(state, "Failed to read chip revision\n");
		return -EIO;
	}

	adv_info(state, "chip found @ 0x%02x revision %02x%02x\n",
		 state->client->addr << 1, lsb, msb);

	return 0;
}

/* -----------------------------------------------------------------------------
 * i2c driver
 */

void adv748x_subdev_init(struct v4l2_subdev *sd, struct adv748x_state *state,
			 const struct v4l2_subdev_ops *ops, u32 function,
			 const char *ident)
{
	v4l2_subdev_init(sd, ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* the owner is the same as the i2c_client's driver owner */
	sd->owner = state->dev->driver->owner;
	sd->dev = state->dev;

	v4l2_set_subdevdata(sd, state);

	/* initialize name */
	snprintf(sd->name, sizeof(sd->name), "%s %d-%04x %s",
		state->dev->driver->name,
		i2c_adapter_id(state->client->adapter),
		state->client->addr, ident);

	sd->entity.function = function;
	sd->entity.ops = &adv748x_media_ops;
}

static int adv748x_parse_dt(struct adv748x_state *state)
{
	struct device_node *ep_np = NULL;
	struct of_endpoint ep;
	bool out_found = false;
	bool in_found = false;
	struct v4l2_fwnode_endpoint v4l2_ep;

	for_each_endpoint_of_node(state->dev->of_node, ep_np) {
		of_graph_parse_endpoint(ep_np, &ep);

		v4l2_fwnode_endpoint_parse(of_fwnode_handle(ep_np), &v4l2_ep);
		adv_info(state, "Endpoint %pOF on port %d\n", ep.local_node,
			 ep.port);

		if (ep.port >= ADV748X_PORT_MAX) {
			adv_err(state, "Invalid endpoint %pOF on port %d\n",
				ep.local_node, ep.port);

			continue;
		}

		if (state->endpoints[ep.port]) {
			adv_err(state,
				"Multiple port endpoints are not supported\n");
			continue;
		}

		of_node_get(ep_np);
		state->endpoints[ep.port] = ep_np;
		if (ep.port == ADV748X_PORT_TXA) {
			const char *str;

			state->hdmi.use_lane =
				v4l2_ep.bus.mipi_csi2.num_data_lanes;

			if (!of_property_read_string(ep_np, "txa_direction",
						     &str)) {
				if (!strcmp(str, "afe"))
					state->afe.txa_switch = true;
				else
					state->afe.txa_switch = false;
			} else {
				state->afe.txa_switch = false;
			}
		}
		/*
		 * At least one input endpoint and one output endpoint shall
		 * be defined.
		 */
		if (ep.port < ADV748X_PORT_TXA)
			in_found = true;
		else
			out_found = true;
	}

	return in_found && out_found ? 0 : -ENODEV;
}

static void adv748x_dt_cleanup(struct adv748x_state *state)
{
	unsigned int i;

	for (i = 0; i < ADV748X_PORT_MAX; i++)
		of_node_put(state->endpoints[i]);
}

static struct snd_soc_dai_driver adv748x_dai = {
	.name = "adv748x-i2s",
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 8,
		.channels_max	= 8,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_U24_LE,
	 },
};

static	int adv748x_of_xlate_dai_name(struct snd_soc_component *component,
				      struct of_phandle_args *args,
				      const char **dai_name)
{
	if (dai_name)
		*dai_name = adv748x_dai.name;
	return 0;
}

static const struct snd_soc_component_driver adv748x_codec = {
	.of_xlate_dai_name = adv748x_of_xlate_dai_name,
};

static int adv748x_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct adv748x_state *state;
	int ret;

	/* Check if the adapter supports the needed features */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	state = devm_kzalloc(&client->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->mutex);

	state->dev = &client->dev;
	state->client = client;
	state->i2c_clients[ADV748X_PAGE_IO] = client;
	i2c_set_clientdata(client, state);

	/*
	 * We can not use container_of to get back to the state with two TXs;
	 * Initialize the TXs's fields unconditionally on the endpoint
	 * presence to access them later.
	 */
	state->txa.state = state->txb.state = state;
	state->txa.page = ADV748X_PAGE_TXA;
	state->txb.page = ADV748X_PAGE_TXB;
	state->txa.port = ADV748X_PORT_TXA;
	state->txb.port = ADV748X_PORT_TXB;

	/* Discover and process ports declared by the Device tree endpoints */
	ret = adv748x_parse_dt(state);
	if (ret) {
		adv_err(state, "Failed to parse device tree\n");
		goto err_free_mutex;
	}

	/* Configure IO Regmap region */
	ret = adv748x_configure_regmap(state, ADV748X_PAGE_IO);
	if (ret) {
		adv_err(state, "Error configuring IO regmap region\n");
		goto err_cleanup_dt;
	}

	ret = adv748x_identify_chip(state);
	if (ret) {
		adv_err(state, "Failed to identify chip\n");
		goto err_cleanup_dt;
	}

	/* Configure remaining pages as I2C clients with regmap access */
	ret = adv748x_initialise_clients(state);
	if (ret) {
		adv_err(state, "Failed to setup client regmap pages\n");
		goto err_cleanup_clients;
	}

	/* SW reset ADV748X to its default values */
	ret = adv748x_reset(state);
	if (ret) {
		adv_err(state, "Failed to reset hardware\n");
		goto err_cleanup_clients;
	}

	/* Initialise HDMI */
	ret = adv748x_hdmi_init(&state->hdmi);
	if (ret) {
		adv_err(state, "Failed to probe HDMI\n");
		goto err_cleanup_clients;
	}

	/* Initialise AFE */
	ret = adv748x_afe_init(&state->afe);
	if (ret) {
		adv_err(state, "Failed to probe AFE\n");
		goto err_cleanup_hdmi;
	}

	/* Initialise TXA */
	ret = adv748x_csi2_init(state, &state->txa);
	if (ret) {
		adv_err(state, "Failed to probe TXA\n");
		goto err_cleanup_afe;
	}

	/* Initialise TXB */
	ret = adv748x_csi2_init(state, &state->txb);
	if (ret) {
		adv_err(state, "Failed to probe TXB\n");
		goto err_cleanup_txa;
	}

	ret = adv748x_dai_init(&state->dai);
	if (ret) {
		adv_err(state, "Failed to probe DAI\n");
		goto err_cleanup_txb;
	}
	return 0;
err_cleanup_txb:
	adv748x_csi2_cleanup(&state->txb);
err_cleanup_txa:
	adv748x_csi2_cleanup(&state->txa);
err_cleanup_afe:
	adv748x_afe_cleanup(&state->afe);
err_cleanup_hdmi:
	adv748x_hdmi_cleanup(&state->hdmi);
err_cleanup_clients:
	adv748x_unregister_clients(state);
err_cleanup_dt:
	adv748x_dt_cleanup(state);
err_free_mutex:
	mutex_destroy(&state->mutex);

	return ret;
}

static int adv748x_remove(struct i2c_client *client)
{
	struct adv748x_state *state = i2c_get_clientdata(client);

	adv748x_dai_cleanup(&state->dai);
	adv748x_afe_cleanup(&state->afe);
	adv748x_hdmi_cleanup(&state->hdmi);

	adv748x_csi2_cleanup(&state->txa);
	adv748x_csi2_cleanup(&state->txb);

	adv748x_unregister_clients(state);
	adv748x_dt_cleanup(state);
	mutex_destroy(&state->mutex);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int adv748x_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adv748x_state *state = i2c_get_clientdata(client);
	struct adv748x_csi2 *txa = &state->txa;
	struct adv748x_csi2 *txb = &state->txb;

	txa->vc_ch = 0x03 & (tx_read(txa, ADV748X_CSI_VC_REF) >>
		    ADV748X_CSI_VC_REF_SHIFT);
	txb->vc_ch = 0x03 & (tx_read(txb, ADV748X_CSI_VC_REF) >>
		    ADV748X_CSI_VC_REF_SHIFT);

	io_write(state, ADV748X_IO_PD, ADV748X_IO_PD_HDMI);

	return 0;
}

static int adv748x_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adv748x_state *state = i2c_get_clientdata(client);
	struct adv748x_csi2 *txa = &state->txa;
	struct adv748x_csi2 *txb = &state->txb;
	int ret;

	/* SW reset ADV748X to its default values */
	ret = adv748x_reset(state);
	if (ret)
		adv_err(state, "Failed to reset hardware");

	/* Initialise the virtual channel */
	tx_write(txa, ADV748X_CSI_VC_REF,
		 txa->vc_ch << ADV748X_CSI_VC_REF_SHIFT);
	tx_write(txb, ADV748X_CSI_VC_REF,
		 txb->vc_ch << ADV748X_CSI_VC_REF_SHIFT);

	ret = adv748x_hdmi_set_resume_edid(&state->hdmi);

	return ret;
}

static const struct dev_pm_ops adv748x_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(adv748x_suspend, adv748x_resume)
};
#endif

static const struct i2c_device_id adv748x_id[] = {
	{ "adv7481", 0 },
	{ "adv7482", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, adv748x_id);

static const struct of_device_id adv748x_of_table[] = {
	{ .compatible = "adi,adv7481", },
	{ .compatible = "adi,adv7482", },
	{ }
};
MODULE_DEVICE_TABLE(of, adv748x_of_table);

static struct i2c_driver adv748x_driver = {
	.driver = {
		.name = "adv748x",
#ifdef CONFIG_PM_SLEEP
		.pm = &adv748x_pm_ops,
#endif
		.of_match_table = adv748x_of_table,
	},
	.probe = adv748x_probe,
	.remove = adv748x_remove,
	.id_table = adv748x_id,
};

module_i2c_driver(adv748x_driver);

MODULE_AUTHOR("Kieran Bingham <kieran.bingham@ideasonboard.com>");
MODULE_DESCRIPTION("ADV748X video decoder");
MODULE_LICENSE("GPL v2");
