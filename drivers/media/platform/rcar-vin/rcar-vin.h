/*
 * Driver for Renesas R-Car VIN
 *
 * Copyright (C) 2016-2018 Renesas Electronics Corp.
 * Copyright (C) 2011-2013 Renesas Solutions Corp.
 * Copyright (C) 2013 Cogent Embedded, Inc., <source@cogentembedded.com>
 * Copyright (C) 2008 Magnus Damm
 *
 * Based on the soc-camera rcar_vin driver
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __RCAR_VIN__
#define __RCAR_VIN__

#include <linux/clk.h>
#include <linux/kref.h>
#include <linux/reset.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define DRV_NAME "rcar-vin"

/* Number of HW buffers */
#define HW_BUFFER_NUM 3

/* Address alignment mask for HW buffers */
#define HW_BUFFER_MASK 0x7f

/* Max number on VIN instances that can be in a system */
#define RCAR_VIN_NUM 8

/* Time until source device reconnects */
#define CONNECTION_TIME 2000
#define SETUP_WAIT_TIME 3000

#define MSTP_WAIT_TIME 100

#define RCAR_VIN_DES1_RESERVED		BIT(0)

struct rvin_group;

enum model_id {
	RCAR_H1,
	RCAR_M1,
	RCAR_GEN2,
	RCAR_GEN3,
};

enum rvin_csi_id {
	RVIN_CSI20,
	RVIN_CSI21,
	RVIN_CSI40,
	RVIN_CSI41,
	RVIN_CSI_MAX,
};

/**
 * STOPPED  - No operation in progress
 * RUNNING  - Operation in progress have buffers
 * STALLED  - No operation in progress have no buffers
 * STOPPING - Stopping operation
 */
enum rvin_dma_state {
	STOPPED = 0,
	RUNNING,
	STALLED,
	STOPPING,
};

/**
 * struct rvin_video_format - Data format stored in memory
 * @fourcc:	Pixelformat
 * @bpp:	Bytes per pixel
 */
struct rvin_video_format {
	u32 fourcc;
	u8 bpp;
};

/**
 * struct rvin_graph_entity - Video endpoint from async framework
 * @asd:	sub-device descriptor for async framework
 * @subdev:	subdevice matched using async framework
 * @source_pad:	source pad of remote subdevice
 * @sink_pad:	sink pad of remote subdevice
 */
struct rvin_graph_entity {
	struct v4l2_async_subdev asd;
	struct v4l2_subdev *subdev;

	unsigned int source_pad;
	unsigned int sink_pad;
};

/**
 * struct rvin_uds_regs - UDS register information
 * @ctrl:		UDS Control register
 * @scale:		UDS Scaling Factor register
 * @pass_bwidth:	UDS Passband Register
 * @clip_size:		UDS Output Size Clipping Register
 */
struct rvin_uds_regs {
	unsigned long ctrl;
	unsigned long scale;
	unsigned long pass_bwidth;
	unsigned long clip_size;
};

/**
 * struct rvin_group_route - describes a route from a channel of a
 *	CSI-2 receiver to a VIN
 *
 * @csi:	CSI-2 receiver ID.
 * @channel:	Output channel of the CSI-2 receiver.
 * @vin:	VIN ID.
 * @mask:	Bitmask of the different CHSEL register values that
 *		allow for a route from @csi + @chan to @vin.
 *
 * .. note::
 *	Each R-Car CSI-2 receiver has four output channels facing the VIN
 *	devices, each channel can carry one CSI-2 Virtual Channel (VC).
 *	There is no correlation between channel number and CSI-2 VC. It's
 *	up to the CSI-2 receiver driver to configure which VC is output
 *	on which channel, the VIN devices only care about output channels.
 *
 *	There are in some cases multiple CHSEL register settings which would
 *	allow for the same route from @csi + @channel to @vin. For example
 *	on R-Car H3 both the CHSEL values 0 and 3 allow for a route from
 *	CSI40/VC0 to VIN0. All possible CHSEL values for a route need to be
 *	recorded as a bitmask in @mask, in this example bit 0 and 3 should
 *	be set.
 */
struct rvin_group_route {
	enum rvin_csi_id csi;
	unsigned int channel;
	unsigned int vin;
	unsigned int mask;
};

/**
 * struct rvin_info - Information about the particular VIN implementation
 * @model:		VIN model
 * @use_mc:		use media controller instead of controlling subdevice
 * @max_width:		max input width the VIN supports
 * @max_height:		max input height the VIN supports
 * @routes:		list of possible routes from the CSI-2 recivers to
 *			all VINs. The list mush be NULL terminated.
 */
struct rvin_info {
	enum model_id model;
	bool use_mc;

	unsigned int max_width;
	unsigned int max_height;
	const struct rvin_group_route *routes;
};

