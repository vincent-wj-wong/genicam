#ifndef GENICAM_FRAME_H
#define GENICAM_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gc_payload_type {
	GC_PAYLOAD_IMAGE = 1,
};

struct gc_frame {
	void *data;
	size_t capacity;
	size_t size;
	uint64_t block_id;
	uint64_t timestamp;
	uint64_t timestamp_hz;	/* Zero if the device clock rate is unknown. */
	size_t stride;
	uint32_t pixel_format;
	uint32_t width;
	uint32_t height;
	uint32_t x_offset;
	uint32_t y_offset;
	uint16_t x_padding;
	uint16_t y_padding;
	enum gc_payload_type payload_type;
};

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_FRAME_H */
