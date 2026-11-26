#ifndef GC_HOST_INTERNAL_H
#define GC_HOST_INTERNAL_H

#include <genicam/host.h>

#include "common/internal.h"

struct gc_device_ops {
	void (*close)(void *context);
	int (*read)(void *context, uint64_t addr, void *data, size_t size);
	int (*write)(void *context, uint64_t addr, const void *data, size_t size);
	int (*read_reg)(void *context, uint64_t addr, uint32_t *value);
	int (*write_reg)(void *context, uint64_t addr, uint32_t value);
	int (*xml)(void *context, void **data, size_t *size);
	int (*stream_open)(void *context, struct gc_stream **stream);
};

struct gc_stream_ops {
	void (*close)(void *context);
	int (*start)(void *context);
	int (*stop)(void *context);
	int (*receive)(void *context, struct gc_frame *frame, unsigned int ms);
	int (*stats)(void *context, struct gc_stream_stats *stats);
};

struct gc_device {
	const struct gc_device_ops *ops;
	void *context;
};

struct gc_stream {
	const struct gc_stream_ops *ops;
	void *context;
};

#endif /* GC_HOST_INTERNAL_H */
