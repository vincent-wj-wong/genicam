#ifndef GC_GIGE_HOST_INTERNAL_H
#define GC_GIGE_HOST_INTERNAL_H

#include <genicam/gige/host.h>

#include "host/internal.h"
#include "osal/osal.h"
#include "transport/gige/wire/protocol.h"

struct gige_device {
	struct gc_device device;
	osal_socket control;
	struct osal_endpoint local;
	struct osal_endpoint peer;
	struct osal_mutex *lock;
	struct osal_thread *heartbeat_thread;
	unsigned int timeout;
	unsigned int heartbeat;
	uint32_t capability;
	uint64_t timestamp_hz;
	uint16_t request;
	uint16_t packet_size;
	int monitor;
	int stopping;
	int stream_open;
};

int gige_stream_open(void *context, struct gc_stream **stream);
int gige_request_resend(struct gige_device *dev, uint64_t block, int extended,
			uint32_t first, uint32_t last);

#endif /* GC_GIGE_HOST_INTERNAL_H */
