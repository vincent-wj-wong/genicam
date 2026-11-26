#include <genicam/gige.h>
#include <genicam/pfnc.h>

#include "osal/osal.h"
#include "transport/gige/wire/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define ADDRESS		0x7f000001U
#define SENSOR_BASE	0x10000U
#define IMAGE_SIZE	2048
#define XML_ZIP_SIZE	4096

#define CHECK(condition) do {						\
	if (!(condition)) {						\
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,	\
			#condition);					\
		exit(EXIT_FAILURE);					\
	}								\
} while (0)

static const char xml[] =
	"<RegisterDescription>"
	"<Integer Name='Width'><pValue>WidthReg</pValue>"
	"<Min>16</Min><Max>128</Max><Inc>8</Inc></Integer>"
	"<IntReg Name='WidthReg'><Address>0x10000</Address><Length>4</Length>"
	"<AccessMode>RW</AccessMode><Endianess>BigEndian</Endianess></IntReg>"
	"<Float Name='ExposureTime'><pValue>ExposureConvert</pValue>"
	"<Min>1</Min><Max>5000</Max></Float>"
	"<Converter Name='ExposureConvert'>"
	"<FormulaTo>FROM * 2</FormulaTo><FormulaFrom>TO / 2</FormulaFrom>"
	"<pValue>ExposureReg</pValue></Converter>"
	"<FloatReg Name='ExposureReg'><Address>0x10004</Address><Length>4</Length>"
	"<AccessMode>RW</AccessMode><Endianess>BigEndian</Endianess></FloatReg>"
	"<Enumeration Name='PixelFormat'>"
	"<EnumEntry Name='Mono8'><Value>0x01080001</Value></EnumEntry>"
	"<pValue>PixelReg</pValue></Enumeration>"
	"<IntReg Name='PixelReg'><Address>0x10008</Address><Length>4</Length></IntReg>"
	"<IntSwissKnife Name='PayloadSize'>"
	"<pVariable Name='WIDTH'>Width</pVariable>"
	"<pVariable Name='PIXELFORMAT'>PixelFormat</pVariable>"
	"<Constant Name='HEIGHT'>32</Constant>"
	"<Formula>WIDTH * HEIGHT * ((PIXELFORMAT &gt;&gt; 16) &amp; 0xFF) / 8</Formula>"
	"</IntSwissKnife>"
	"<Command Name='Acquire'><pValue>CaptureReg</pValue>"
	"<CommandValue>1</CommandValue></Command>"
	"<IntReg Name='CaptureReg'><Address>0x1000c</Address><Length>4</Length></IntReg>"
	"<StringReg Name='Label'><Address>0x10010</Address><Length>16</Length></StringReg>"
	"<String Name='LabelAlias'><pValue>Label</pValue></String>"
	"<String Name='LabelLiteral'><Value> fixed </Value></String>"
	"<Integer Name='Cycle'><pValue>Cycle</pValue></Integer>"
	"<Boolean Name='Enabled'><pValue>StateReg</pValue>"
	"<OnValue>2</OnValue><OffValue>3</OffValue></Boolean>"
	"<IntReg Name='StateReg'><Address>0x10020</Address><Length>4</Length></IntReg>"
	"<Integer Name='Locked'><pValue>WidthReg</pValue>"
	"<pIsLocked>Enabled</pIsLocked></Integer>"
	"</RegisterDescription>";

struct sensor {
	struct gc_camera *camera;
	struct osal_mutex *lock;
	uint8_t registers[36];
	uint8_t image[IMAGE_SIZE];
	uint8_t cache[IMAGE_SIZE];
	int stopped;
	int completed;
};

struct pending_receive {
	struct gc_stream *stream;
	int result;
};

static void put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
	put_le16(p, (uint16_t)value);
	put_le16(p + 2, (uint16_t)(value >> 16));
}

