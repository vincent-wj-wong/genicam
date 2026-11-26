#ifndef GC_COMMON_INTERNAL_H
#define GC_COMMON_INTERNAL_H

#include <genicam/frame.h>
#include <genicam/status.h>

#define GC_MAX_XML_SIZE (8U * 1024U * 1024U)
#define GC_IMAGE_LIMIT (256U * 1024U * 1024U)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

int gc_image_layout(struct gc_frame *frame, size_t *size);

static inline uint16_t gc_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

static inline uint32_t gc_le32(const uint8_t *p)
{
	return (uint32_t)gc_le16(p + 2) << 16 | gc_le16(p);
}

static inline uint64_t gc_le64(const uint8_t *p)
{
	return (uint64_t)gc_le32(p + 4) << 32 | gc_le32(p);
}

static inline void gc_put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void gc_put_le32(uint8_t *p, uint32_t value)
{
	gc_put_le16(p, (uint16_t)value);
	gc_put_le16(p + 2, (uint16_t)(value >> 16));
}

static inline void gc_put_le64(uint8_t *p, uint64_t value)
{
	gc_put_le32(p, (uint32_t)value);
	gc_put_le32(p + 4, (uint32_t)(value >> 32));
}

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

#endif /* GC_COMMON_INTERNAL_H */
