#ifndef GENICAM_H
#define GENICAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gc_status {
	GC_OK = 0,
	GC_INVALID = -1,
	GC_NOMEM = -2,
	GC_IO = -3,
	GC_TIMEOUT = -4,
	GC_PROTOCOL = -5,
	GC_ACCESS = -6,
	GC_UNSUPPORTED = -7,
	GC_NOT_FOUND = -8,
	GC_BUFFER_SMALL = -9,
	GC_BUSY = -10,
	GC_STOPPED = -11,
};

struct gc_device;
struct gc_stream;
struct gc_nodemap;

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

struct gc_stream_stats {
	uint64_t completed;
	uint64_t incomplete;
	uint64_t duplicates;
	uint64_t invalid_packets;
	uint64_t resend_requests;
};

const char *gc_status_string(int status);

/* Close streams and node maps before closing the device. */
void gc_device_close(struct gc_device *device);
int gc_device_read(struct gc_device *device, uint64_t address, void *data,
		   size_t size);
int gc_device_write(struct gc_device *device, uint64_t address, const void *data,
		    size_t size);
int gc_device_read_register(struct gc_device *device, uint64_t address,
			    uint32_t *value);
int gc_device_write_register(struct gc_device *device, uint64_t address,
			     uint32_t value);

/* One stream per device. Start and stop control local reception. */
int gc_stream_open(struct gc_device *device, struct gc_stream **stream);
int gc_stream_start(struct gc_stream *stream);
int gc_stream_stop(struct gc_stream *stream);
/* One receiver at a time. The caller owns frame->data. */
int gc_stream_receive(struct gc_stream *stream, struct gc_frame *frame,
		      unsigned int timeout_ms);
int gc_stream_get_stats(struct gc_stream *stream, struct gc_stream_stats *stats);
/* Stop and join receiving callers before closing. */
void gc_stream_close(struct gc_stream *stream);

int gc_nodemap_open(struct gc_device *device, struct gc_nodemap **map);
void gc_nodemap_close(struct gc_nodemap *map);
int gc_feature_get_integer(struct gc_nodemap *map, const char *name,
			   int64_t *value);
int gc_feature_set_integer(struct gc_nodemap *map, const char *name,
			   int64_t value);
int gc_feature_get_float(struct gc_nodemap *map, const char *name, double *value);
int gc_feature_set_float(struct gc_nodemap *map, const char *name, double value);
int gc_feature_get_string(struct gc_nodemap *map, const char *name, char *value,
			  size_t capacity);
int gc_feature_set_string(struct gc_nodemap *map, const char *name,
			  const char *value);
int gc_feature_execute(struct gc_nodemap *map, const char *name);
size_t gc_nodemap_count(const struct gc_nodemap *map);
const char *gc_nodemap_name(const struct gc_nodemap *map, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_H */