static size_t make_zip(uint8_t *zip)
{
	static const char name[] = "fixture.xml";
	size_t name_size = sizeof(name) - 1;
	size_t data = 30 + name_size;
	size_t directory;
	size_t end;
	size_t length;
	uint32_t crc = (uint32_t)crc32(0, (const Bytef *)xml, sizeof(xml) - 1);
	z_stream stream = { 0 };

	stream.next_in = (Bytef *)xml;
	stream.avail_in = sizeof(xml) - 1;
	stream.next_out = zip + data;
	stream.avail_out = XML_ZIP_SIZE - data - 100;
	CHECK(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
			   -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
	CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	length = stream.total_out;
	CHECK(deflateEnd(&stream) == Z_OK);

	put_le32(zip, 0x04034b50);
	put_le16(zip + 4, 20);
	put_le16(zip + 8, 8);
	put_le32(zip + 14, crc);
	put_le32(zip + 18, (uint32_t)length);
	put_le32(zip + 22, sizeof(xml) - 1);
	put_le16(zip + 26, (uint16_t)name_size);
	memcpy(zip + 30, name, name_size);

	directory = data + length;
	put_le32(zip + directory, 0x02014b50);
	put_le16(zip + directory + 4, 20);
	put_le16(zip + directory + 6, 20);
	put_le16(zip + directory + 10, 8);
	put_le32(zip + directory + 16, crc);
	put_le32(zip + directory + 20, (uint32_t)length);
	put_le32(zip + directory + 24, sizeof(xml) - 1);
	put_le16(zip + directory + 28, (uint16_t)name_size);
	memcpy(zip + directory + 46, name, name_size);

	end = directory + 46 + name_size;
	put_le32(zip + end, 0x06054b50);
	put_le16(zip + end + 8, 1);
	put_le16(zip + end + 10, 1);
	put_le32(zip + end + 12, (uint32_t)(end - directory));
	put_le32(zip + end + 16, (uint32_t)directory);
	return end + 22;
}

static int sensor_read(void *context, uint64_t address, void *data, size_t size)
{
	struct sensor *sensor = context;
	uint64_t offset = address - SENSOR_BASE;

	if (offset >= sizeof(sensor->registers) ||
	    size > sizeof(sensor->registers) - offset)
		return GC_NOT_FOUND;
	memcpy(data, sensor->registers + offset, size);
	return GC_OK;
}

static int sensor_write(void *context, uint64_t address, const void *data,
			size_t size)
{
	struct sensor *sensor = context;
	uint64_t offset = address - SENSOR_BASE;

	if (offset >= sizeof(sensor->registers) ||
	    size > sizeof(sensor->registers) - offset)
		return GC_NOT_FOUND;
	memcpy(sensor->registers + offset, data, size);
	return GC_OK;
}

static void sensor_done(void *context, const struct gc_frame *frame, int status)
{
	struct sensor *sensor = context;

	CHECK(frame->data == sensor->image);
	CHECK(status == GC_OK);
	sensor->completed++;
}

static void run_sensor(void *context)
{
	struct sensor *sensor = context;
	struct gc_frame frame = {
		.data = sensor->image,
		.size = IMAGE_SIZE,
		.capacity = IMAGE_SIZE,
		.width = 64,
		.height = 32,
		.pixel_format = GC_PFNC_MONO8,
		.payload_type = GC_PAYLOAD_IMAGE,
		.timestamp = 123456,
	};
	int stopped;
	int rc;

	for (;;) {
		osal_mutex_lock(sensor->lock);
		stopped = sensor->stopped;
		osal_mutex_unlock(sensor->lock);
		if (stopped)
			return;
		rc = gc_camera_poll(sensor->camera, 10);
		CHECK(rc == GC_OK || rc == GC_TIMEOUT);
		if (gc_be32(sensor->registers + 12)) {
			gc_put_be32(sensor->registers + 12, 0);
			CHECK(gc_camera_submit_frame(sensor->camera, &frame,
						     sensor_done, sensor) == GC_OK);
		}
	}
}

static uint16_t temporary_port(void)
{
	struct osal_endpoint local = { .address = ADDRESS };
	osal_socket socket;

	CHECK(osal_udp_open(local, 0, 0, &socket) == GC_OK);
	CHECK(osal_udp_local(socket, &local) == GC_OK);
	osal_udp_close(socket);
	return local.port;
}

static void slow_control(void *context)
{
	osal_socket socket = *(osal_socket *)context;
	struct osal_endpoint source;
	struct gvcp_packet packet;
	uint8_t rx[32];
	uint8_t tx[12];
	size_t length;
	uint16_t commands[] = { GEV_VERSION, GEV_CAPABILITY, GEV_TICK_HI, GEV_TICK_LO };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		CHECK(osal_udp_receive(socket, &source, rx, sizeof(rx), &length, 1000) == GC_OK);
		CHECK(gvcp_decode(rx, length, &packet) == GC_OK);
		CHECK(packet.command == GVCP_READ_REG && packet.size == 4);
		CHECK(gc_be32(packet.body) == commands[i]);
		if (i == 0) {
			gvcp_header(tx, 0, GVCP_PENDING_ACK, 4, packet.id);
			gc_put_be32(tx + 8, 200);
			CHECK(osal_udp_send(socket, source, tx, sizeof(tx)) == GC_OK);
			osal_msleep(80);
		}
		gvcp_header(tx, 0, GVCP_READ_REG + 1, 4, packet.id);
		gc_put_be32(tx + 8, i == 0 ? 0x00010002 : 0);
		CHECK(osal_udp_send(socket, source, tx, sizeof(tx)) == GC_OK);
	}
}

