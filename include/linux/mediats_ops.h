/*
 * Copyright (C) 2020 CETITEC GmbH. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef mediats_ops_h_
#define mediats_ops_h_

#include <linux/types.h>
#include <linux/netdevice.h>

struct ctc_mediats_ctx;

struct ctc_mediats_ops {
	int (*close)(struct ctc_mediats_ctx *ctx);
	int (*get)(struct ctc_mediats_ctx *ctx, uint32_t *avtp, bool blocking);
	int (*flush)(struct ctc_mediats_ctx *ctx);
};

struct ctc_mediats_ctx {
	const struct ctc_mediats_ops *ops;
};

#endif
