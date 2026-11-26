#include <genicam/gige/host.h>
#include <genicam/pfnc.h>
#include <genicam/sfnc.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CAPTURE (64U * 1024U * 1024U)
#define MAX_DEVICES 32

static int discover(const struct gc_gige_options *options,
		    struct gc_gige_device_info *devices, size_t capacity,
		    size_t *count)
{
	size_t i;
	int rc;

	rc = gc_gige_discover(options, devices, capacity, count);
	if (rc && rc != GC_BUFFER_SMALL)
		return rc;
	for (i = 0; i < *count; i++) {
		printf("%s:%u on %s  %s %s [%s]\n", devices[i].address,
		       (unsigned int)devices[i].port, devices[i].interface,
		       devices[i].manufacturer, devices[i].model,
		       devices[i].serial);
	}
	return *count ? GC_OK : GC_NOT_FOUND;
}

static int save_pgm(const char *path, const struct gc_frame *frame)
{
	FILE *file;
	uint32_t row;
	int rc = GC_OK;

	if (frame->pixel_format != GC_PFNC_MONO8)
		return GC_UNSUPPORTED;
	file = fopen(path, "wb");
	if (!file)
		return GC_IO;
	if (fprintf(file, "P5\n%" PRIu32 " %" PRIu32 "\n255\n",
		    frame->width, frame->height) < 0)
		rc = GC_IO;
	for (row = 0; !rc && row < frame->height; row++) {
		const uint8_t *start = (const uint8_t *)frame->data + row * frame->stride;

		if (fwrite(start, 1, frame->width, file) != frame->width)
			rc = GC_IO;
	}
	if (fclose(file) && !rc)
		rc = GC_IO;
	return rc;
}

static int capture(struct gc_gige_options *options, unsigned int frames,
		    const char *path)
{
	struct gc_gige_device_info devices[MAX_DEVICES];
	struct gc_device *device = NULL;
	struct gc_nodemap *map = NULL;
	struct gc_stream *stream = NULL;
	struct gc_stream_queue *queue = NULL;
	struct gc_frame frame = { 0 };
	struct gc_stream_stats stats;
	const char *exposure_name = GC_SFNC_EXPOSURE_TIME;
	const char *format;
	char pixel[32];
	double exposure;
	int64_t payload;
	size_t count;
	unsigned int i;
	int rc;
	int cleanup_rc;
	int acquiring = 0;

	rc = discover(options, devices, MAX_DEVICES, &count);
	if (rc)
		goto out;
	options->interface = devices[0].interface;
	rc = gc_gige_open(options, &device);
	if (rc)
		goto out;
	rc = gc_nodemap_open(device, &map);
	if (rc)
		goto out;

	rc = gc_feature_set_float(map, exposure_name, 2000.0);
	if (rc == GC_NOT_FOUND) {
		exposure_name = "ExposureTimeAbs";
		rc = gc_feature_set_float(map, exposure_name, 2000.0);
	}
	if (!rc)
		rc = gc_feature_get_float(map, exposure_name, &exposure);
	if (!rc)
		rc = gc_feature_get_string(map, GC_SFNC_PIXEL_FORMAT, pixel, sizeof(pixel));
	if (!rc)
		rc = gc_feature_get_integer(map, GC_SFNC_PAYLOAD_SIZE, &payload);
	if (rc)
		goto out;
	if (payload <= 0 || payload > MAX_CAPTURE) {
		rc = GC_INVALID;
		goto out;
	}
	printf("ExposureTime %.1f us, PixelFormat %s, PayloadSize %" PRId64 "\n",
	       exposure, pixel, payload);

	frame.capacity = (size_t)payload;
	frame.data = malloc(frame.capacity);
	if (!frame.data) {
		rc = GC_NOMEM;
		goto out;
	}
	rc = gc_stream_open(device, &stream);
	if (!rc)
		rc = gc_stream_queue_open(stream, 1, &queue);
	if (!rc)
		rc = gc_stream_queue_push(queue, frame.data, frame.capacity);
	if (!rc)
		rc = gc_stream_start(stream);
	if (!rc)
		rc = gc_feature_execute(map, GC_SFNC_ACQUISITION_START);
	if (rc)
		goto out;
	acquiring = 1;

	for (i = 0; i < frames; i++) {
		rc = gc_stream_queue_receive(queue, &frame, 2000);
		if (rc)
			goto out;
		format = gc_pfnc_name(frame.pixel_format);
		printf("Frame %" PRIu64 "  %" PRIu32 "x%" PRIu32
		       "  %s  %zu bytes  ticks=%" PRIu64 "  rate=%" PRIu64 " Hz\n",
		       frame.block_id, frame.width, frame.height,
		       format ? format : "unknown", frame.size,
		       frame.timestamp, frame.timestamp_hz);
		if (i + 1 < frames) {
			rc = gc_stream_queue_push(queue, frame.data, frame.capacity);
			if (rc)
				goto out;
		}
	}
	if (path) {
		rc = save_pgm(path, &frame);
		if (rc)
			goto out;
	}
	rc = gc_stream_get_stats(stream, &stats);
	if (!rc)
		printf("Completed %" PRIu64 ", incomplete %" PRIu64 "\n",
		       stats.completed, stats.incomplete);
out:
	if (acquiring) {
		cleanup_rc = gc_feature_execute(map, GC_SFNC_ACQUISITION_STOP);
		if (!rc)
			rc = cleanup_rc;
	}
	if (stream) {
		cleanup_rc = gc_stream_stop(stream);
		if (!rc)
			rc = cleanup_rc;
	}
	gc_stream_queue_close(queue);
	gc_stream_close(stream);
	free(frame.data);
	gc_nodemap_close(map);
	gc_device_close(device);
	return rc;
}