static void test_pending_ack(void)
{
	struct gc_gige_options options = {
		.address = "127.0.0.1",
		.timeout_ms = 50,
		.monitor = 1,
	};
	struct osal_endpoint local = { .address = ADDRESS };
	struct osal_thread *thread;
	struct gc_device *device;
	osal_socket socket;

	CHECK(osal_udp_open(local, 0, 0, &socket) == GC_OK);
	CHECK(osal_udp_local(socket, &local) == GC_OK);
	options.port = local.port;
	thread = osal_thread_create(slow_control, &socket);
	CHECK(thread);
	CHECK(gc_gige_open(&options, &device) == GC_OK);
	gc_device_close(device);
	osal_thread_join(thread);
	osal_udp_close(socket);
}

static void send_gvsp(osal_socket socket, struct osal_endpoint peer,
		      uint64_t block, uint32_t id, unsigned int kind,
		      int extended, const void *data, size_t size)
{
	uint8_t bytes[1500] = { 0 };
	size_t header = extended ? 20 : 8;

	if (extended) {
		bytes[4] = (uint8_t)(0x80 | kind);
		gc_put_be64(bytes + 8, block);
		gc_put_be32(bytes + 16, id);
	} else {
		gc_put_be16(bytes + 2, (uint16_t)block);
		gc_put_be32(bytes + 4, (uint32_t)kind << 24 | id);
	}
	CHECK(size + header <= sizeof(bytes));
	memcpy(bytes + header, data, size);
	CHECK(osal_udp_send(socket, peer, bytes, header + size) == GC_OK);
}

static void read_frame(void *context)
{
	struct pending_receive *pending = context;
	uint8_t data[IMAGE_SIZE];
	struct gc_frame frame = { .data = data, .capacity = sizeof(data) };

	pending->result = gc_stream_receive(pending->stream, &frame, 5000);
}

