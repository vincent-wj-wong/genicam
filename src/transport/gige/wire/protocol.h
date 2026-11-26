/* GigE Vision 1.2 bootstrap and GVCP. GVSP extended IDs are from 2.x. */
#ifndef GC_GIGE_PROTOCOL_H
#define GC_GIGE_PROTOCOL_H

#include "common/internal.h"

#define GVCP_HEADER		8
#define GVCP_MEMORY_MAX		512
#define GVCP_BUFFER		(GVCP_HEADER + 4 + GVCP_MEMORY_MAX)
#define GVCP_REQUEST		0x42
#define GVCP_ACK_REQUIRED	0x01
#define GVCP_DISCOVERY		0x0002
#define GVCP_RESEND		0x0040
#define GVCP_READ_REG		0x0080
#define GVCP_WRITE_REG		0x0082
#define GVCP_READ_MEM		0x0084
#define GVCP_WRITE_MEM		0x0086
#define GVCP_PENDING_ACK	0x0089

#define GEV_VERSION		0x0000
#define GEV_DEVICE_MODE		0x0004
#define GEV_MAC_HIGH		0x0008
#define GEV_MAC_LOW		0x000c
#define GEV_IP_OPTIONS		0x0010
#define GEV_IP_CONFIG		0x0014
#define GEV_IP			0x0024
#define GEV_NETMASK		0x0034
#define GEV_MANUFACTURER	0x0048
#define GEV_MODEL		0x0068
#define GEV_DEVICE_VERSION	0x0088
#define GEV_SERIAL		0x00d8
#define GEV_DISCOVERY_SIZE	0x00f8
#define GEV_XML_URL0		0x0200
#define GEV_XML_URL1		0x0400
#define GEV_XML_URL_SIZE	512
#define GEV_NET_INTERFACES	0x0600
#define GEV_STREAM_CHANNELS	0x0904
#define GEV_CAPABILITY		0x0934
#define GEV_HEARTBEAT		0x0938
#define GEV_TICK_HI		0x093c
#define GEV_TICK_LO		0x0940
#define GEV_CONTROL		0x0a00
#define GEV_STREAM_PORT		0x0d00
#define GEV_PACKET_SIZE		0x0d04
#define GEV_PACKET_DELAY	0x0d08
#define GEV_STREAM_IP		0x0d18

#define GEV_CONTROL_CONTROL	2
#define GEV_CONTROL_EXCLUSIVE	1
#define GEV_CAP_WRITE_MEM	(1U << 1)
#define GEV_CAP_RESEND		(1U << 2)
#define GEV_DEFAULT_HEARTBEAT	3000
#define GEV_DEFAULT_PACKET	1500
#define GEV_MAX_PACKET		9000
#define GEV_MIN_PACKET		576
#define GEV_UDP_OVERHEAD	28

#define GVSP_HEADER		8
#define GVSP_EXT_HEADER		20
#define GVSP_IMAGE_LEADER	36
#define GVSP_IMAGE_TRAILER	8
#define GVSP_LEADER		1
#define GVSP_TRAILER		2
#define GVSP_DATA		3
#define GVSP_ALL_IN		4
#define GVSP_EXTENDED		0x80

struct gvcp_packet {
	const uint8_t *body;
	size_t size;
	uint16_t status;
	uint16_t command;
	uint16_t id;
};

struct gvsp_packet {
	const uint8_t *body;
	size_t size;
	uint64_t block;
	uint32_t id;
	unsigned int kind;
	int extended;
};

int gvcp_decode(const uint8_t *buffer, size_t size, struct gvcp_packet *packet);
int gvsp_decode(const uint8_t *buffer, size_t size, struct gvsp_packet *packet);
void gvsp_write_header(uint8_t *buffer, uint16_t status, uint16_t block,
		       uint32_t id, unsigned int kind);
int gvsp_read_image_leader(const struct gvsp_packet *packet, struct gc_frame *frame);
void gvsp_write_image_leader(uint8_t *buffer, const struct gc_frame *frame);
int gvsp_read_image_trailer(const uint8_t *data, size_t size, uint32_t height);
void gvsp_write_image_trailer(uint8_t *buffer, uint32_t height);

static inline void gvcp_header(uint8_t *buffer, uint16_t status,
			       uint16_t command, uint16_t size, uint16_t id)
{
	gc_put_be16(buffer, status);
	gc_put_be16(buffer + 2, command);
	gc_put_be16(buffer + 4, size);
	gc_put_be16(buffer + 6, id);
}

#endif /* GC_GIGE_PROTOCOL_H */
