#include "internal.h"

#include "common/frame_queue.h"

#include <stdlib.h>

struct gc_camera {
	const struct gc_camera_ops *ops;
	void *context;
	struct gc_frame_queue queue;
	const void *sending;
	int polling;
	int closing;
};

int gc_camera_create(const struct gc_camera_ops *ops, void *context,
		      size_t capacity, struct gc_camera **result)
{
	struct gc_camera *camera;
	int rc;

	camera = calloc(1, sizeof(*camera));
	if (!camera)
		return GC_NOMEM;
	rc = gc_frame_queue_init(&camera->queue, capacity);
	if (rc) {
		free(camera);
		return rc;
	}
	camera->ops = ops;
	camera->context = context;
	*result = camera;
	return GC_OK;
}

int gc_camera_submit_frame(struct gc_camera *camera, const struct gc_frame *frame,
			    gc_camera_frame_done done, void *context)
{
	struct gc_frame_slot slot;
	size_t expected;
	int rc;

	if (!camera || !frame || !done || !frame->data || !frame->size ||
	    frame->size > frame->capacity)
		return GC_INVALID;
	if (frame->payload_type != GC_PAYLOAD_IMAGE)
		return GC_UNSUPPORTED;
	slot.frame = *frame;
	slot.done = done;
	slot.context = context;
	rc = gc_image_layout(&slot.frame, &expected);
	if (rc)
		return rc;
	if (frame->size != expected || (frame->stride && frame->stride != slot.frame.stride))
		return GC_INVALID;

	osal_mutex_lock(camera->queue.lock);
	if (camera->closing)
		rc = GC_STOPPED;
	else if (camera->sending == frame->data)
		rc = GC_BUSY;
	else
		rc = gc_frame_queue_push(&camera->queue, &slot);
	osal_mutex_unlock(camera->queue.lock);
	return rc;
}

int gc_camera_poll(struct gc_camera *camera, unsigned int timeout_ms)
{
	struct gc_frame_slot slot;
	int rc;
	int result;
	int have_frame = 0;

	if (!camera)
		return GC_INVALID;
	osal_mutex_lock(camera->queue.lock);
	if (camera->closing || camera->polling) {
		rc = camera->closing ? GC_STOPPED : GC_BUSY;
		osal_mutex_unlock(camera->queue.lock);
		return rc;
	}
	camera->polling = 1;
	if (camera->queue.count)
		timeout_ms = 0;
	osal_mutex_unlock(camera->queue.lock);

	rc = camera->ops->poll(camera->context, timeout_ms);
	if (rc == GC_OK || rc == GC_TIMEOUT) {
		osal_mutex_lock(camera->queue.lock);
		if (!gc_frame_queue_pop(&camera->queue, &slot)) {
			have_frame = 1;
			camera->sending = slot.frame.data;
		}
		osal_mutex_unlock(camera->queue.lock);
	}
	if (have_frame) {
		result = camera->ops->send(camera->context, &slot.frame);
		rc = result == GC_STOPPED ? GC_OK : result;
		osal_mutex_lock(camera->queue.lock);
		camera->sending = NULL;
		osal_mutex_unlock(camera->queue.lock);
		slot.done(slot.context, &slot.frame, result);
	}

	osal_mutex_lock(camera->queue.lock);
	camera->polling = 0;
	osal_mutex_unlock(camera->queue.lock);
	return rc;
}

void gc_camera_close(struct gc_camera *camera)
{
	struct gc_frame_slot slot;

	if (!camera)
		return;
	osal_mutex_lock(camera->queue.lock);
	if (camera->closing || camera->polling) {
		osal_mutex_unlock(camera->queue.lock);
		return;
	}
	camera->closing = 1;
	while (!gc_frame_queue_pop(&camera->queue, &slot)) {
		osal_mutex_unlock(camera->queue.lock);
		slot.done(slot.context, &slot.frame, GC_STOPPED);
		osal_mutex_lock(camera->queue.lock);
	}
	osal_mutex_unlock(camera->queue.lock);
	camera->ops->close(camera->context);
	gc_frame_queue_destroy(&camera->queue);
	free(camera);
}
