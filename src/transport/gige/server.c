#include "gige.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_BOOTSTRAP	0x1000
#define SERVER_XML_ADDRESS	0x10000000U

struct gc_gige_server {
	osal_socket control;
	osal_socket stream;
	struct osal_endpoint owner;
	struct osal_endpoint destination;
	uint64_t owner_deadline;
	const uint8_t *xml;
	size_t xml_size;
	void *context;
	int (*read)(void *context, uint64_t address, void *data, size_t size);
	int (*write)(void *context, uint64_t address, const void *data, size_t size);
	uint8_t registers[SERVER_BOOTSTRAP];
	uint8_t packet[GEV_MAX_PACKET];
	struct gc_frame last_frame;
	uint16_t block;
};

static int resend_packets(struct gc_gige_server *server, const uint8_t *data,
			  size_t size);

static void set_reg(struct gc_gige_server *server, unsigned int address,
		    uint32_t value)
{
	gc_put_be32(server->registers + address, value);
}

static uint32_t get_reg(const struct gc_gige_server *server, unsigned int address)
{
	return gc_be32(server->registers + address);
}

static void release_owner(struct gc_gige_server *server)
{
	server->owner.port = 0;
	server->destination.port = 0;
	server->last_frame.size = 0;
	set_reg(server, GEV_CONTROL, 0);
	set_reg(server, GEV_STREAM_PORT, 0);
	set_reg(server, GEV_STREAM_IP, 0);
}

static void expire_owner(struct gc_gige_server *server)
{
	if (server->owner.port && !osal_remaining_ms(server->owner_deadline))
		release_owner(server);
}

static int is_owner(const struct gc_gige_server *server,
		    struct osal_endpoint peer)
{
	return server->owner.port && server->owner.port == peer.port &&
	       server->owner.address == peer.address;
}

static int read_memory(struct gc_gige_server *server, uint32_t address,
		       uint8_t *data, size_t size)
{
	size_t offset;
	size_t count;

	if (address < sizeof(server->registers)) {
		if (size > sizeof(server->registers) - address)
			return GC_NOT_FOUND;
		memcpy(data, server->registers + address, size);
		return GC_OK;
	}
	if (address >= SERVER_XML_ADDRESS) {
		offset = address - SERVER_XML_ADDRESS;
		if (offset >= server->xml_size ||
		    size > ((server->xml_size + 3) & ~(size_t)3) - offset)
			return GC_NOT_FOUND;
		count = server->xml_size - offset;
		if (count > size)
			count = size;
		memcpy(data, server->xml + offset, count);
		memset(data + count, 0, size - count);
		return GC_OK;
	}
	if (!server->read)
		return GC_NOT_FOUND;
	return server->read(server->context, address, data, size);
}

static int write_control(struct gc_gige_server *server,
			 struct osal_endpoint peer, uint32_t value)
{
	if (value != 0 && value != GEV_CONTROL_CONTROL &&
	    value != GEV_CONTROL_EXCLUSIVE)
		return GC_UNSUPPORTED;
	if (server->owner.port && !is_owner(server, peer))
		return GC_ACCESS;
	if (!value) {
		release_owner(server);
		return GC_OK;
	}
	server->owner = peer;
	server->owner_deadline = osal_deadline(get_reg(server, GEV_HEARTBEAT));
	set_reg(server, GEV_CONTROL, value);
	return GC_OK;
}

static int write_register(struct gc_gige_server *server,
			  struct osal_endpoint peer, uint32_t address,
			  uint32_t value)
{
	uint8_t data[4];

	if (address == GEV_CONTROL)
		return write_control(server, peer, value);
	if (!is_owner(server, peer))
		return GC_ACCESS;
	switch (address) {
	case GEV_HEARTBEAT:
		if (value < 100 || value > 60000)
			return GC_INVALID;
		server->owner_deadline = osal_deadline(value);
		break;
	case GEV_STREAM_PORT:
		if (value > UINT16_MAX)
			return GC_INVALID;
		server->destination.port = (uint16_t)value;
		break;
	case GEV_STREAM_IP:
		if (value && value != peer.address)
			return GC_UNSUPPORTED;
		server->destination.address = value;
		break;
	case GEV_PACKET_SIZE:
		if ((value & 0xffff) < GEV_MIN_PACKET ||
		    (value & 0xffff) > GEV_MAX_PACKET)
			return GC_INVALID;
		if (value & 0x80000000)
			return GC_UNSUPPORTED;
		server->last_frame.size = 0;
		break;
	default:
		if (address < SERVER_BOOTSTRAP || address >= SERVER_XML_ADDRESS)
			return GC_ACCESS;
		if (!server->write)
			return GC_ACCESS;
		gc_put_be32(data, value);
		return server->write(server->context, address, data, sizeof(data));
	}
	set_reg(server, address, value);
	return GC_OK;
}

