#include <genicam/gige/camera.h>
#include <genicam/pfnc.h>

#include "osal/osal.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WIDTH	640
#define MAX_HEIGHT	480
#define FRAME_TIME_US	50000
#define REG_BASE	0x10000U
#define REG_WIDTH	0
#define REG_HEIGHT	4
#define REG_EXPOSURE	8
#define REG_GAIN	12
#define REG_PIXEL	16
#define REG_ACQUISITION	20
#define REG_PAYLOAD	24
#define REG_LABEL	28
#define REG_OFFSET_X	60
#define REG_OFFSET_Y	64
#define REG_ACQ_MODE	68

#define MAX_XML_SIZE	(8U * 1024U * 1024U)
#define MAX_INTERFACES	32
#define XML_PATH_SIZE	4096

struct camera {
	uint8_t registers[REG_ACQ_MODE + 4];
	uint32_t frame_id;
	int frame_pending;
};

static volatile sig_atomic_t stopping;

static void stop(int signum)
{
	(void)signum;
	stopping = 1;
}

static uint32_t load32(const uint8_t *data)
{
	return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
	       (uint32_t)data[2] << 8 | data[3];
}

static void store32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)(value >> 16);
	data[2] = (uint8_t)(value >> 8);
	data[3] = (uint8_t)value;
}

static void update_payload(struct camera *cam)
{
	store32(cam->registers + REG_PAYLOAD,
		load32(cam->registers + REG_WIDTH) *
		load32(cam->registers + REG_HEIGHT));
}

static int camera_read(void *context, uint64_t address, void *data, size_t size)
{
	struct camera *cam = context;
	uint64_t offset = address - REG_BASE;

	if (offset >= sizeof(cam->registers) || size > sizeof(cam->registers) - offset)
		return GC_NOT_FOUND;
	memcpy(data, cam->registers + offset, size);
	return GC_OK;
}

static int camera_write(void *context, uint64_t address, const void *data, size_t size)
{
	struct camera *cam = context;
	uint64_t offset = address - REG_BASE;
	uint32_t value;
	float number;

	if (offset >= sizeof(cam->registers) || size > sizeof(cam->registers) - offset)
		return GC_NOT_FOUND;
	if (offset >= REG_LABEL && offset < REG_OFFSET_X) {
		if (size > REG_OFFSET_X - offset)
			return GC_INVALID;
		memcpy(cam->registers + offset, data, size);
		return GC_OK;
	}
	if (offset % 4 || size != 4)
		return GC_INVALID;
	value = load32(data);
	switch (offset) {
	case REG_WIDTH:
	case REG_HEIGHT:
		if (value < 16 || value > (offset == REG_WIDTH ? MAX_WIDTH : MAX_HEIGHT))
			return GC_INVALID;
		if (load32(cam->registers + REG_ACQUISITION))
			return GC_ACCESS;
		break;
	case REG_EXPOSURE:
	case REG_GAIN:
		memcpy(&number, &value, sizeof(value));
		if (offset == REG_EXPOSURE ? !(number >= 100 && number <= 30000) :
		    !(number >= 0 && number <= 24))
			return GC_INVALID;
		break;
	case REG_PIXEL:
		if (value != GC_PFNC_MONO8)
			return GC_INVALID;
		break;
	case REG_ACQUISITION:
		if (value > 1)
			return GC_INVALID;
		break;
	case REG_OFFSET_X:
	case REG_OFFSET_Y:
	case REG_ACQ_MODE:
		if (value)
			return GC_INVALID;
		break;
	default:
		return GC_ACCESS;
	}
	memcpy(cam->registers + offset, data, size);
	update_payload(cam);
	return GC_OK;
}

static void frame_done(void *context, const struct gc_frame *frame, int status)
{
	struct camera *cam = context;

	(void)frame;
	(void)status;
	cam->frame_pending = 0;
}