static void test_stream(struct gc_device *device, struct gc_nodemap *map,
			const struct sensor *sensor)
{
	struct osal_endpoint endpoint = { .address = ADDRESS };
	struct osal_endpoint peer = { .address = ADDRESS };
	struct gc_stream *stream;
	struct gc_stream *extra;
	struct gc_stream_queue *queue;
	struct gc_stream_stats stats;
	struct pending_receive pending;
	struct osal_thread *thread;
	uint8_t bytes[IMAGE_SIZE];
	uint8_t spare[32];
	struct gc_frame frame = { .data = bytes, .capacity = sizeof(bytes) };
	uint8_t leader[36] = { 0 };
	uint8_t trailer[8] = { 0 };
	uint8_t all_in[36 + 8 + sizeof(spare)];
	uint32_t port;
	uint64_t deadline;
	uint64_t extended = UINT64_C(0x1234567800000001);
	size_t classic_chunk = 1464;
	size_t extended_chunk = 1452;
	osal_socket socket;

	CHECK(gc_stream_open(device, &stream) == GC_OK);
	CHECK(gc_stream_open(device, &extra) == GC_BUSY);
	CHECK(gc_stream_receive(stream, &frame, 0) == GC_STOPPED);
	CHECK(gc_stream_queue_open(stream, 1, &queue) == GC_OK);
	CHECK(gc_stream_queue_push(queue, bytes, sizeof(bytes)) == GC_OK);
	CHECK(gc_stream_queue_push(queue, bytes, sizeof(bytes)) == GC_BUSY);
	CHECK(gc_stream_queue_push(queue, spare, sizeof(spare)) == GC_BUFFER_SMALL);
	CHECK(gc_stream_start(stream) == GC_OK);
	CHECK(gc_feature_execute(map, "Acquire") == GC_OK);
	CHECK(gc_stream_queue_receive(queue, &frame, 1000) == GC_OK);
	CHECK(frame.data == bytes);
	CHECK(frame.block_id == 1 && frame.timestamp == 123456);
	CHECK(frame.timestamp_hz == 1000000 && frame.stride == 64);
	CHECK(frame.size == sizeof(sensor->image));
	CHECK(!memcmp(bytes, sensor->image, sizeof(bytes)));
	CHECK(gc_device_read_register(device, GEV_STREAM_PORT, &port) == GC_OK);
	CHECK(port && port <= UINT16_MAX);
	peer.port = (uint16_t)port;
	CHECK(osal_udp_open(endpoint, 0, 0, &socket) == GC_OK);

	gc_put_be16(leader + 2, GC_PAYLOAD_IMAGE);
	gc_put_be32(leader + 12, GC_PFNC_MONO8);
	gc_put_be32(leader + 16, 64);
	gc_put_be32(leader + 20, 32);
	gc_put_be32(trailer, GC_PAYLOAD_IMAGE);
	gc_put_be32(trailer + 4, 32);

	/* Omit one packet from the cached block. */
	memset(bytes, 0, sizeof(bytes));
	send_gvsp(socket, peer, 1, 0, GVSP_LEADER, 0, leader, 32);
	send_gvsp(socket, peer, 1, 2, GVSP_DATA, 0, sensor->image + classic_chunk,
		  IMAGE_SIZE - classic_chunk);
	send_gvsp(socket, peer, 1, 3, GVSP_TRAILER, 0, trailer, sizeof(trailer));
	CHECK(gc_stream_receive(stream, &frame, 1000) == GC_OK);
	CHECK(!memcmp(bytes, sensor->image, sizeof(bytes)));

	/* A different block includes a malformed packet and reordered data. */
	gc_put_be32(trailer + 4, 0);
	send_gvsp(socket, peer, extended, 0, GVSP_LEADER, 1, leader, sizeof(leader));
	send_gvsp(socket, peer, extended, 3, GVSP_TRAILER, 1, trailer, sizeof(trailer));
	send_gvsp(socket, peer, extended, 1, GVSP_DATA, 1, sensor->image, 5);
	send_gvsp(socket, peer, extended, 2, GVSP_DATA, 1,
		  sensor->image + extended_chunk, IMAGE_SIZE - extended_chunk);
	send_gvsp(socket, peer, extended, 2, GVSP_DATA, 1,
		  sensor->image + extended_chunk, IMAGE_SIZE - extended_chunk);
	send_gvsp(socket, peer, extended, 1, GVSP_DATA, 1,
		  sensor->image, extended_chunk);
	CHECK(gc_stream_receive(stream, &frame, 1000) == GC_OK);
	CHECK(frame.block_id == extended && frame.size == sizeof(bytes));
	CHECK(!memcmp(bytes, sensor->image, sizeof(bytes)));

	send_gvsp(socket, peer, 2, 0, GVSP_LEADER, 0, leader, sizeof(leader));
	send_gvsp(socket, peer, 2, 3, GVSP_TRAILER, 0, trailer, sizeof(trailer));
	CHECK(gc_stream_receive(stream, &frame, 80) == GC_TIMEOUT);
	CHECK(gc_stream_get_stats(stream, &stats) == GC_OK);
	CHECK(stats.completed == 3 && stats.incomplete == 1 &&
	      stats.duplicates == 1 && stats.invalid_packets >= 1 &&
	      stats.resend_requests >= 1);

	gc_put_be32(leader + 16, 8);
	gc_put_be32(leader + 20, 4);
	gc_put_be32(trailer + 4, 4);
	memcpy(all_in, leader, sizeof(leader));
	memcpy(all_in + sizeof(leader), trailer, sizeof(trailer));
	memcpy(all_in + sizeof(leader) + sizeof(trailer), sensor->image, sizeof(spare));
	send_gvsp(socket, peer, 3, 0, 4, 0, all_in, sizeof(all_in));
	CHECK(gc_stream_queue_push(queue, spare, sizeof(spare)) == GC_OK);
	CHECK(gc_stream_queue_receive(queue, &frame, 1000) == GC_OK);
	CHECK(frame.block_id == 3 && frame.data == spare && frame.size == sizeof(spare));
	CHECK(frame.width == 8 && frame.height == 4);
	CHECK(!memcmp(spare, sensor->image, sizeof(spare)));

	frame.data = bytes;
	gc_put_be32(leader + 16, 64);
	gc_put_be32(leader + 20, 32);
	send_gvsp(socket, peer, 3, 0, GVSP_LEADER, 0, leader, sizeof(leader));
	frame.capacity = 1;
	CHECK(gc_stream_receive(stream, &frame, 100) == GC_BUFFER_SMALL);
	frame.capacity = sizeof(bytes);
	pending.stream = stream;
	thread = osal_thread_create(read_frame, &pending);
	CHECK(thread);
	osal_msleep(50);
	deadline = osal_deadline(1000);
	CHECK(gc_stream_stop(stream) == GC_OK);
	osal_thread_join(thread);
	CHECK(pending.result == GC_STOPPED && osal_remaining_ms(deadline));
	CHECK(gc_stream_start(stream) == GC_OK);
	CHECK(gc_feature_execute(map, "Acquire") == GC_OK);
	CHECK(gc_stream_receive(stream, &frame, 1000) == GC_OK);
	CHECK(gc_stream_queue_push(queue, bytes, sizeof(bytes)) == GC_OK);
	CHECK(gc_stream_queue_receive(queue, &frame, 20) == GC_TIMEOUT);
	CHECK(frame.data == bytes);
	CHECK(gc_stream_queue_receive(queue, &frame, 0) == GC_NOT_FOUND);
	CHECK(!frame.data);
	CHECK(gc_stream_queue_push(queue, bytes, sizeof(bytes)) == GC_OK);
	CHECK(gc_stream_queue_pop(queue, &frame) == GC_OK && frame.data == bytes);
	gc_stream_queue_close(queue);
	gc_stream_close(stream);
	osal_udp_close(socket);
}