static int write_memory(struct gc_gige_server *server, struct osal_endpoint peer,
			uint32_t address, const uint8_t *data, size_t size)
{
	if (size == 4)
		return write_register(server, peer, address, gc_be32(data));
	if (!is_owner(server, peer) || address < SERVER_BOOTSTRAP ||
	    address >= SERVER_XML_ADDRESS || !server->write)
		return GC_ACCESS;
	if (size > SERVER_XML_ADDRESS - address)
		return GC_INVALID;
	return server->write(server->context, address, data, size);
}

static int process_request(struct gc_gige_server *server,
			   struct osal_endpoint peer, uint16_t command,
			   const uint8_t *data, size_t size, uint8_t *output,
			   uint16_t *length)
{
	uint32_t address;
	uint16_t bytes;
	int rc;

	if (command == GVCP_DISCOVERY) {
		if (size)
			return GC_INVALID;
		memcpy(output, server->registers, GEV_DISCOVERY_SIZE);
		*length = GEV_DISCOVERY_SIZE;
		return GC_OK;
	}
	if (command == GVCP_RESEND)
		return is_owner(server, peer) ? resend_packets(server, data, size) :
					       GC_ACCESS;
	if (size < 4)
		return GC_INVALID;
	address = gc_be32(data);
	if (address % 4)
		return GC_INVALID;

	if (is_owner(server, peer))
		server->owner_deadline = osal_deadline(get_reg(server, GEV_HEARTBEAT));
	else if ((get_reg(server, GEV_CONTROL) & GEV_CONTROL_EXCLUSIVE) &&
		 address != GEV_CONTROL)
		return GC_ACCESS;

	switch (command) {
	case GVCP_READ_REG:
		if (size != 4)
			return GC_INVALID;
		*length = 4;
		return read_memory(server, address, output, 4);
	case GVCP_WRITE_REG:
		if (size != 8)
			return GC_INVALID;
		*length = 4;
		gc_put_be32(output, 1);
		return write_register(server, peer, address, gc_be32(data + 4));
	case GVCP_READ_MEM:
		if (size != 8)
			return GC_INVALID;
		bytes = gc_be16(data + 6);
		if (!bytes || bytes > GVCP_MEMORY_MAX || bytes % 4 ||
		    (uint64_t)address + bytes > UINT64_C(0x100000000))
			return GC_INVALID;
		*length = 4 + bytes;
		gc_put_be32(output, address);
		return read_memory(server, address, output + 4, bytes);
	case GVCP_WRITE_MEM:
		if (size <= 4 || size > 4 + GVCP_MEMORY_MAX || size % 4)
			return GC_INVALID;
		rc = write_memory(server, peer, address, data + 4, size - 4);
		*length = 4;
		gc_put_be32(output, address);
		return rc;
	default:
		return GC_UNSUPPORTED;
	}
}

static uint16_t wire_status(int rc)
{
	switch (rc) {
	case GC_OK:
		return 0;
	case GC_UNSUPPORTED:
		return 0x8001;
	case GC_INVALID:
		return 0x8002;
	case GC_NOT_FOUND:
		return 0x8003;
	case GC_ACCESS:
		return 0x8006;
	case GC_BUSY:
		return 0x8007;
	default:
		return 0x8008;
	}
}

int gc_gige_server_poll(struct gc_gige_server *server, unsigned int timeout_ms)
{
	uint8_t request[GVCP_BUFFER];
	uint8_t response[GVCP_BUFFER];
	struct osal_endpoint peer;
	uint16_t command;
	uint16_t length = 0;
	size_t size;
	int rc;

	if (!server)
		return GC_INVALID;
	rc = osal_udp_receive(server->control, &peer, request, sizeof(request),
			      &size, timeout_ms);
	expire_owner(server);
	if (rc == GC_BUFFER_SMALL)
		return GC_OK;
	if (rc)
		return rc;
	if (size < GVCP_HEADER || request[0] != GVCP_REQUEST ||
	    gc_be16(request + 4) != size - GVCP_HEADER || !gc_be16(request + 6))
		return GC_OK;

	command = gc_be16(request + 2);
	rc = process_request(server, peer, command, request + GVCP_HEADER,
			     size - GVCP_HEADER, response + GVCP_HEADER, &length);
	if (!(request[1] & GVCP_ACK_REQUIRED) && command != GVCP_DISCOVERY)
		return GC_OK;
	if (rc)
		length = 0;
	gvcp_header(response, wire_status(rc), command + 1, length,
		    gc_be16(request + 6));
	return osal_udp_send(server->control, peer, response, GVCP_HEADER + length);
}

