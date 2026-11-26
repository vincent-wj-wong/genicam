/* GigE Vision 1.2 GVCP/GVSP and 2.x GVSP extended IDs. */
#include "protocol.h"

int gvcp_decode(const uint8_t *buffer, size_t size, struct gvcp_packet *packet)
{
	if (size < GVCP_HEADER || size - GVCP_HEADER != gc_be16(buffer + 4))
		return GC_PROTOCOL;
	packet->status = gc_be16(buffer);
	packet->command = gc_be16(buffer + 2);
	packet->size = size - GVCP_HEADER;
	packet->id = gc_be16(buffer + 6);
	packet->body = buffer + GVCP_HEADER;
	return GC_OK;
}

int gvsp_decode(const uint8_t *buffer, size_t size, struct gvsp_packet *packet)
{
	size_t header;
	uint16_t status;

	if (size < GVSP_HEADER)
		return GC_PROTOCOL;
	status = gc_be16(buffer);
	if (status && status != 0x0100)
		return GC_PROTOCOL;
	packet->extended = !!(buffer[4] & GVSP_EXTENDED);
	header = packet->extended ? GVSP_EXT_HEADER : GVSP_HEADER;
	if (size < header)
		return GC_PROTOCOL;

	packet->kind = buffer[4] & 0x7f;
	packet->block = packet->extended ? gc_be64(buffer + 8) : gc_be16(buffer + 2);
	packet->id = packet->extended ? gc_be32(buffer + 16) :
					 (gc_be32(buffer + 4) & 0xffffff);
	packet->size = size - header;
	packet->body = buffer + header;
	return GC_OK;
}

void gvsp_write_header(uint8_t *buffer, uint16_t status, uint16_t block,
		       uint32_t id, unsigned int kind)
{
	gc_put_be16(buffer, status);
	gc_put_be16(buffer + 2, block);
	gc_put_be32(buffer + 4, (uint32_t)kind << 24 | id);
}

int gvsp_read_image_leader(const struct gvsp_packet *packet, struct gc_frame *frame)
{
	const uint8_t *data = packet->body;

	if (packet->size < 4)
		return GC_PROTOCOL;
	if (gc_be16(data + 2) != GC_PAYLOAD_IMAGE)
		return GC_UNSUPPORTED;
	/* Aravis 0.8.20 omits the padding fields in classic leader packets. */
	if (packet->size < GVSP_IMAGE_LEADER &&
	    (packet->kind != GVSP_LEADER || packet->extended || packet->size != 32))
		return GC_PROTOCOL;
	frame->block_id = packet->block;
	frame->timestamp = gc_be64(data + 4);
	frame->pixel_format = gc_be32(data + 12);
	frame->width = gc_be32(data + 16);
	frame->height = gc_be32(data + 20);
	frame->x_offset = gc_be32(data + 24);
	frame->y_offset = gc_be32(data + 28);
	frame->x_padding = 0;
	frame->y_padding = 0;
	if (packet->size >= GVSP_IMAGE_LEADER) {
		frame->x_padding = gc_be16(data + 32);
		frame->y_padding = gc_be16(data + 34);
	}
	frame->payload_type = GC_PAYLOAD_IMAGE;
	return GC_OK;
}

void gvsp_write_image_leader(uint8_t *buffer, const struct gc_frame *frame)
{
	gc_put_be16(buffer, 0);
	gc_put_be16(buffer + 2, GC_PAYLOAD_IMAGE);
	gc_put_be64(buffer + 4, frame->timestamp);
	gc_put_be32(buffer + 12, frame->pixel_format);
	gc_put_be32(buffer + 16, frame->width);
	gc_put_be32(buffer + 20, frame->height);
	gc_put_be32(buffer + 24, frame->x_offset);
	gc_put_be32(buffer + 28, frame->y_offset);
	gc_put_be16(buffer + 32, frame->x_padding);
	gc_put_be16(buffer + 34, frame->y_padding);
}

int gvsp_read_image_trailer(const uint8_t *data, size_t size, uint32_t height)
{
	uint32_t transmitted_height;

	if (size < GVSP_IMAGE_TRAILER || gc_be16(data + 2) != GC_PAYLOAD_IMAGE)
		return GC_PROTOCOL;
	transmitted_height = gc_be32(data + 4);
	/* Some legacy transmitters leave the image height unset. */
	return !transmitted_height || transmitted_height == height ? GC_OK : GC_PROTOCOL;
}

void gvsp_write_image_trailer(uint8_t *buffer, uint32_t height)
{
	gc_put_be32(buffer, GC_PAYLOAD_IMAGE);
	gc_put_be32(buffer + 4, height);
}
