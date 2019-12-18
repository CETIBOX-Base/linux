#ifndef _DEVCTS_H_
#define _DEVCTS_H_

#include <linux/ktime.h>
#include <linux/errno.h>
#include <uapi/linux/devcts.h>

typedef int (*devcts_get_time_fn_t)(ktime_t *, ktime_t *, void *);

#if defined(CONFIG_CHAR_CROSSTIMESTAMP) || defined(CONFIG_CHAR_CROSSTIMESTAMP_MODULE)

int devcts_register_device(const char *name, devcts_get_time_fn_t fn, void *ctx);
void devcts_unregister_device(const char *name);

#else

static inline int devcts_register_device(const char *name, devcts_get_time_fn_t fn, void *ctx)
{
	return -ENODEV;
}
static inline void devcts_unregister_device(const char *name)
{
}

#endif

#endif