static int send_packet(struct gc_gige_server *server, unsigned int kind,
		       uint32_t id, size_t length, int resent)
{
	gc_put_be16(server->packet, resent ? 0x0100 : 0);
	gc_put_be16(server->packet + 2, server->block);
	gc_put_be32(server->packet + 4, (uint32_t)kind << 24 | id);
	return osal_udp_send(server->stream, server->destination, server->packet,
			     GVSP_HEADER + length);
}

static size_t packet_data_size(struct gc_gige_server *server)
{
	return (get_reg(server, GEV_PACKET_SIZE) & 0xffff) -
	       GEV_UDP_OVERHEAD - GVSP_HEADER;
}

static int send_image_packet(struct gc_gige_server *server,
			     const struct gc_frame *image, uint32_t id, int resent)
{
	uint8_t *payload = server->packet + GVSP_HEADER;
	size_t block_size = packet_data_size(server);
	size_t offset;
	size_t size;
	uint32_t end = (uint32_t)((image->size - 1) / block_size + 2);

	if (id == 0) {
		memset(payload, 0, GVSP_IMAGE_LEADER);
		gc_put_be16(payload + 2, GC_PAYLOAD_IMAGE);
		gc_put_be64(payload + 4, image->timestamp);
		gc_put_be32(payload + 12, image->pixel_format);
		gc_put_be32(payload + 16, image->width);
		gc_put_be32(payload + 20, image->height);
		gc_put_be32(payload + 24, image->x_offset);
		gc_put_be32(payload + 28, image->y_offset);
		gc_put_be16(payload + 32, image->x_padding);
		gc_put_be16(payload + 34, image->y_padding);
		return send_packet(server, GVSP_LEADER, id, GVSP_IMAGE_LEADER, resent);
	}
	if (id == end) {
		gc_put_be32(payload, GC_PAYLOAD_IMAGE);
		gc_put_be32(payload + 4, image->height);
		return send_packet(server, GVSP_TRAILER, id, GVSP_IMAGE_TRAILER, resent);
	}
	offset = (id - 1) * block_size;
	size = image->size - offset;
	if (size > block_size)
		size = block_size;
	memcpy(payload, (const uint8_t *)image->data + offset, size);
	return send_packet(server, GVSP_DATA, id, size, resent);
}

static int resend_packets(struct gc_gige_server *server, const uint8_t *data,
			  size_t size)
{
	const struct gc_frame *frame = &server->last_frame;
	uint32_t first;
	uint32_t last;
	uint32_t id;
	int rc;

	if (!frame->size || !server->destination.port)
		return GC_NOT_FOUND;
	if (size != 12 || gc_be16(data) || gc_be16(data + 2) != server->block)
		return GC_INVALID;
	first = gc_be32(data + 4);
	last = gc_be32(data + 8);
	if (first > last || last > (frame->size - 1) / packet_data_size(server) + 2)
		return GC_INVALID;
	for (id = first; id <= last; id++) {
		rc = send_image_packet(server, frame, id, 1);
		if (rc)
			return rc;
	}
	return GC_OK;
}

int gc_gige_server_send(struct gc_gige_server *server, const struct gc_frame *frame)
{
	struct gc_frame image;
	struct gc_frame *last;
	uint32_t end;
	uint32_t id;
	size_t total;
	int rc;

	if (!server || !frame || !frame->data ||
	    frame->payload_type != GC_PAYLOAD_IMAGE)
		return GC_INVALID;
	expire_owner(server);
	if (!server->owner.port || !server->destination.address ||
	    !server->destination.port)
		return GC_STOPPED;

	image = *frame;
	rc = gige_image_layout(&image, &total);
	if (rc)
		return rc;
	if (total != image.size || image.capacity < total)
		return GC_INVALID;

	if (++server->block == 0)
		server->block++;
	last = &server->last_frame;
	last->size = 0;
	if (last->capacity >= total) {
		void *buffer = last->data;
		size_t capacity = last->capacity;

		memmove(buffer, frame->data, total);
		*last = image;
		last->data = buffer;
		last->capacity = capacity;
	}
	end = (uint32_t)((total - 1) / packet_data_size(server) + 2);
	for (id = 0; id <= end; id++) {
		rc = send_image_packet(server, &image, id, 0);
		if (rc)
			return rc;
	}
	return GC_OK;
}

