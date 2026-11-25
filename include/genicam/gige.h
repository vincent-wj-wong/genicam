#ifndef GENICAM_GIGE_H
#define GENICAM_GIGE_H

#include <genicam/genicam.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_GIGE_CONTROL_PORT 3956
#define GC_GIGE_IPV4_SIZE 16

struct gc_gige_device_info {
	char address[GC_GIGE_IPV4_SIZE];
	char interface[GC_GIGE_IPV4_SIZE];
	char manufacturer[33];
	char model[33];
	char serial[17];
	uint16_t port;
	uint16_t version_major;
	uint16_t version_minor;
};

struct gc_gige_options {
	const char *interface;
	const char *address;
	uint16_t port;
	uint16_t packet_size;
	unsigned int timeout_ms;
	int monitor;
};

/* A NULL address broadcasts on the selected or available interfaces. */
int gc_gige_discover(const struct gc_gige_options *options,
		     struct gc_gige_device_info *devices, size_t capacity,
		     size_t *count);
int gc_gige_open(const struct gc_gige_options *options,
		 struct gc_device **device);

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

#endif /* GENICAM_GIGE_H */