int main(void)
{
	struct sensor sensor = { 0 };
	struct gc_camera_config config = { 0 };
	struct gc_gige_camera_options camera_options = { 0 };
	struct gc_gige_options options = { .address = "127.0.0.1", .timeout_ms = 100 };
	struct gc_gige_device_info info;
	struct gc_device *device;
	struct gc_device *monitor;
	struct gc_device *contender;
	struct gc_nodemap *map;
	enum gc_feature_type feature_type;
	struct osal_thread *thread;
	uint8_t zip[XML_ZIP_SIZE] = { 0 };
	void *raw_xml;
	size_t raw_size;
	char text[32];
	uint32_t reg;
	double exposure;
	int64_t value;
	size_t count;
	size_t i;

	sensor.lock = osal_mutex_create();
	CHECK(sensor.lock);
	gc_put_be32(sensor.registers, 64);
	gc_put_be32(sensor.registers + 8, GC_PFNC_MONO8);
	gc_put_be32(sensor.registers + 32, 3);
	for (i = 0; i < sizeof(sensor.image); i++)
		sensor.image[i] = (uint8_t)(i % 251);

	camera_options.port = temporary_port();
	camera_options.address = options.address;
	camera_options.netmask = "255.0.0.0";
	camera_options.broadcast_discovery = 1;
	config.manufacturer = "Test Labs";
	config.serial = "TEST-001";
	config.xml = zip;
	config.xml_size = make_zip(zip);
	config.context = &sensor;
	config.read = sensor_read;
	config.write = sensor_write;
	config.timestamp_hz = 1000000;
	camera_options.resend_buffer = sensor.cache;
	camera_options.resend_capacity = sizeof(sensor.cache);
	CHECK(gc_gige_camera_open(&config, &camera_options, 2, &sensor.camera) == GC_OK);
	thread = osal_thread_create(run_sensor, &sensor);
	CHECK(thread);

	options.port = camera_options.port;
#ifdef __linux__
	options.interface = camera_options.address;
	options.address = NULL;
	CHECK(gc_gige_discover(&options, &info, 1, &count) == GC_OK);
	CHECK(count == 1 && !strcmp(info.address, camera_options.address));
	options.address = camera_options.address;
	options.interface = NULL;
#endif
	CHECK(gc_gige_discover(&options, &info, 1, &count) == GC_OK);
	CHECK(count == 1 && !strcmp(info.serial, config.serial));
	CHECK(!strcmp(info.address, options.address));
	CHECK(gc_gige_open(&options, &device) == GC_OK);
	CHECK(gc_gige_open(&options, &contender) == GC_ACCESS && !contender);
	options.monitor = 1;
	CHECK(gc_gige_open(&options, &monitor) == GC_OK);
	CHECK(gc_device_read_register(monitor, SENSOR_BASE, &reg) == GC_OK && reg == 64);
	CHECK(gc_device_write_register(monitor, SENSOR_BASE, 72) == GC_ACCESS);
	gc_device_close(monitor);
	options.monitor = 0;

	CHECK(gc_device_get_xml(device, &raw_xml, &raw_size) == GC_OK);
	CHECK(raw_size == config.xml_size && !memcmp(raw_xml, config.xml, raw_size));
	CHECK(gc_nodemap_open_xml(device, raw_xml, raw_size, &map) == GC_OK);
	gc_device_free_xml(raw_xml);
	CHECK(gc_feature_get_type(map, "Width", &feature_type) == GC_OK &&
	      feature_type == GC_FEATURE_INTEGER);
	CHECK(gc_feature_get_type(map, "ExposureTime", &feature_type) == GC_OK &&
	      feature_type == GC_FEATURE_FLOAT);
	CHECK(gc_feature_get_type(map, "PixelFormat", &feature_type) == GC_OK &&
	      feature_type == GC_FEATURE_ENUMERATION);
	CHECK(gc_feature_get_type(map, "Label", &feature_type) == GC_OK &&
	      feature_type == GC_FEATURE_STRING);
	CHECK(gc_feature_get_type(map, "Acquire", &feature_type) == GC_OK &&
	      feature_type == GC_FEATURE_COMMAND);
	CHECK(gc_feature_get_type(map, "Missing", &feature_type) == GC_NOT_FOUND);
	CHECK(gc_feature_get_integer(map, "Width", &value) == GC_OK && value == 64);
	CHECK(gc_feature_set_integer(map, "Width", 65) == GC_INVALID);
	CHECK(gc_feature_set_integer(map, "Width", 72) == GC_OK);
	CHECK(gc_feature_get_integer(map, "Width", &value) == GC_OK && value == 72);
	CHECK(gc_feature_get_integer(map, "PayloadSize", &value) == GC_OK && value == 2304);
	CHECK(gc_feature_set_integer(map, "Enabled", 1) == GC_OK);
	CHECK(gc_feature_get_integer(map, "Enabled", &value) == GC_OK && value == 1);
	CHECK(gc_feature_set_integer(map, "Locked", 64) == GC_ACCESS);
	CHECK(gc_feature_set_integer(map, "Enabled", 0) == GC_OK);
	CHECK(gc_feature_set_integer(map, "Locked", 64) == GC_OK);
	CHECK(gc_feature_set_float(map, "ExposureTime", 1250.5) == GC_OK);
	CHECK(gc_feature_get_float(map, "ExposureTime", &exposure) == GC_OK);
	CHECK(exposure == 1250.5);
	CHECK(gc_feature_get_string(map, "PixelFormat", text, sizeof(text)) == GC_OK);
	CHECK(!strcmp(text, "Mono8"));
	CHECK(gc_feature_set_string(map, "PixelFormat", "Mono8") == GC_OK);
	CHECK(gc_feature_set_string(map, "Label", "test") == GC_OK);
	CHECK(gc_feature_get_string(map, "Label", text, 3) == GC_BUFFER_SMALL);
	CHECK(gc_feature_get_string(map, "Label", text, sizeof(text)) == GC_OK);
	CHECK(!strcmp(text, "test"));
	CHECK(gc_feature_set_string(map, "LabelAlias", "alias") == GC_OK);
	CHECK(gc_feature_get_string(map, "LabelAlias", text, sizeof(text)) == GC_OK);
	CHECK(!strcmp(text, "alias"));
	CHECK(gc_feature_get_string(map, "LabelLiteral", text, sizeof(text)) == GC_OK);
	CHECK(!strcmp(text, " fixed "));
	CHECK(gc_feature_set_string(map, "LabelLiteral", "new") == GC_ACCESS);
	CHECK(gc_feature_get_integer(map, "Cycle", &value) == GC_PROTOCOL);
	CHECK(gc_pfnc_row_size(GC_PFNC_MONO10P, 5, &i) == GC_OK && i == 7);

	test_stream(device, map, &sensor);
	gc_nodemap_close(map);
	gc_device_close(device);
	CHECK(gc_gige_open(&options, &device) == GC_OK);
	gc_device_close(device);
	osal_mutex_lock(sensor.lock);
	sensor.stopped = 1;
	osal_mutex_unlock(sensor.lock);
	osal_thread_join(thread);
	CHECK(sensor.completed == 2);
	gc_camera_close(sensor.camera);
	osal_mutex_destroy(sensor.lock);
	test_pending_ack();
	return EXIT_SUCCESS;
}
