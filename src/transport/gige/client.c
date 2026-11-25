#include "gige.h"

#include <stdlib.h>
#include <string.h>

#define DISCOVERY_INTERFACES 32
#define CONTROL_TIMEOUT 250

struct discovery_socket {
	osal_socket socket;
	struct osal_endpoint local;
};

static void discovery_info(struct gc_gige_device_info *info,
			   const uint8_t *data,
			   struct osal_endpoint local, struct osal_endpoint peer)
{
	memset(info, 0, sizeof(*info));
	osal_ipv4_format(gc_be32(data + GEV_IP), info->address);
	osal_ipv4_format(local.address, info->interface);
	memcpy(info->manufacturer, data + GEV_MANUFACTURER, 32);
	memcpy(info->model, data + GEV_MODEL, 32);
	memcpy(info->serial, data + GEV_SERIAL, 16);
	info->port = peer.port;
	info->version_major = gc_be16(data + GEV_VERSION);
	info->version_minor = gc_be16(data + GEV_VERSION + 2);
}

static int discovery_send(struct discovery_socket *entry,
			  struct osal_endpoint peer, int broadcast, uint16_t id)
{
	uint8_t command[GVCP_HEADER];
	int rc;

	rc = osal_udp_open(entry->local, broadcast, 0, &entry->socket);
	if (rc)
		return rc;
	if (!broadcast) {
		rc = osal_udp_connect(entry->socket, peer);
		if (!rc)
			rc = osal_udp_local(entry->socket, &entry->local);
	}
	if (!rc) {
		gvcp_header(command, GVCP_REQUEST << 8 | GVCP_ACK_REQUIRED,
			    GVCP_DISCOVERY, 0, id);
		rc = osal_udp_send(entry->socket,
				   broadcast ? peer : (struct osal_endpoint){ 0 },
				   command, sizeof(command));
	}
	if (rc)
		osal_udp_close(entry->socket);
	return rc;
}

int gc_gige_discover(const struct gc_gige_options *options,
		     struct gc_gige_device_info *devices, size_t capacity,
		     size_t *count)
{
	struct osal_interface interfaces[DISCOVERY_INTERFACES];
	struct discovery_socket sockets[DISCOVERY_INTERFACES];
	struct osal_endpoint peer = { .port = GC_GIGE_CONTROL_PORT };
	struct osal_endpoint source;
	struct gc_gige_device_info info;
	uint8_t ack[GVCP_BUFFER];
	uint32_t local = 0;
	size_t iface_count = 0;
	size_t open_count = 0;
	size_t length;
	size_t i;
	size_t j;
	uint64_t deadline;
	unsigned int ms;
	unsigned int timeout = CONTROL_TIMEOUT;
	int directed = 0;
	int truncated = 0;
	int rc;

	if (!count || (!devices && capacity))
		return GC_INVALID;
	*count = 0;
	if (options) {
		if (options->address) {
			rc = osal_ipv4_parse(options->address, &peer.address);
			if (rc || !peer.address)
				return GC_INVALID;
			directed = 1;
		}
		if (options->interface &&
		    osal_ipv4_parse(options->interface, &local))
			return GC_INVALID;
		if (options->port)
			peer.port = options->port;
		if (options->timeout_ms)
			timeout = options->timeout_ms;
	}
	if (directed) {
		interfaces[0].address = local;
		iface_count = 1;
	} else {
		rc = osal_ipv4_interfaces(interfaces, DISCOVERY_INTERFACES,
					  &iface_count);
		if (rc)
			return rc;
	}
	for (i = 0; i < iface_count; i++) {
		struct discovery_socket *entry = &sockets[open_count];

		if (!directed) {
			if (local && interfaces[i].address != local)
				continue;
			peer.address = interfaces[i].address | ~interfaces[i].netmask;
		}
		entry->local.address = interfaces[i].address;
		entry->local.port = 0;
		rc = discovery_send(entry, peer, !directed, (uint16_t)(open_count + 1));
		if (!rc)
			open_count++;
	}
	if (!open_count)
		return GC_IO;

	deadline = osal_deadline(timeout);
	while ((ms = osal_remaining_ms(deadline))) {
		for (i = 0; i < open_count && ms; i++) {
			rc = osal_udp_receive(sockets[i].socket, &source, ack,
					      sizeof(ack), &length, ms > 10 ? 10 : ms);
			ms = osal_remaining_ms(deadline);
			if (rc || length < GVCP_HEADER + GEV_DISCOVERY_SIZE ||
			    gc_be16(ack) || gc_be16(ack + 2) != GVCP_DISCOVERY + 1 ||
			    gc_be16(ack + 4) != length - GVCP_HEADER ||
			    gc_be16(ack + 6) != i + 1 || source.port != peer.port)
				continue;
			discovery_info(&info, ack + GVCP_HEADER, sockets[i].local,
				       source);
			for (j = 0; j < *count; j++) {
				if (!strcmp(devices[j].address, info.address) &&
				    !strcmp(devices[j].interface, info.interface))
					break;
			}
			if (j < *count)
				continue;
			if (*count < capacity)
				devices[(*count)++] = info;
			else
				truncated = 1;
		}
	}
	for (i = 0; i < open_count; i++)
		osal_udp_close(sockets[i].socket);

	return truncated ? GC_BUFFER_SMALL : GC_OK;
}

