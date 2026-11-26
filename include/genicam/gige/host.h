/* GigE Vision 1.2 control and 1.2/2.x GVSP image reception. */
#ifndef GENICAM_GIGE_HOST_H
#define GENICAM_GIGE_HOST_H

#include <genicam/gige/common.h>
#include <genicam/host.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_GIGE_HOST_H */
