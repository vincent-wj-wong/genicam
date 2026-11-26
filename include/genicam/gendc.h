/* GenICam GenDC 1.2 final containers in contiguous memory. */
#ifndef GENICAM_GENDC_H
#define GENICAM_GENDC_H

#include <genicam/frame.h>
#include <genicam/status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_GENDC_VERSION_MAJOR 1
#define GC_GENDC_VERSION_MINOR 2

enum gc_gendc_flags {
	GC_GENDC_TIMESTAMP_PTP = 1,
	GC_GENDC_COMPONENT_INVALID = 2,
};

enum gc_gendc_part_type {
	GC_GENDC_CHUNK = 0x4000,
	GC_GENDC_XML = 0x4001,
	GC_GENDC_ARRAY = 0x4100,
	GC_GENDC_IMAGE = 0x4200,
	GC_GENDC_JPEG = 0x4201,
	GC_GENDC_JPEG2000 = 0x4202,
	GC_GENDC_H264 = 0x4203,
	GC_GENDC_JPEG_XS = 0x4204,
};

struct gc_gendc_view {
	const uint8_t *bytes;
	size_t size;
	size_t descriptor_size;
	size_t data_offset;
	size_t data_size;
	uint64_t id;
	uint32_t component_count;
	uint16_t flags;
	uint8_t version_major;
	uint8_t version_minor;
	uint8_t version_subminor;
};

struct gc_gendc_component {
	uint64_t type_id;
	uint64_t timestamp_ns;
	uint32_t format;
	uint32_t x_offset;
	uint32_t y_offset;
	uint16_t group_id;
	uint16_t source_id;
	uint16_t region_id;
	uint16_t part_count;
	int invalid;
};

struct gc_gendc_part {
	const void *data;
	size_t size;
	uint64_t flow_offset;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint16_t x_padding;
	uint16_t y_padding;
	uint16_t flow_id;
	uint16_t type;
	uint16_t flags;
};

struct gc_gendc_image_info {
	uint64_t container_id;
	uint64_t component_type;
	uint64_t timestamp_ns;
	uint16_t group_id;
	uint16_t source_id;
	uint16_t region_id;
	uint16_t flags;
};

/* The view borrows the input. It must stay alive and immutable. */
int gc_gendc_open(const void *data, size_t size, struct gc_gendc_view *view);
int gc_gendc_get_component(const struct gc_gendc_view *view, uint32_t index,
			    struct gc_gendc_component *component);
int gc_gendc_get_part(const struct gc_gendc_view *view, uint32_t component,
		       uint16_t index, struct gc_gendc_part *part);
/* The encoder accepts one unpacked or packed PFNC image in wire byte order. */
int gc_gendc_encode_image(void *data, size_t capacity, const struct gc_frame *frame,
			   const struct gc_gendc_image_info *info, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_GENDC_H */