static int gvcp_error(uint16_t status)
{
	switch (status) {
	case 0x8001:
		return GC_UNSUPPORTED;
	case 0x8002:
	case 0x8005:
		return GC_INVALID;
	case 0x8003:
	case 0x8004:
	case 0x8006:
		return GC_ACCESS;
	case 0x8007:
		return GC_BUSY;
	default:
		return GC_PROTOCOL;
	}
}

static uint16_t next_request(struct gige_device *dev)
{
	if (++dev->request == 0)
		dev->request++;
	return dev->request;
}

int gige_request_resend(struct gige_device *dev, uint64_t block, int extended,
			uint32_t first, uint32_t last)
{
	struct osal_endpoint connected = { 0 };
	uint8_t packet[GVCP_HEADER + 20] = { 0 };
	uint16_t length = extended ? 20 : 12;
	int rc;

	osal_mutex_lock(dev->lock);
	gvcp_header(packet, GVCP_REQUEST << 8 | (extended ? 0x10 : 0),
		    GVCP_RESEND, length, next_request(dev));
	if (extended)
		gc_put_be64(packet + GVCP_HEADER + 12, block);
	else
		gc_put_be16(packet + GVCP_HEADER + 2, (uint16_t)block);
	gc_put_be32(packet + GVCP_HEADER + 4, first);
	gc_put_be32(packet + GVCP_HEADER + 8, last);
	rc = osal_udp_send(dev->control, connected, packet, GVCP_HEADER + length);
	osal_mutex_unlock(dev->lock);
	return rc;
}

static int transaction(struct gige_device *dev, uint16_t command,
		       const uint8_t *request, uint16_t request_size,
		       uint8_t *response, uint16_t response_size)
{
	uint8_t tx[GVCP_BUFFER];
	uint8_t rx[GVCP_BUFFER];
	struct osal_endpoint connected = { 0 };
	unsigned int attempts = command == GVCP_READ_REG ||
				command == GVCP_READ_MEM ? 2 : 1;
	uint64_t deadline;
	unsigned int remaining;
	size_t received;
	uint16_t ack;
	uint16_t status;
	uint16_t id;
	int rc;

	id = next_request(dev);
	gvcp_header(tx, GVCP_REQUEST << 8 | GVCP_ACK_REQUIRED, command,
		    request_size, id);
	memcpy(tx + GVCP_HEADER, request, request_size);

	while (attempts--) {
		rc = osal_udp_send(dev->control, connected, tx,
				   GVCP_HEADER + request_size);
		if (rc)
			return rc;
		deadline = osal_deadline(dev->timeout);
		while ((remaining = osal_remaining_ms(deadline))) {
			rc = osal_udp_receive(dev->control, NULL, rx, sizeof(rx),
					      &received, remaining);
			if (rc == GC_TIMEOUT)
				break;
			if (rc == GC_BUFFER_SMALL)
				continue;
			if (rc)
				return rc;
			if (received < GVCP_HEADER || gc_be16(rx + 6) != id)
				continue;
			ack = gc_be16(rx + 2);
			status = gc_be16(rx);
			if (ack == GVCP_PENDING_ACK && !status)
				continue;
			if (ack != command + 1)
				continue;
			if (status)
				return gvcp_error(status);
			if (gc_be16(rx + 4) != response_size ||
			    received != (size_t)GVCP_HEADER + response_size)
				return GC_PROTOCOL;
			memcpy(response, rx + GVCP_HEADER, response_size);
			return GC_OK;
		}
	}
	return GC_TIMEOUT;
}

static int read_reg_locked(struct gige_device *dev, uint32_t address,
			   uint32_t *value)
{
	uint8_t request[4];
	uint8_t response[4];
	int rc;

	gc_put_be32(request, address);
	rc = transaction(dev, GVCP_READ_REG, request, sizeof(request),
			 response, sizeof(response));
	if (!rc)
		*value = gc_be32(response);
	return rc;
}

static int write_reg_locked(struct gige_device *dev, uint32_t address,
			    uint32_t value)
{
	uint8_t request[8];
	uint8_t response[4];

	gc_put_be32(request, address);
	gc_put_be32(request + 4, value);
	return transaction(dev, GVCP_WRITE_REG, request, sizeof(request),
			   response, sizeof(response));
}

