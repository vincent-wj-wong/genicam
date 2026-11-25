#include "gige.h"

#include <genicam/pfnc.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_LIMIT		(256U * 1024U * 1024U)
#define DATA_LIMIT		(GEV_MIN_PACKET - GEV_UDP_OVERHEAD - GVSP_EXT_HEADER)
#define PACKET_LIMIT		(IMAGE_LIMIT / DATA_LIMIT + 2)
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

struct stream_packet {
	const uint8_t *body;
	size_t size;
	uint64_t block;
	uint32_t id;
	unsigned int kind;
	int extended;
};

int gige_image_layout(struct gc_frame *frame, size_t *size)
{
	size_t row;
	int rc;

	if (!frame->width || !frame->height)
		return GC_PROTOCOL;
	rc = gc_pfnc_row_size(frame->pixel_format, frame->width, &row);
	if (rc)
		return rc;
	if (row > IMAGE_LIMIT || frame->x_padding > IMAGE_LIMIT - row)
		return GC_PROTOCOL;
	frame->stride = row + frame->x_padding;
	if (frame->height > (IMAGE_LIMIT - frame->y_padding) / frame->stride)
		return GC_PROTOCOL;
	*size = frame->stride * frame->height + frame->y_padding;
	return GC_OK;
}

static int parse_packet(const uint8_t *data, size_t size,
			struct stream_packet *packet)
{
	size_t header;

	if (size < GVSP_HEADER || (gc_be16(data) && gc_be16(data) != 0x0100))
		return GC_PROTOCOL;
	packet->extended = !!(data[4] & GVSP_EXTENDED);
	header = packet->extended ? GVSP_EXT_HEADER : GVSP_HEADER;
	if (size < header)
		return GC_PROTOCOL;

	packet->kind = data[4] & 0x7f;
	packet->block = packet->extended ? gc_be64(data + 8) : gc_be16(data + 2);
	packet->id = packet->extended ? gc_be32(data + 16) :
					 (gc_be32(data + 4) & 0xffffff);
	packet->size = size - header;
	packet->body = data + header;
	return GC_OK;
}

static int newer_block(uint64_t previous, const struct stream_packet *packet)
{
	if (packet->extended)
		return packet->block - previous < (UINT64_C(1) << 63);
	return (uint16_t)(packet->block - previous) < 0x8000;
}

static int frame_leader(struct gige_stream *stream, struct assembly *assembly,
			const struct stream_packet *packet, struct gc_frame *frame)
{
	const uint8_t *data = packet->body;
	size_t expected = 0;
	int rc;

	if (packet->id || (packet->size < GVSP_IMAGE_LEADER &&
	    (packet->extended || packet->size != 32)))
		return GC_PROTOCOL;
	if (gc_be16(data + 2) != GC_PAYLOAD_IMAGE)
		return GC_UNSUPPORTED;
	memset(assembly, 0, sizeof(*assembly));
	assembly->frame.data = frame->data;
	assembly->frame.capacity = frame->capacity;
	assembly->frame.block_id = packet->block;
	assembly->frame.timestamp = gc_be64(data + 4);
	assembly->frame.timestamp_hz = stream->device->timestamp_hz;
	assembly->frame.pixel_format = gc_be32(data + 12);
	assembly->frame.width = gc_be32(data + 16);
	assembly->frame.height = gc_be32(data + 20);
	assembly->frame.x_offset = gc_be32(data + 24);
	assembly->frame.y_offset = gc_be32(data + 28);
	if (packet->size >= GVSP_IMAGE_LEADER) {
		assembly->frame.x_padding = gc_be16(data + 32);
		assembly->frame.y_padding = gc_be16(data + 34);
	}
	assembly->frame.payload_type = GC_PAYLOAD_IMAGE;

	rc = gige_image_layout(&assembly->frame, &expected);
	if (rc && rc != GC_UNSUPPORTED)
		return rc;
	if (expected > frame->capacity)
		return GC_BUFFER_SMALL;
	assembly->expected_size = expected;
	assembly->payload_size = stream->device->packet_size - GEV_UDP_OVERHEAD -
				 (packet->extended ? GVSP_EXT_HEADER : GVSP_HEADER);
	assembly->extended = packet->extended;
	assembly->active = 1;
	assembly->next_resend = osal_deadline(RESEND_INTERVAL_MS);
	memset(stream->seen, 0, sizeof(stream->seen));
	return GC_OK;
}

static int frame_data(struct gige_stream *stream, struct assembly *assembly,
		      const struct stream_packet *packet)
{
	uint32_t id = packet->id;
	size_t offset;
	size_t end;
	uint8_t *word;
	uint8_t bit;

	if (!id || id >= PACKET_LIMIT || !packet->size ||
	    packet->size > assembly->payload_size ||
	    id - 1 >= IMAGE_LIMIT / assembly->payload_size + 1 ||
	    (assembly->trailer && id >= assembly->trailer))
		return GC_PROTOCOL;
	offset = (id - 1) * assembly->payload_size;
	end = offset + packet->size;
	if (end > IMAGE_LIMIT)
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
			 const struct stream_packet *packet)
{
	uint32_t height;

	if (packet->size < GVSP_IMAGE_TRAILER || packet->id < 2 ||
	    packet->id > PACKET_LIMIT ||
	    gc_be16(packet->body + 2) != GC_PAYLOAD_IMAGE ||
	    packet->id <= assembly->last_data ||
	    (assembly->trailer && assembly->trailer != packet->id) ||
	    (assembly->short_packet && assembly->short_packet != packet->id - 1))
		return GC_PROTOCOL;
	height = gc_be32(packet->body + 4);
	if (height && height != assembly->frame.height)
		return GC_PROTOCOL;
	if (assembly->expected_size &&
	    (assembly->expected_size - 1) / assembly->payload_size + 2 !=
	    packet->id)
		return GC_PROTOCOL;
	assembly->trailer = packet->id;
	return GC_OK;
}

static int assemble(struct gige_stream *stream, struct assembly *assembly,
		    const struct stream_packet *packet, struct gc_frame *frame,
		    int *complete)
{
	int rc;

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
	struct stream_packet packet;
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
			rc = parse_packet(stream->packet, length, &packet);
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
