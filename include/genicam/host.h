#ifndef GENICAM_HOST_H
#define GENICAM_HOST_H

#include <genicam/frame.h>
#include <genicam/status.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gc_device;
struct gc_stream;
struct gc_nodemap;
struct gc_stream_queue;

enum gc_feature_type {
	GC_FEATURE_INTEGER,
	GC_FEATURE_FLOAT,
	GC_FEATURE_BOOLEAN,
	GC_FEATURE_ENUMERATION,
	GC_FEATURE_STRING,
	GC_FEATURE_COMMAND,
};

struct gc_stream_stats {
	uint64_t completed;
	uint64_t incomplete;
	uint64_t duplicates;
	uint64_t invalid_packets;
	uint64_t resend_requests;
};

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

int gc_stream_queue_open(struct gc_stream *stream, size_t capacity,
			  struct gc_stream_queue **queue);
/* The caller keeps the buffer alive until receive, pop, or queue close. */
int gc_stream_queue_push(struct gc_stream_queue *queue, void *data, size_t capacity);
/* A dequeued buffer is returned in frame even after a receive error. */
int gc_stream_queue_receive(struct gc_stream_queue *queue, struct gc_frame *frame,
			     unsigned int timeout_ms);
int gc_stream_queue_pop(struct gc_stream_queue *queue, struct gc_frame *frame);
/* Stop other callers first. Queue close releases all remaining buffers. */
void gc_stream_queue_close(struct gc_stream_queue *queue);

/* The caller releases the returned XML or ZIP bytes with gc_device_free_xml(). */
int gc_device_get_xml(struct gc_device *device, void **data, size_t *size);
void gc_device_free_xml(void *data);
int gc_nodemap_open(struct gc_device *device, struct gc_nodemap **map);
/* The caller may release the supplied description after this returns. */
int gc_nodemap_open_xml(struct gc_device *device, const void *data, size_t size,
			struct gc_nodemap **map);
void gc_nodemap_close(struct gc_nodemap *map);
int gc_feature_get_type(struct gc_nodemap *map, const char *name,
			enum gc_feature_type *type);
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

#endif /* GENICAM_HOST_H */
