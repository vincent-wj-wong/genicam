/* GigE Vision 1.2/2.x GVSP single-channel image reception. */
#include "gige.h"

#include <stdlib.h>
#include <string.h>

#define DATA_LIMIT		(GEV_MIN_PACKET - GEV_UDP_OVERHEAD - GVSP_EXT_HEADER)
#define PACKET_LIMIT		(GC_IMAGE_LIMIT / DATA_LIMIT + 2)
#define PACKET_BITMAP		((PACKET_LIMIT + 7) / 8)
#define RESEND_INTERVAL_MS	10
#define RESEND_ATTEMPTS		3

struct gige_stream {
	struct gc_stream stream;
	struct gige_device *device;
	struct osal_mutex *lock;
	osal_socket socket;
	struct osal_endpoint local;
	struct gc_stream_stats stats;
	uint32_t packet_size;
	uint8_t packet[GEV_MAX_PACKET];
	uint8_t seen[PACKET_BITMAP];
	int running;
	int receiving;
};

struct assembly {
	struct gc_frame frame;
	unsigned int received;
	uint32_t last_data;
	uint32_t trailer;
	uint32_t short_packet;
	size_t payload_size;
	size_t received_size;
	size_t expected_size;
	uint64_t next_resend;
	unsigned int resend_attempts;
	int extended;
	int active;
};

static int newer_block(uint64_t previous, const struct gvsp_packet *packet)
{
	if (packet->extended)
		return packet->block - previous < (UINT64_C(1) << 63);
	return (uint16_t)(packet->block - previous) < 0x8000;
}

static int frame_leader(struct gige_stream *stream, struct assembly *assembly,
			const struct gvsp_packet *packet, struct gc_frame *frame)
{
	size_t expected = 0;
	int rc;

	if (packet->id)
		return GC_PROTOCOL;
	memset(assembly, 0, sizeof(*assembly));
	assembly->frame.data = frame->data;
	assembly->frame.capacity = frame->capacity;
	assembly->frame.timestamp_hz = stream->device->timestamp_hz;
	rc = gvsp_read_image_leader(packet, &assembly->frame);
	if (rc)
		return rc;

	rc = gc_image_layout(&assembly->frame, &expected);
	if (rc && rc != GC_UNSUPPORTED)
		return rc;
	if (expected > frame->capacity)
		return GC_BUFFER_SMALL;
	assembly->expected_size = expected;
	assembly->payload_size = stream->packet_size - GEV_UDP_OVERHEAD -
				 (packet->extended ? GVSP_EXT_HEADER : GVSP_HEADER);
	assembly->extended = packet->extended;
	assembly->active = 1;
	assembly->next_resend = osal_deadline(RESEND_INTERVAL_MS);
	memset(stream->seen, 0, sizeof(stream->seen));
	return GC_OK;
}

static int frame_data(struct gige_stream *stream, struct assembly *assembly,
		      const struct gvsp_packet *packet)
{
	uint32_t id = packet->id;
	size_t offset;
	size_t end;
	uint8_t *word;
	uint8_t bit;

	if (!id || id >= PACKET_LIMIT || !packet->size ||
	    packet->size > assembly->payload_size ||
	    id - 1 >= GC_IMAGE_LIMIT / assembly->payload_size + 1 ||
	    (assembly->trailer && id >= assembly->trailer))
		return GC_PROTOCOL;
	offset = (id - 1) * assembly->payload_size;
	end = offset + packet->size;
	if (end > GC_IMAGE_LIMIT)
		return GC_PROTOCOL;
	if (assembly->expected_size &&
	    (end > assembly->expected_size ||
	     (packet->size < assembly->payload_size &&
	      end != assembly->expected_size)))
		return GC_PROTOCOL;
	if (end > assembly->frame.capacity)
		return GC_BUFFER_SMALL;
	if ((assembly->short_packet && id > assembly->short_packet) ||
	    (packet->size < assembly->payload_size && id < assembly->last_data))
		return GC_PROTOCOL;

	word = &stream->seen[id / 8];
	bit = (uint8_t)(1U << (id % 8));
	if (*word & bit) {
		stream->stats.duplicates++;
		return GC_OK;
	}

	memcpy((uint8_t *)assembly->frame.data + offset, packet->body, packet->size);
	*word |= bit;
	if (id > assembly->last_data) {
		assembly->last_data = id;
		assembly->received_size = end;
	}
	if (packet->size < assembly->payload_size)
		assembly->short_packet = id;
	assembly->received++;
	return GC_OK;
}

