#include "frame_queue.h"

#include <stdlib.h>

#define QUEUE_MAX 4096

int gc_frame_queue_init(struct gc_frame_queue *queue, size_t capacity)
{
	if (!capacity || capacity > QUEUE_MAX)
		return GC_INVALID;
	queue->slots = calloc(capacity, sizeof(*queue->slots));
	if (!queue->slots)
		return GC_NOMEM;
	queue->lock = osal_mutex_create();
	if (!queue->lock) {
		free(queue->slots);
		return GC_NOMEM;
	}
	queue->capacity = capacity;
	return GC_OK;
}

void gc_frame_queue_destroy(struct gc_frame_queue *queue)
{
	osal_mutex_destroy(queue->lock);
	free(queue->slots);
}

int gc_frame_queue_push(struct gc_frame_queue *queue, const struct gc_frame_slot *slot)
{
	size_t tail = queue->head;
	size_t i;

	for (i = 0; i < queue->count; i++) {
		if (queue->slots[tail].frame.data == slot->frame.data)
			return GC_BUSY;
		tail = (tail + 1) % queue->capacity;
	}
	if (queue->count == queue->capacity)
		return GC_BUFFER_SMALL;
	queue->slots[tail] = *slot;
	queue->count++;
	return GC_OK;
}

int gc_frame_queue_pop(struct gc_frame_queue *queue, struct gc_frame_slot *slot)
{
	if (!queue->count)
		return GC_NOT_FOUND;
	*slot = queue->slots[queue->head];
	queue->head = (queue->head + 1) % queue->capacity;
	queue->count--;
	return GC_OK;
}