int gc_gige_server_open(const struct gc_gige_server_config *config,
			struct gc_gige_server **result)
{
	struct osal_endpoint listen;
	struct osal_endpoint source = { 0 };
	struct gc_gige_server *server;
	uint32_t netmask;
	int rc;

	if (!config || !result || !config->address || !config->xml ||
	    !config->xml_size || config->xml_size > GC_MAX_XML_SIZE ||
	    (!config->resend_buffer && config->resend_capacity))
		return GC_INVALID;
	*result = NULL;
	rc = osal_ipv4_parse(config->address, &source.address);
	if (rc || !source.address)
		return GC_INVALID;
	rc = osal_ipv4_parse(config->netmask ? config->netmask : "255.255.255.0",
			     &netmask);
	if (rc)
		return rc;

	server = calloc(1, sizeof(*server));
	if (!server)
		return GC_NOMEM;
	server->control = OSAL_INVALID_SOCKET;
	server->stream = OSAL_INVALID_SOCKET;
	server->xml = config->xml;
	server->xml_size = config->xml_size;
	server->read = config->read;
	server->write = config->write;
	server->context = config->context;
	server->last_frame.data = config->resend_buffer;
	server->last_frame.capacity = config->resend_capacity;

	listen.address = config->broadcast_discovery ? 0 : source.address;
	listen.port = config->port ? config->port : GC_GIGE_CONTROL_PORT;
	rc = osal_udp_open(source, 0, 0, &server->stream);
	if (!rc)
		rc = osal_udp_open(listen, 0, 0, &server->control);
	if (rc) {
		gc_gige_server_close(server);
		return rc;
	}

	set_reg(server, GEV_VERSION, 1U << 16 | 2);
	set_reg(server, GEV_DEVICE_MODE, 1U << 31);
	set_reg(server, GEV_MAC_HIGH, 0x0200);
	set_reg(server, GEV_MAC_LOW, source.address);
	set_reg(server, GEV_IP_OPTIONS, 1);
	set_reg(server, GEV_IP_CONFIG, 1);
	set_reg(server, GEV_IP, source.address);
	set_reg(server, GEV_NETMASK, netmask);
	set_reg(server, GEV_NET_INTERFACES, 1);
	set_reg(server, GEV_STREAM_CHANNELS, 1);
	set_reg(server, GEV_CAPABILITY, GEV_CAP_WRITE_MEM |
		(config->resend_capacity ? GEV_CAP_RESEND : 0));
	set_reg(server, GEV_HEARTBEAT, GEV_DEFAULT_HEARTBEAT);
	set_reg(server, GEV_TICK_HI, (uint32_t)(config->timestamp_hz >> 32));
	set_reg(server, GEV_TICK_LO, (uint32_t)config->timestamp_hz);
	set_reg(server, GEV_PACKET_SIZE, GEV_DEFAULT_PACKET);
	snprintf((char *)server->registers + GEV_MANUFACTURER, 32, "%s",
		 config->manufacturer ? config->manufacturer : "");
	snprintf((char *)server->registers + GEV_MODEL, 32, "%s",
		 config->model ? config->model : "");
	snprintf((char *)server->registers + GEV_DEVICE_VERSION, 32, "1.0");
	snprintf((char *)server->registers + GEV_SERIAL, 16, "%s",
		 config->serial ? config->serial : "");
	snprintf((char *)server->registers + GEV_XML_URL0, GEV_XML_URL_SIZE,
		 "Local:camera.%s;%08x;%08x",
		 server->xml_size >= 4 && !memcmp(server->xml, "PK\003\004", 4) ?
		 "zip" : "xml", SERVER_XML_ADDRESS, (unsigned int)server->xml_size);
	*result = server;
	return GC_OK;
}

void gc_gige_server_close(struct gc_gige_server *server)
{
	if (!server)
		return;
	osal_udp_close(server->control);
	osal_udp_close(server->stream);
	free(server);
}