static int frame_trailer(struct assembly *assembly,
			 const struct gvsp_packet *packet)
{
	if (packet->id < 2 ||
	    packet->id > PACKET_LIMIT ||
	    packet->id <= assembly->last_data ||
	    (assembly->trailer && assembly->trailer != packet->id) ||
	    (assembly->short_packet && assembly->short_packet != packet->id - 1))
		return GC_PROTOCOL;
	if (gvsp_read_image_trailer(packet->body, packet->size,
				    assembly->frame.height))
		return GC_PROTOCOL;
	if (assembly->expected_size &&
	    (assembly->expected_size - 1) / assembly->payload_size + 2 !=
	    packet->id)
		return GC_PROTOCOL;
	assembly->trailer = packet->id;
	return GC_OK;
}

static int frame_all_in(struct gige_stream *stream, const struct gvsp_packet *packet,
			 struct gc_frame *frame)
{
	const size_t headers = GVSP_IMAGE_LEADER + GVSP_IMAGE_TRAILER;
	struct gc_frame image = { .data = frame->data, .capacity = frame->capacity };
	size_t expected;
	size_t size;
	int rc;

	if (packet->size <= headers || packet->id)
		return GC_PROTOCOL;
	rc = gvsp_read_image_leader(packet, &image);
	if (rc)
		return rc;
	rc = gvsp_read_image_trailer(packet->body + GVSP_IMAGE_LEADER,
				     GVSP_IMAGE_TRAILER, image.height);
	if (rc)
		return rc;
	size = packet->size - headers;
	if (size > frame->capacity)
		return GC_BUFFER_SMALL;
	rc = gc_image_layout(&image, &expected);
	if (rc && rc != GC_UNSUPPORTED)
		return rc;
	if (!rc && expected != size)
		return GC_PROTOCOL;
	image.size = size;
	image.timestamp_hz = stream->device->timestamp_hz;
	memcpy(image.data, packet->body + headers, size);
	*frame = image;
	return GC_OK;
}

static int assemble(struct gige_stream *stream, struct assembly *assembly,
		    const struct gvsp_packet *packet, struct gc_frame *frame,
		    int *complete)
{
	int rc;

	if (packet->kind == GVSP_ALL_IN) {
		if (assembly->active && (assembly->frame.block_id == packet->block ||
		    !newer_block(assembly->frame.block_id, packet)))
			return GC_OK;
		rc = frame_all_in(stream, packet, frame);
		if (rc)
			return rc;
		if (assembly->active)
			stream->stats.incomplete++;
		*complete = 1;
		return GC_OK;
	}
	if (packet->kind == GVSP_LEADER) {
		if (assembly->active) {
			if (assembly->frame.block_id == packet->block) {
				stream->stats.duplicates++;
				return GC_OK;
			}
			if (!newer_block(assembly->frame.block_id, packet))
				return GC_OK;
			stream->stats.incomplete++;
			assembly->active = 0;
		}
		return frame_leader(stream, assembly, packet, frame);
	}
	if (!assembly->active || assembly->frame.block_id != packet->block ||
	    assembly->extended != packet->extended)
		return GC_OK;
	switch (packet->kind) {
	case GVSP_DATA:
		rc = frame_data(stream, assembly, packet);
		break;
	case GVSP_TRAILER:
		rc = frame_trailer(assembly, packet);
		break;
	default:
		return GC_PROTOCOL;
	}
	if (rc)
		return rc;

	if (assembly->trailer && assembly->received == assembly->trailer - 1) {
		if (assembly->expected_size &&
		    assembly->expected_size != assembly->received_size)
			return GC_PROTOCOL;
		assembly->frame.size = assembly->received_size;
		*frame = assembly->frame;
		*complete = 1;
	}
	return GC_OK;
}