static int submit_frame(struct gc_camera *camera, struct camera *cam,
			 uint8_t *pixels, uint64_t timestamp)
{
	struct gc_frame frame = {
		.data = pixels,
		.capacity = MAX_WIDTH * MAX_HEIGHT,
		.width = load32(cam->registers + REG_WIDTH),
		.height = load32(cam->registers + REG_HEIGHT),
		.pixel_format = GC_PFNC_MONO8,
		.payload_type = GC_PAYLOAD_IMAGE,
		.timestamp = timestamp,
	};
	uint32_t x;
	uint32_t y;

	frame.size = (size_t)frame.width * frame.height;
	for (y = 0; y < frame.height; y++) {
		for (x = 0; x < frame.width; x++)
			pixels[(size_t)y * frame.width + x] = (uint8_t)(x + y + cam->frame_id);
	}
	cam->frame_id++;
	cam->frame_pending = 1;
	return gc_camera_submit_frame(camera, &frame, frame_done, cam);
}

static int parse_port(const char *arg, uint16_t *port)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(arg, &end, 10);
	if (errno || end == arg || *end || !value || value > UINT16_MAX)
		return GC_INVALID;
	*port = (uint16_t)value;
	return GC_OK;
}

static int parse_options(int argc, char **argv,
			 struct gc_gige_camera_options *options,
			 const char **xml_file)
{
	unsigned int positional = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--xml")) {
			if (*xml_file || i + 1 == argc)
				return GC_INVALID;
			*xml_file = argv[++i];
			continue;
		}
		if (argv[i][0] == '-' || positional == 2)
			return GC_INVALID;
		if (positional++ == 0)
			options->address = argv[i];
		else if (parse_port(argv[i], &options->port))
			return GC_INVALID;
	}
	return GC_OK;
}

static int sibling_xml(const char *program, char path[XML_PATH_SIZE])
{
	static const char filename[] = "camera.xml";
	const char *dir = strrchr(program, '/');
	const char *backslash = strrchr(program, '\\');
	size_t length;

	if (backslash && (!dir || backslash > dir))
		dir = backslash;
	length = dir ? (size_t)(dir - program) + 1 : 0;
	if (length > XML_PATH_SIZE - sizeof(filename))
		return GC_INVALID;
	memcpy(path, program, length);
	memcpy(path + length, filename, sizeof(filename));
	return GC_OK;
}

static int interface_netmask(const char *address, char netmask[GC_GIGE_IPV4_SIZE])
{
	struct osal_interface interfaces[MAX_INTERFACES];
	uint32_t ip;
	size_t count;
	size_t i;
	int rc;

	rc = osal_ipv4_parse(address, &ip);
	if (rc)
		return rc;
	if (ip >> 24 == 127) {
		osal_ipv4_format(0xff000000, netmask);
		return GC_OK;
	}
	rc = osal_ipv4_interfaces(interfaces, MAX_INTERFACES, &count);
	if (rc)
		return rc;
	for (i = 0; i < count; i++) {
		if (interfaces[i].address == ip) {
			osal_ipv4_format(interfaces[i].netmask, netmask);
			return GC_OK;
		}
	}
	return GC_NOT_FOUND;
}

static int read_xml(const char *path, void **data, size_t *size)
{
	uint8_t *bytes = NULL;
	FILE *file;
	long length;
	int rc = GC_IO;

	file = fopen(path, "rb");
	if (!file)
		return errno == ENOENT ? GC_NOT_FOUND : GC_IO;
	if (fseek(file, 0, SEEK_END))
		goto out;
	length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET))
		goto out;
	if (!length || (unsigned long)length > MAX_XML_SIZE) {
		rc = GC_INVALID;
		goto out;
	}
	bytes = malloc((size_t)length);
	if (!bytes) {
		rc = GC_NOMEM;
		goto out;
	}
	if (fread(bytes, 1, (size_t)length, file) == (size_t)length)
		rc = GC_OK;