static int gige_read_reg(void *context, uint64_t address, uint32_t *value)
{
	struct gige_device *dev = context;
	int rc;

	if (address > UINT32_MAX || address % 4)
		return GC_INVALID;
	osal_mutex_lock(dev->lock);
	rc = read_reg_locked(dev, (uint32_t)address, value);
	osal_mutex_unlock(dev->lock);
	return rc;
}

static int gige_write_reg(void *context, uint64_t address, uint32_t value)
{
	struct gige_device *dev = context;
	int rc;

	if (address > UINT32_MAX || address % 4)
		return GC_INVALID;
	if (dev->monitor)
		return GC_ACCESS;
	osal_mutex_lock(dev->lock);
	rc = write_reg_locked(dev, (uint32_t)address, value);
	osal_mutex_unlock(dev->lock);
	return rc;
}

static int read_mem_locked(struct gige_device *dev, uint32_t address,
			   uint8_t *response, uint16_t size)
{
	uint8_t request[8] = { 0 };
	int rc;

	gc_put_be32(request, address);
	gc_put_be16(request + 6, size);
	rc = transaction(dev, GVCP_READ_MEM, request, sizeof(request),
			 response, 4 + size);
	if (!rc && gc_be32(response) != address)
		rc = GC_PROTOCOL;
	return rc;
}

static int gige_memory(struct gige_device *dev, uint64_t address, uint8_t *out,
		       const uint8_t *in, size_t size)
{
	uint8_t response[4 + GVCP_MEMORY_MAX];
	uint8_t ack[4];
	unsigned int skip;
	size_t chunk;
	uint16_t aligned;
	uint32_t base;
	int rc = GC_OK;

	if (address > UINT32_MAX || size - 1 > UINT32_MAX - address)
		return GC_INVALID;
	if (in && dev->monitor)
		return GC_ACCESS;

	osal_mutex_lock(dev->lock);
	while (size) {
		base = (uint32_t)address & ~3U;
		skip = (unsigned int)address & 3;
		chunk = size;
		if (chunk > GVCP_MEMORY_MAX - skip)
			chunk = GVCP_MEMORY_MAX - skip;
		aligned = (uint16_t)((skip + chunk + 3) & ~3U);
		if (out || chunk != aligned) {
			rc = read_mem_locked(dev, base, response, aligned);
			if (rc)
				break;
		}
		if (out) {
			memcpy(out, response + 4 + skip, chunk);
			out += chunk;
		} else {
			gc_put_be32(response, base);
			memcpy(response + 4 + skip, in, chunk);
			if (aligned == 4)
				rc = write_reg_locked(dev, base, gc_be32(response + 4));
			else
				rc = transaction(dev, GVCP_WRITE_MEM, response,
						 4 + aligned, ack, sizeof(ack));
			if (rc)
				break;
			in += chunk;
		}
		address += chunk;
		size -= chunk;
	}
	osal_mutex_unlock(dev->lock);
	return rc;
}

static int gige_read(void *context, uint64_t address, void *data, size_t size)
{
	return gige_memory(context, address, data, NULL, size);
}

static int gige_write(void *context, uint64_t address, const void *data,
		      size_t size)
{
	return gige_memory(context, address, NULL, data, size);
}

static int parse_xml_url(const char *url, uint64_t *address, size_t *size)
{
	const char *begin;
	char *end;
	unsigned long long offset;
	unsigned long long length;

	if (strncmp(url, "Local:", 6) && strncmp(url, "local:", 6))
		return GC_UNSUPPORTED;
	begin = strchr(url + 6, ';');
	if (!begin || begin[1] == '-' || begin[1] == '+')
		return GC_PROTOCOL;
	offset = strtoull(++begin, &end, 16);
	if (begin == end || *end != ';' || offset > UINT32_MAX)
		return GC_PROTOCOL;
	begin = end + 1;
	if (*begin == '-' || *begin == '+')
		return GC_PROTOCOL;
	length = strtoull(begin, &end, 16);
	if (begin == end || (*end && *end != ';') || !length ||
	    length > GC_MAX_XML_SIZE || length - 1 > UINT32_MAX - offset)
		return GC_PROTOCOL;
	*address = (uint64_t)offset;
	*size = (size_t)length;
	return GC_OK;
}

