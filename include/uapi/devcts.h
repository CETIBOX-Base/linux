#ifndef _UAPI_LINUX_DEVCTS_H
#define _UAPI_LINUX_DEVCTS_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <uapi/asm-generic/ioctl.h>

struct devcts_req {
	u64 src_ts, dst_ts;
	char __user *src_dev, *dst_dev;
};

#define DEVCTS_DEVTOSYS _IOWR(42, 0, struct devcts_req)
#define DEVCTS_SYSTODEV _IOWR(42, 1, struct devcts_req)
#define DEVCTS_DEVTODEV _IOWR(42, 2, struct devcts_req)

#endif
