#ifndef GC_INTERNAL_H
#define GC_INTERNAL_H

#include <genicam/genicam.h>

#define GC_MAX_XML_SIZE (8U * 1024U * 1024U)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

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

static inline uint16_t gc_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t gc_be32(const uint8_t *p)
{
	return (uint32_t)gc_be16(p) << 16 | gc_be16(p + 2);
}

static inline uint64_t gc_be64(const uint8_t *p)
{
	return (uint64_t)gc_be32(p) << 32 | gc_be32(p + 4);
}

static inline void gc_put_be16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static inline void gc_put_be32(uint8_t *p, uint32_t value)
{
	gc_put_be16(p, (uint16_t)(value >> 16));
	gc_put_be16(p + 2, (uint16_t)value);
}

static inline void gc_put_be64(uint8_t *p, uint64_t value)
{
	gc_put_be32(p, (uint32_t)(value >> 32));
	gc_put_be32(p + 4, (uint32_t)value);
}

#endif /* GC_INTERNAL_H */