out:
	fclose(file);
	if (rc)
		free(bytes);
	else {
		*data = bytes;
		*size = (size_t)length;
	}
	return rc;
}

int main(int argc, char **argv)
{
	struct gc_camera *camera;
	struct camera cam = { 0 };
	struct gc_gige_camera_options options = {
		.address = "127.0.0.1",
		.port = GC_GIGE_CONTROL_PORT,
		.broadcast_discovery = 1,
	};
	struct gc_camera_config config = {
		.manufacturer = "GenICam Demo",
		.model = "Virtual GigE Camera",
		.serial = "DEMO-GEV-001",
		.context = &cam,
		.read = camera_read,
		.write = camera_write,
		.timestamp_hz = 1000000,
	};
	const char *xml_file = NULL;
	char xml_path[XML_PATH_SIZE];
	char netmask[GC_GIGE_IPV4_SIZE];
	void *xml;
	uint8_t *pixels;
	uint64_t next = 0;
	uint64_t now;
	int rc;

	if (parse_options(argc, argv, &options, &xml_file)) {
		fprintf(stderr, "Usage: %s [address [port]] [--xml file]\n", argv[0]);
		return EXIT_FAILURE;
	}
	rc = interface_netmask(options.address, netmask);
	if (rc) {
		fprintf(stderr, "Local address %s: %s\n", options.address,
			gc_status_string(rc));
		return EXIT_FAILURE;
	}
	options.netmask = netmask;
	if (xml_file) {
		rc = read_xml(xml_file, &xml, &config.xml_size);
	} else {
		xml_file = GC_DEMO_XML_FILE;
		rc = read_xml(xml_file, &xml, &config.xml_size);
		if (rc == GC_NOT_FOUND && !sibling_xml(argv[0], xml_path)) {
			xml_file = xml_path;
			rc = read_xml(xml_file, &xml, &config.xml_size);
		}
	}
	if (rc) {
		fprintf(stderr, "Camera XML %s: %s\n", xml_file, gc_status_string(rc));
		return EXIT_FAILURE;
	}
	config.xml = xml;
	store32(cam.registers + REG_WIDTH, MAX_WIDTH);
	store32(cam.registers + REG_HEIGHT, MAX_HEIGHT);
	store32(cam.registers + REG_EXPOSURE, 0x447a0000);
	store32(cam.registers + REG_PIXEL, GC_PFNC_MONO8);
	update_payload(&cam);
	pixels = malloc(2U * MAX_WIDTH * MAX_HEIGHT);
	if (!pixels) {
		free(xml);
		return EXIT_FAILURE;
	}
	options.resend_buffer = pixels + MAX_WIDTH * MAX_HEIGHT;
	options.resend_capacity = MAX_WIDTH * MAX_HEIGHT;

	rc = gc_gige_camera_open(&config, &options, 1, &camera);
	if (rc) {
		fprintf(stderr, "Server open: %s\n", gc_status_string(rc));
		free(pixels);
		free(xml);
		return EXIT_FAILURE;
	}
	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	printf("GigE Vision server at %s:%u\n", options.address, (unsigned int)options.port);
	printf("Camera XML: %s\n", xml_file);
	fflush(stdout);

	while (!stopping) {
		rc = gc_camera_poll(camera, 10);
		if (rc != GC_OK && rc != GC_TIMEOUT)
			break;
		rc = GC_OK;
		now = osal_monotonic_us();
		if (load32(cam.registers + REG_ACQUISITION) && now >= next &&
		    !cam.frame_pending) {
			next = now + FRAME_TIME_US;
			rc = submit_frame(camera, &cam, pixels, now);
			if (rc)
				break;
		}
	}
	if (rc)
		fprintf(stderr, "Server I/O: %s\n", gc_status_string(rc));
	gc_camera_close(camera);
	free(pixels);
	free(xml);
	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
