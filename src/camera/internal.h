#ifndef GC_CAMERA_INTERNAL_H
#define GC_CAMERA_INTERNAL_H

#include <genicam/camera.h>

struct gc_camera_ops {
	int (*poll)(void *context, unsigned int timeout_ms);
	int (*send)(void *context, const struct gc_frame *frame);
	void (*close)(void *context);
};

int gc_camera_create(const struct gc_camera_ops *ops, void *context,
		      size_t capacity, struct gc_camera **result);

#endif /* GC_CAMERA_INTERNAL_H */