static int packet_seen(const struct gige_stream *stream, uint32_t id)
{
	return stream->seen[id / 8] & (1U << (id % 8));
}

static void request_missing(struct gige_stream *stream, struct assembly *assembly)
{
	uint32_t first;
	uint32_t last;
	uint32_t limit;

	if (!assembly->active ||
	    !(stream->device->capability & GEV_CAP_RESEND) ||
	    assembly->resend_attempts == RESEND_ATTEMPTS ||
	    osal_remaining_ms(assembly->next_resend))
		return;

	limit = assembly->trailer ? assembly->trailer - 1 : assembly->last_data;
	for (first = 1; first <= limit && packet_seen(stream, first); first++)
		;
	last = first;
	if (first <= limit) {
		while (last < limit && !packet_seen(stream, last + 1))
			last++;
	} else if (assembly->trailer || !assembly->expected_size ||
		   (assembly->expected_size - 1) / assembly->payload_size + 1 != limit) {
		return;
	}
	assembly->resend_attempts++;
	assembly->next_resend = osal_deadline(RESEND_INTERVAL_MS);
	if (!gige_request_resend(stream->device, assembly->frame.block_id,
				 assembly->extended, first, last))
		stream->stats.resend_requests++;
}

static int stream_receive(void *context, struct gc_frame *frame,
			  unsigned int timeout_ms)
{
	struct gige_stream *stream = context;
	struct osal_endpoint source;
	struct gvsp_packet packet;
	struct assembly assembly = { 0 };
	uint64_t deadline = osal_deadline(timeout_ms);
	unsigned int remaining;
	size_t length;
	int complete = 0;
	int rc;

	osal_mutex_lock(stream->lock);
	if (stream->receiving) {
		osal_mutex_unlock(stream->lock);
		return GC_BUSY;
	}
	stream->receiving = 1;
	for (;;) {
		if (!stream->running) {
			rc = GC_STOPPED;
			break;
		}
		remaining = osal_remaining_ms(deadline);
		osal_mutex_unlock(stream->lock);
		rc = osal_udp_receive(stream->socket, &source, stream->packet,
				      sizeof(stream->packet), &length,
				      remaining > 20 ? 20 : remaining);
		osal_mutex_lock(stream->lock);
		if (!stream->running) {
			rc = GC_STOPPED;
			break;
		}
		if (rc == GC_BUFFER_SMALL) {
			stream->stats.invalid_packets++;
		} else if (!rc && source.address == stream->device->peer.address) {
			rc = gvsp_decode(stream->packet, length, &packet);
			if (!rc)
				rc = assemble(stream, &assembly, &packet, frame,
					      &complete);
			if (rc == GC_PROTOCOL)
				stream->stats.invalid_packets++;
			else if (rc || complete)
				break;
		} else if (rc && rc != GC_TIMEOUT) {
			break;
		}
		if (!osal_remaining_ms(deadline)) {
			rc = GC_TIMEOUT;
			break;
		}
		request_missing(stream, &assembly);
	}
	if (complete)
		stream->stats.completed++;
	else if (assembly.active)
		stream->stats.incomplete++;
	stream->receiving = 0;
	osal_mutex_unlock(stream->lock);
	return rc;
}

