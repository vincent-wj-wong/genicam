#include <genicam/host.h>

#include "common/frame_queue.h"

#include <stdlib.h>

struct gc_stream_queue {
	struct gc_stream *stream;
	struct gc_frame_queue queue;
	const void *receiving;
};

int gc_stream_queue_open(struct gc_stream *stream, size_t capacity,
			  struct gc_stream_queue **result)
{
	struct gc_stream_queue *queue;
	int rc;

	if (!stream || !result)
		return GC_INVALID;
	*result = NULL;
	queue = calloc(1, sizeof(*queue));
	if (!queue)
		return GC_NOMEM;
	rc = gc_frame_queue_init(&queue->queue, capacity);
	if (rc) {
		free(queue);
		return rc;
	}
	queue->stream = stream;
	*result = queue;
	return GC_OK;
}

int gc_stream_queue_push(struct gc_stream_queue *queue, void *data, size_t capacity)
{
	struct gc_frame_slot slot = { .frame = { .data = data, .capacity = capacity } };
	int rc;

	if (!queue || !data || !capacity)
		return GC_INVALID;
	osal_mutex_lock(queue->queue.lock);
	rc = queue->receiving == data ? GC_BUSY :
					 gc_frame_queue_push(&queue->queue, &slot);
	osal_mutex_unlock(queue->queue.lock);
	return rc;
}

int gc_stream_queue_receive(struct gc_stream_queue *queue, struct gc_frame *frame,
			     unsigned int timeout_ms)
{
	struct gc_frame_slot slot;
	int rc;

	if (!queue || !frame)
		return GC_INVALID;
	*frame = (struct gc_frame){ 0 };
	osal_mutex_lock(queue->queue.lock);
	if (queue->receiving) {
		osal_mutex_unlock(queue->queue.lock);
		return GC_BUSY;
	}
	rc = gc_frame_queue_pop(&queue->queue, &slot);
	if (!rc)
		queue->receiving = slot.frame.data;
	osal_mutex_unlock(queue->queue.lock);
	if (rc)
		return rc;

	*frame = slot.frame;
	rc = gc_stream_receive(queue->stream, frame, timeout_ms);
	osal_mutex_lock(queue->queue.lock);
	queue->receiving = NULL;
	osal_mutex_unlock(queue->queue.lock);
	return rc;
}

int gc_stream_queue_pop(struct gc_stream_queue *queue, struct gc_frame *frame)
{
	struct gc_frame_slot slot;
	int rc;

	if (!queue || !frame)
		return GC_INVALID;
	osal_mutex_lock(queue->queue.lock);
	rc = gc_frame_queue_pop(&queue->queue, &slot);
	osal_mutex_unlock(queue->queue.lock);
	if (!rc)
		*frame = slot.frame;
	return rc;
}

void gc_stream_queue_close(struct gc_stream_queue *queue)
{
	if (!queue)
		return;
	gc_frame_queue_destroy(&queue->queue);
	free(queue);
}