static unsigned long parse_number(const char *arg, unsigned long max)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(arg, &end, 10);
	return errno || end == arg || *end || value > max ? 0 : value;
}

static int print_feature(struct gc_nodemap *map, const char *name)
{
	enum gc_feature_type type;
	char text[4097];
	int64_t integer;
	double decimal;
	int rc;

	rc = gc_feature_get_type(map, name, &type);
	if (rc)
		return rc;
	switch (type) {
	case GC_FEATURE_INTEGER:
		rc = gc_feature_get_integer(map, name, &integer);
		if (!rc)
			printf("%s = %" PRId64 "\n", name, integer);
		return rc;
	case GC_FEATURE_BOOLEAN:
		rc = gc_feature_get_integer(map, name, &integer);
		if (!rc)
			printf("%s = %s\n", name, integer ? "true" : "false");
		return rc;
	case GC_FEATURE_FLOAT:
		rc = gc_feature_get_float(map, name, &decimal);
		if (!rc)
			printf("%s = %.17g\n", name, decimal);
		return rc;
	case GC_FEATURE_ENUMERATION:
	case GC_FEATURE_STRING:
		rc = gc_feature_get_string(map, name, text, sizeof(text));
		if (!rc)
			printf("%s = %s\n", name, text);
		return rc;
	default:
		return GC_UNSUPPORTED;
	}
}

static void list_features(struct gc_nodemap *map)
{
	static const char *const types[] = {
		[GC_FEATURE_INTEGER] = "integer",
		[GC_FEATURE_FLOAT] = "float",
		[GC_FEATURE_BOOLEAN] = "boolean",
		[GC_FEATURE_ENUMERATION] = "enumeration",
		[GC_FEATURE_STRING] = "string",
		[GC_FEATURE_COMMAND] = "command",
	};
	enum gc_feature_type type;
	const char *name;
	size_t i;

	for (i = 0; i < gc_nodemap_count(map); i++) {
		name = gc_nodemap_name(map, i);
		if (gc_feature_get_type(map, name, &type) == GC_OK)
			printf("%-26s %s\n", name, types[type]);
	}
}

static int inspect(const struct gc_gige_options *options, const char *name)
{
	struct gc_device *device = NULL;
	struct gc_nodemap *map = NULL;
	int rc;

	rc = gc_gige_open(options, &device);
	if (!rc)
		rc = gc_nodemap_open(device, &map);
	if (!rc) {
		if (name)
			rc = print_feature(map, name);
		else
			list_features(map);
	}
	gc_nodemap_close(map);
	gc_device_close(device);
	return rc;
}

int main(int argc, char **argv)
{
	struct gc_gige_device_info devices[MAX_DEVICES];
	struct gc_gige_options options = { 0 };
	unsigned long frames = 5;
	unsigned long port;
	const char *feature;
	size_t count;
	int required;
	int rc;

	if (argc > 1 && !strcmp(argv[1], "--discover")) {
		if (argc > 3)
			goto usage;
		if (argc == 3)
			options.interface = argv[2];
		rc = discover(&options, devices, MAX_DEVICES, &count);
	} else if (argc > 1 && (!strcmp(argv[1], "--get") ||
			       !strcmp(argv[1], "--list"))) {
		required = !strcmp(argv[1], "--get") ? 4 : 3;
		if (argc < required || argc > required + 1)
			goto usage;
		options.address = argv[2];
		options.monitor = 1;
		feature = required == 4 ? argv[3] : NULL;
		if (argc > required) {
			port = parse_number(argv[required], UINT16_MAX);
			if (!port)
				goto usage;
			options.port = (uint16_t)port;
		}
		rc = inspect(&options, feature);
	} else {
		if (argc > 5)
			goto usage;
		options.address = argc > 1 ? argv[1] : "127.0.0.1";
		if (argc > 2) {
			port = parse_number(argv[2], UINT16_MAX);
			if (!port)
				goto usage;
			options.port = (uint16_t)port;
		}
		if (argc > 3) {
			frames = parse_number(argv[3], 1000000);
			if (!frames)
				goto usage;
		}
		rc = capture(&options, (unsigned int)frames, argc == 5 ? argv[4] : NULL);
	}
	if (rc)
		fprintf(stderr, "Controller: %s\n", gc_status_string(rc));
	return rc ? EXIT_FAILURE : EXIT_SUCCESS;

usage:
	fprintf(stderr, "Usage: %s [address [port [frame-count [image.pgm]]]]\n",
		argv[0]);
	fprintf(stderr, "       %s --discover [interface]\n", argv[0]);
	fprintf(stderr, "       %s --list address [port]\n", argv[0]);
	fprintf(stderr, "       %s --get address feature [port]\n", argv[0]);
	return EXIT_FAILURE;
}
