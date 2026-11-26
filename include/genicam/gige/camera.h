/* GigE Vision 1.2 IPv4/UDP single-channel camera profile. */
#ifndef GENICAM_GIGE_CAMERA_H
#define GENICAM_GIGE_CAMERA_H

#include <genicam/camera.h>
#include <genicam/gige/common.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gc_gige_camera_options {
	const char *address;
	const char *netmask;
	int broadcast_discovery;
	uint16_t port;
	void *resend_buffer;
	size_t resend_capacity;
};

/* The XML, callback context, and resend workspace live until close. */
int gc_gige_camera_open(const struct gc_camera_config *config,
			 const struct gc_gige_camera_options *options,
			 size_t capacity, struct gc_camera **camera);

struct gc_gige_server;

struct gc_gige_server_config {
	const char *address;
	const char *netmask;
	int broadcast_discovery;	/* Answer discovery broadcasts on this subnet. */
	uint16_t port;
	const char *manufacturer;
	const char *model;
	const char *serial;
	const void *xml;
	size_t xml_size;
	void *context;
	int (*read)(void *context, uint64_t address, void *data, size_t size);
	int (*write)(void *context, uint64_t address, const void *data, size_t size);
	uint64_t timestamp_hz;
	/* Optional workspace for keeping the most recent frame. */
	void *resend_buffer;
	size_t resend_capacity;
};

/* XML, callback context, and resend workspace live until close. */
/* Poll and send serially. */
int gc_gige_server_open(const struct gc_gige_server_config *config,
			struct gc_gige_server **server);
int gc_gige_server_poll(struct gc_gige_server *server, unsigned int timeout_ms);
int gc_gige_server_send(struct gc_gige_server *server,
			const struct gc_frame *frame);
void gc_gige_server_close(struct gc_gige_server *server);

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_GIGE_CAMERA_H */