static int stream_start(void *context)
{
	struct gige_stream *stream = context;
	struct gige_device *dev = stream->device;
	uint32_t packet;
	size_t size;
	unsigned int i;
	int rc = GC_OK;

	osal_mutex_lock(stream->lock);
	if (stream->running)
		goto out;
	if (stream->receiving) {
		rc = GC_BUSY;
		goto out;
	}

	/* Remove old datagrams before the next acquisition. */
	for (i = 0; i < 4096; i++) {
		rc = osal_udp_receive(stream->socket, NULL, stream->packet,
				      sizeof(stream->packet), &size, 0);
		if (rc && rc != GC_BUFFER_SMALL)
			break;
	}
	rc = gc_device_read_register(&dev->device, GEV_PACKET_SIZE, &packet);
	if (!rc)
		rc = gc_device_write_register(&dev->device, GEV_PACKET_SIZE,
					     (packet & 0x60000000) | dev->packet_size);
	if (!rc) {
		rc = gc_device_read_register(&dev->device, GEV_PACKET_SIZE, &packet);
		if (!rc) {
			stream->packet_size = packet & 0xffff;
			if (stream->packet_size < GEV_MIN_PACKET ||
			    stream->packet_size > GEV_MAX_PACKET)
				rc = GC_PROTOCOL;
		}
	}
	if (!rc)
		rc = gc_device_write_register(&dev->device, GEV_STREAM_IP,
					     stream->local.address);
	if (!rc)
		rc = gc_device_write_register(&dev->device, GEV_STREAM_PORT,
					     stream->local.port);
	if (!rc)
		stream->running = 1;
out:
	osal_mutex_unlock(stream->lock);
	return rc;
}

static int stream_stop(void *context)
{
	struct gige_stream *stream = context;
	struct gige_device *dev = stream->device;
	int rc = GC_OK;

	osal_mutex_lock(stream->lock);
	if (stream->running) {
		stream->running = 0;
		rc = gc_device_write_register(&dev->device, GEV_STREAM_PORT, 0);
	}
	osal_mutex_unlock(stream->lock);
	return rc;
}

static int stream_stats(void *context, struct gc_stream_stats *stats)
{
	struct gige_stream *stream = context;

	osal_mutex_lock(stream->lock);
	*stats = stream->stats;
	osal_mutex_unlock(stream->lock);
	return GC_OK;
}

static void stream_close(void *context)
{
	struct gige_stream *stream = context;
	struct gige_device *dev = stream->device;

	stream_stop(stream);
	osal_udp_close(stream->socket);
	osal_mutex_destroy(stream->lock);
	osal_mutex_lock(dev->lock);
	dev->stream_open = 0;
	osal_mutex_unlock(dev->lock);
	free(stream);
}

static const struct gc_stream_ops stream_ops = {
	.close = stream_close,
	.start = stream_start,
	.stop = stream_stop,
	.receive = stream_receive,
	.stats = stream_stats,
};

int gige_stream_open(void *context, struct gc_stream **result)
{
	struct gige_device *dev = context;
	struct gige_stream *stream;
	uint32_t channels;
	int rc;

	if (dev->monitor)
		return GC_ACCESS;
	rc = gc_device_read_register(&dev->device, GEV_STREAM_CHANNELS, &channels);
	if (rc)
		return rc;
	if (!channels)
		return GC_UNSUPPORTED;
	stream = calloc(1, sizeof(*stream));
	if (!stream)
		return GC_NOMEM;
	stream->device = dev;
	stream->stream.ops = &stream_ops;
	stream->stream.context = stream;
	stream->socket = OSAL_INVALID_SOCKET;
	stream->local.address = dev->local.address;
	stream->lock = osal_mutex_create();
	if (!stream->lock) {
		rc = GC_NOMEM;
		goto free_stream;
	}
	rc = osal_udp_open(stream->local, 0, 4 * 1024 * 1024, &stream->socket);
	if (!rc)
		rc = osal_udp_local(stream->socket, &stream->local);
	if (rc)
		goto close_socket;

	osal_mutex_lock(dev->lock);
	if (dev->stream_open) {
		osal_mutex_unlock(dev->lock);
		rc = GC_BUSY;
		goto close_socket;
	}
	dev->stream_open = 1;
	osal_mutex_unlock(dev->lock);
	*result = &stream->stream;
	return GC_OK;

close_socket:
	osal_udp_close(stream->socket);
	osal_mutex_destroy(stream->lock);
free_stream:
	free(stream);
	return rc;
}