static int gige_xml(void *context, void **data, size_t *size)
{
	struct gige_device *dev = context;
	char url[GEV_XML_URL_SIZE + 1] = { 0 };
	uint64_t address;
	void *bytes;
	int rc;

	rc = gige_read(dev, GEV_XML_URL0, url, GEV_XML_URL_SIZE);
	if (!rc)
		rc = parse_xml_url(url, &address, size);
	if (rc) {
		rc = gige_read(dev, GEV_XML_URL1, url, GEV_XML_URL_SIZE);
		if (!rc)
			rc = parse_xml_url(url, &address, size);
	}
	if (rc)
		return rc;
	bytes = malloc(*size);
	if (!bytes)
		return GC_NOMEM;
	rc = gige_read(dev, address, bytes, *size);
	if (rc)
		free(bytes);
	else
		*data = bytes;
	return rc;
}

static void heartbeat(void *context)
{
	struct gige_device *dev = context;
	unsigned int interval = dev->heartbeat / 3;
	uint64_t next = osal_deadline(interval);
	uint32_t privilege;
	unsigned int ms;

	for (;;) {
		osal_mutex_lock(dev->lock);
		if (dev->stopping) {
			osal_mutex_unlock(dev->lock);
			return;
		}
		ms = osal_remaining_ms(next);
		if (!ms) {
			read_reg_locked(dev, GEV_CONTROL, &privilege);
			next = osal_deadline(interval);
		}
		osal_mutex_unlock(dev->lock);
		if (ms)
			osal_msleep(ms > 20 ? 20 : ms);
	}
}

static void gige_close(void *context)
{
	struct gige_device *dev = context;

	if (dev->heartbeat_thread) {
		osal_mutex_lock(dev->lock);
		dev->stopping = 1;
		osal_mutex_unlock(dev->lock);
		osal_thread_join(dev->heartbeat_thread);
		gige_write_reg(dev, GEV_CONTROL, 0);
	}
	osal_udp_close(dev->control);
	osal_mutex_destroy(dev->lock);
	free(dev);
}

static const struct gc_device_ops device_ops = {
	.close = gige_close,
	.read = gige_read,
	.write = gige_write,
	.read_reg = gige_read_reg,
	.write_reg = gige_write_reg,
	.xml = gige_xml,
	.stream_open = gige_stream_open,
};

int gc_gige_open(const struct gc_gige_options *options,
		 struct gc_device **device)
{
	struct gige_device *dev;
	uint32_t high;
	uint32_t low;
	uint32_t value;
	int rc;

	if (!options || !options->address || !device)
		return GC_INVALID;
	*device = NULL;
	if (options->packet_size &&
	    (options->packet_size < GEV_MIN_PACKET ||
	     options->packet_size > GEV_MAX_PACKET))
		return GC_INVALID;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return GC_NOMEM;
	dev->control = OSAL_INVALID_SOCKET;
	dev->lock = osal_mutex_create();
	if (!dev->lock) {
		free(dev);
		return GC_NOMEM;
	}
	rc = osal_ipv4_parse(options->address, &dev->peer.address);
	if (rc || !dev->peer.address) {
		rc = GC_INVALID;
		goto error;
	}
	if (options->interface) {
		rc = osal_ipv4_parse(options->interface, &dev->local.address);
		if (rc)
			goto error;
	}
	dev->peer.port = options->port ? options->port : GC_GIGE_CONTROL_PORT;
	dev->timeout = options->timeout_ms ? options->timeout_ms : CONTROL_TIMEOUT;
	dev->packet_size = options->packet_size ? options->packet_size :
						  GEV_DEFAULT_PACKET;
	dev->monitor = options->monitor;
	dev->device.ops = &device_ops;
	dev->device.context = dev;
	rc = osal_udp_open(dev->local, 0, 0, &dev->control);
	if (!rc)
		rc = osal_udp_connect(dev->control, dev->peer);
	if (!rc)
		rc = osal_udp_local(dev->control, &dev->local);
	if (!rc)
		rc = gige_read_reg(dev, GEV_VERSION, &value);
	if (rc)
		goto error;
	gige_read_reg(dev, GEV_CAPABILITY, &dev->capability);
	if (!gige_read_reg(dev, GEV_TICK_HI, &high) &&
	    !gige_read_reg(dev, GEV_TICK_LO, &low))
		dev->timestamp_hz = (uint64_t)high << 32 | low;
	if (!dev->monitor) {
		rc = gige_write_reg(dev, GEV_CONTROL, GEV_CONTROL_CONTROL);
		if (rc)
			goto error;
		dev->heartbeat = GEV_DEFAULT_HEARTBEAT;
		if (!gige_read_reg(dev, GEV_HEARTBEAT, &value) && value >= 3)
			dev->heartbeat = value;
		dev->heartbeat_thread = osal_thread_create(heartbeat, dev);
		if (!dev->heartbeat_thread) {
			gige_write_reg(dev, GEV_CONTROL, 0);
			rc = GC_NOMEM;
			goto error;
		}
	}
	*device = &dev->device;
	return GC_OK;

error:
	gige_close(dev);
	return rc;
}
