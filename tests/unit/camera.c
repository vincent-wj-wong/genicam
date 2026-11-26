#include <genicam/gige/camera.h>
#include <genicam/pfnc.h>

#include "osal/osal.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do {						\
	if (!(expr)) {							\
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);	\
		exit(EXIT_FAILURE);					\
	}								\
} while (0)

struct completion {
	const void *data;
	unsigned int count;
	int status;
};

static void completed(void *context, const struct gc_frame *frame, int status)
{
	struct completion *result = context;

	result->data = frame->data;
	result->status = status;
	result->count++;
}

int main(void)
{
	static const char xml[] = "<RegisterDescription/>";
	struct gc_camera_config config = { .xml = xml, .xml_size = sizeof(xml) - 1 };
	struct gc_gige_camera_options options = { .address = "127.0.0.1" };
	struct osal_endpoint local = { .address = 0x7f000001 };
	struct gc_camera *camera;
	struct completion first = { 0 };
	struct completion second = { 0 };
	osal_socket probe;
	uint8_t pixels[4] = { 0 };
	uint8_t other[4] = { 0 };
	struct gc_frame frame = {
		.data = pixels, .capacity = sizeof(pixels), .size = sizeof(pixels),
		.width = 4, .height = 1, .payload_type = GC_PAYLOAD_IMAGE,
		.pixel_format = GC_PFNC_MONO8,
	};

	CHECK(osal_udp_open(local, 0, 0, &probe) == GC_OK);
	CHECK(osal_udp_local(probe, &local) == GC_OK);
	osal_udp_close(probe);
	options.port = local.port;
	CHECK(gc_gige_camera_open(&config, &options, 0, &camera) == GC_INVALID);
	CHECK(!camera);
	CHECK(gc_gige_camera_open(&config, &options, 1, &camera) == GC_OK);
	CHECK(gc_camera_submit_frame(camera, &frame, completed, &first) == GC_OK);
	CHECK(gc_camera_submit_frame(camera, &frame, completed, &first) == GC_BUSY);
	frame.data = other;
	CHECK(gc_camera_submit_frame(camera, &frame, completed, &second) == GC_BUFFER_SMALL);
	CHECK(!first.count && !second.count);
	CHECK(gc_camera_poll(camera, 0) == GC_OK);
	CHECK(first.count == 1 && first.status == GC_STOPPED && first.data == pixels);
	CHECK(gc_camera_submit_frame(camera, &frame, completed, &second) == GC_OK);
	gc_camera_close(camera);
	CHECK(second.count == 1 && second.status == GC_STOPPED && second.data == other);
	return EXIT_SUCCESS;
}
