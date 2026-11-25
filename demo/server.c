#include <genicam/gige.h>
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

#define INTEGER(name, reg, min, max) \
	"<Integer Name='" name "'><pValue>" reg "</pValue>" \
	"<Min>" min "</Min><Max>" max "</Max></Integer>"
#define FLOAT(name, reg, min, max) \
	"<Float Name='" name "'><pValue>" reg "</pValue>" \
	"<Min>" min "</Min><Max>" max "</Max></Float>"
#define INT_REG(name, address, access) \
	"<IntReg Name='" name "'><Address>" address "</Address><Length>4</Length>" \
	"<AccessMode>" access "</AccessMode><pPort>Device</pPort>" \
	"<Endianess>BigEndian</Endianess></IntReg>"
#define FLOAT_REG(name, address) \
	"<FloatReg Name='" name "'><Address>" address "</Address><Length>4</Length>" \
	"<AccessMode>RW</AccessMode><pPort>Device</pPort>" \
	"<Endianess>BigEndian</Endianess></FloatReg>"

static const char camera_xml[] =
	"<?xml version='1.0' encoding='UTF-8'?>"
	"<RegisterDescription ModelName='VirtualGigECamera' VendorName='GenICamDemo'"
	" StandardNameSpace='None' SchemaMajorVersion='1' SchemaMinorVersion='1'"
	" SchemaSubMinorVersion='0' MajorVersion='1' MinorVersion='0' SubMinorVersion='0'"
	" ProductGuid='36D356C9-3ADE-4D09-A4F4-C5E171387F70'"
	" VersionGuid='59D8CD5E-4E40-49D7-8E2D-EEC932787342'"
	" xmlns='http://www.genicam.org/GenApi/Version_1_1'>"
	"<Category Name='Root'><pFeature>Width</pFeature><pFeature>Height</pFeature>"
	"<pFeature>PixelFormat</pFeature><pFeature>ExposureTime</pFeature>"
	"<pFeature>Gain</pFeature><pFeature>PayloadSize</pFeature>"
	"<pFeature>AcquisitionStart</pFeature><pFeature>AcquisitionStop</pFeature>"
	"<pFeature>DeviceUserID</pFeature></Category>"
	INTEGER("Width", "WidthReg", "16", "640")
	INTEGER("Height", "HeightReg", "16", "480")
	FLOAT("ExposureTime", "ExposureReg", "100", "30000")
	FLOAT("Gain", "GainReg", "0", "24")
	"<Enumeration Name='PixelFormat'>"
	"<EnumEntry Name='Mono8'><Value>0x01080001</Value></EnumEntry>"
	"<pValue>PixelReg</pValue></Enumeration>"
	"<Command Name='AcquisitionStart'><pValue>AcquisitionReg</pValue>"
	"<CommandValue>1</CommandValue></Command>"
	"<Command Name='AcquisitionStop'><pValue>AcquisitionReg</pValue>"
	"<CommandValue>0</CommandValue></Command>"
	"<Integer Name='PayloadSize'><pValue>PayloadReg</pValue></Integer>"
	"<StringReg Name='DeviceUserID'><Address>0x1001c</Address><Length>32</Length>"
	"<AccessMode>RW</AccessMode><pPort>Device</pPort></StringReg>"
	INT_REG("WidthReg", "0x10000", "RW")
	INT_REG("HeightReg", "0x10004", "RW")
	FLOAT_REG("ExposureReg", "0x10008")
	FLOAT_REG("GainReg", "0x1000c")
	INT_REG("PixelReg", "0x10010", "RW")
	INT_REG("AcquisitionReg", "0x10014", "WO")
	INT_REG("PayloadReg", "0x10018", "RO")
	"<Port Name='Device'/>"
	"</RegisterDescription>";

struct camera {
	uint8_t registers[REG_LABEL + 32];
	uint32_t frame_id;
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
	if (offset >= REG_LABEL) {
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
	default:
		return GC_ACCESS;
	}
	memcpy(cam->registers + offset, data, size);
	update_payload(cam);
	return GC_OK;
}

static int send_frame(struct gc_gige_server *server, struct camera *cam,
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
	return gc_gige_server_send(server, &frame);
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

int main(int argc, char **argv)
{
	struct gc_gige_server *server;
	struct camera cam = { 0 };
	struct gc_gige_server_config config = {
		.address = "127.0.0.1",
		.netmask = "255.0.0.0",
		.port = GC_GIGE_CONTROL_PORT,
		.manufacturer = "GenICam Demo",
		.model = "Virtual GigE Camera",
		.serial = "DEMO-GEV-001",
		.xml = camera_xml,
		.xml_size = sizeof(camera_xml) - 1,
		.context = &cam,
		.read = camera_read,
		.write = camera_write,
		.timestamp_hz = 1000000,
	};
	uint8_t *pixels;
	uint64_t next = 0;
	uint64_t now;
	int rc;

	if (argc > 3 || (argc == 3 && parse_port(argv[2], &config.port))) {
		fprintf(stderr, "Usage: %s [address [port]]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		config.address = argv[1];
	if (strncmp(config.address, "127.", 4)) {
		config.netmask = "255.255.255.0";
		config.broadcast_discovery = 1;
	}
	store32(cam.registers + REG_WIDTH, MAX_WIDTH);
	store32(cam.registers + REG_HEIGHT, MAX_HEIGHT);
	store32(cam.registers + REG_EXPOSURE, 0x447a0000);
	store32(cam.registers + REG_PIXEL, GC_PFNC_MONO8);
	update_payload(&cam);
	pixels = malloc(2U * MAX_WIDTH * MAX_HEIGHT);
	if (!pixels)
		return EXIT_FAILURE;
	config.resend_buffer = pixels + MAX_WIDTH * MAX_HEIGHT;
	config.resend_capacity = MAX_WIDTH * MAX_HEIGHT;

	rc = gc_gige_server_open(&config, &server);
	if (rc) {
		fprintf(stderr, "Server open: %s\n", gc_status_string(rc));
		free(pixels);
		return EXIT_FAILURE;
	}
	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	printf("GigE Vision server at %s:%u\n", config.address, (unsigned int)config.port);
	fflush(stdout);

	while (!stopping) {
		rc = gc_gige_server_poll(server, 10);
		if (rc != GC_OK && rc != GC_TIMEOUT)
			break;
		rc = GC_OK;
		now = osal_monotonic_us();
		if (load32(cam.registers + REG_ACQUISITION) && now >= next) {
			next = now + FRAME_TIME_US;
			rc = send_frame(server, &cam, pixels, now);
			if (rc != GC_OK && rc != GC_STOPPED)
				break;
			rc = GC_OK;
		}
	}
	if (rc)
		fprintf(stderr, "Server I/O: %s\n", gc_status_string(rc));
	gc_gige_server_close(server);
	free(pixels);
	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