/**
 * struct rvin_dev - Renesas VIN device structure
 * @dev:		(OF) device
 * @base:		device I/O register space remapped to virtual memory
 * @info:		info about VIN instance
 *
 * @vdev:		V4L2 video device associated with VIN
 * @v4l2_dev:		V4L2 device
 * @ctrl_handler:	V4L2 control handler
 * @notifier:		V4L2 asynchronous subdevs notifier
 * @digital:		entity in the DT for local digital subdevice
 * @rstc:		CPG reset/release control
 * @clk:		CPG clock control
 *
 * @group:		Gen3 CSI group
 * @id:			Gen3 group id for this VIN
 * @pad:		media pad for the video device entity
 *
 * @lock:		protects @queue
 * @queue:		vb2 buffers queue
 * @scratch:		cpu address for scratch buffer
 * @scratch_phys:	physical address of the scratch buffer
 *
 * @qlock:		protects @queue_buf, @buf_list, @continuous, @sequence
 *			@state
 * @queue_buf:		Keeps track of buffers given to HW slot
 * @buf_list:		list of queued buffers
 * @continuous:		tracks if active operation is continuous or single mode
 * @sequence:		V4L2 buffers sequence number
 * @state:		keeps track of operation state
 *
 * @mbus_cfg:		media bus configuration from DT
 * @mbus_code:		media bus format code
 * @format:		active V4L2 pixel format
 *
 * @crop:		active cropping
 * @compose:		active composing
 * @source:		active size of the video source
 * @std:		active video standard of the video source
 * @work_queue:		work queue at resuming
 * @rvin_resume:	delayed work at resuming
 * @chsel:		channel selection
 * @setup_wait:		wait queue used to setup VIN
 * @suspend:		suspend flag
 * @chip_info:         chip information by each device
 */
struct rvin_dev {
	struct device *dev;
	void __iomem *base;
	const struct rvin_info *info;

	struct video_device vdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_async_notifier notifier;
	struct rvin_graph_entity *digital;
	struct reset_control *rstc;
	struct clk *clk;

	struct rvin_group *group;
	unsigned int id;
	struct media_pad pad;

	struct mutex lock;
	struct vb2_queue queue;
	void *scratch;
	dma_addr_t scratch_phys;

	spinlock_t qlock;
	struct vb2_v4l2_buffer *queue_buf[HW_BUFFER_NUM];
	struct list_head buf_list;
	bool continuous;
	unsigned int sequence;
	enum rvin_dma_state state;

	struct v4l2_mbus_config mbus_cfg;
	u32 mbus_code;
	struct v4l2_pix_format format;

	struct v4l2_rect crop;
	struct v4l2_rect compose;
	struct v4l2_rect source;
	v4l2_std_id std;

	struct workqueue_struct *work_queue;
	struct delayed_work rvin_resume;
	unsigned int chsel;
	wait_queue_head_t setup_wait;
	bool suspend;
	bool subdev_completed;
	u32 chip_info;
};

#define vin_to_source(vin)		((vin)->digital->subdev)

/* Debug */
#define vin_dbg(d, fmt, arg...)		dev_dbg(d->dev, fmt, ##arg)
#define vin_info(d, fmt, arg...)	dev_info(d->dev, fmt, ##arg)
#define vin_warn(d, fmt, arg...)	dev_warn(d->dev, fmt, ##arg)
#define vin_err(d, fmt, arg...)		dev_err(d->dev, fmt, ##arg)

/**
 * struct rvin_group - VIN CSI2 group information
 * @refcount:		number of VIN instances using the group
 *
 * @mdev:		media device which represents the group
 *
 * @lock:		protects the count, notifier, vin and csi members
 * @count:		number of enabled VIN instances found in DT
 * @notifier:		pointer to the notifier of a VIN which handles the
 *			groups async sub-devices.
 * @vin:		VIN instances which are part of the group
 * @csi:		array of pairs of fwnode and subdev pointers
 *			to all CSI-2 subdevices.
 */
struct rvin_group {
	struct kref refcount;

	struct media_device mdev;

	struct mutex lock;
	unsigned int count;
	struct v4l2_async_notifier *notifier;
	struct rvin_dev *vin[RCAR_VIN_NUM];

	struct {
		struct fwnode_handle *fwnode;
		struct v4l2_subdev *subdev;
	} csi[RVIN_CSI_MAX];
};

int rvin_dma_register(struct rvin_dev *vin, int irq);
void rvin_dma_unregister(struct rvin_dev *vin);

int rvin_v4l2_register(struct rvin_dev *vin);
void rvin_v4l2_unregister(struct rvin_dev *vin);

const struct rvin_video_format *rvin_format_from_pixel(u32 pixelformat);

int rvin_set_channel_routing(struct rvin_dev *vin, u8 chsel);
u32 rvin_get_chsel(struct rvin_dev *vin);

void rvin_resume_start_streaming(struct work_struct *work);
void rvin_suspend_stop_streaming(struct rvin_dev *vin);

#endif
