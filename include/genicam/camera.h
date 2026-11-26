#ifndef GENICAM_CAMERA_H
#define GENICAM_CAMERA_H

#include <genicam/frame.h>
#include <genicam/status.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gc_camera;

struct gc_camera_config {
	const char *manufacturer;
	const char *model;
	const char *serial;
	const void *xml;
	size_t xml_size;
	void *context;
	int (*read)(void *context, uint64_t address, void *data, size_t size);
	int (*write)(void *context, uint64_t address, const void *data, size_t size);
	uint64_t timestamp_hz;
};

typedef void (*gc_camera_frame_done)(void *context, const struct gc_frame *frame,
				     int status);

/* A successful submission borrows the payload until the completion callback. */
int gc_camera_submit_frame(struct gc_camera *camera, const struct gc_frame *frame,
			    gc_camera_frame_done done, void *context);
/* One service caller. Use a finite timeout when submitting from another task. */
int gc_camera_poll(struct gc_camera *camera, unsigned int timeout_ms);
/* Join other callers before closing. Pending frames complete with GC_STOPPED. */
void gc_camera_close(struct gc_camera *camera);

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_CAMERA_H */
