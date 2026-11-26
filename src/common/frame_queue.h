#ifndef GC_FRAME_QUEUE_H
#define GC_FRAME_QUEUE_H

#include "common/internal.h"
#include "osal/osal.h"

struct gc_frame_slot {
	struct gc_frame frame;
	void (*done)(void *context, const struct gc_frame *frame, int status);
	void *context;
};

struct gc_frame_queue {
	struct osal_mutex *lock;
	struct gc_frame_slot *slots;
	size_t capacity;
	size_t head;
	size_t count;
};

/* The owner holds the queue lock around operations on live entries. */
int gc_frame_queue_init(struct gc_frame_queue *queue, size_t capacity);
void gc_frame_queue_destroy(struct gc_frame_queue *queue);
int gc_frame_queue_push(struct gc_frame_queue *queue, const struct gc_frame_slot *slot);
int gc_frame_queue_pop(struct gc_frame_queue *queue, struct gc_frame_slot *slot);

#endif /* GC_FRAME_QUEUE_H */
