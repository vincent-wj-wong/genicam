/* GigE Vision 1.2 camera transport binding. */
#include <genicam/gige/camera.h>

#include "camera/internal.h"

static int poll_server(void *context, unsigned int timeout_ms)
{
	return gc_gige_server_poll(context, timeout_ms);
}

static int send_frame(void *context, const struct gc_frame *frame)
{
	return gc_gige_server_send(context, frame);
}

static void close_server(void *context)
{
	gc_gige_server_close(context);
}

static const struct gc_camera_ops ops = {
	.poll = poll_server,
	.send = send_frame,
	.close = close_server,
};

int gc_gige_camera_open(const struct gc_camera_config *config,
			 const struct gc_gige_camera_options *options,
			 size_t capacity, struct gc_camera **camera)
{
	struct gc_gige_server_config settings;
	struct gc_gige_server *server;
	int rc;

	if (!camera)
		return GC_INVALID;
	*camera = NULL;
	if (!config || !options)
		return GC_INVALID;

	settings = (struct gc_gige_server_config) {
		.address = options->address,
		.netmask = options->netmask,
		.broadcast_discovery = options->broadcast_discovery,
		.port = options->port,
		.manufacturer = config->manufacturer,
		.model = config->model,
		.serial = config->serial,
		.xml = config->xml,
		.xml_size = config->xml_size,
		.context = config->context,
		.read = config->read,
		.write = config->write,
		.timestamp_hz = config->timestamp_hz,
		.resend_buffer = options->resend_buffer,
		.resend_capacity = options->resend_capacity,
	};

	rc = gc_gige_server_open(&settings, &server);
	if (rc)
		return rc;
	rc = gc_camera_create(&ops, server, capacity, camera);
	if (rc)
		gc_gige_server_close(server);
	return rc;
}
