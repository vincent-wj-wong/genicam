#include <genicam/gendc.h>
#include <genicam/pfnc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do {						\
	if (!(expr)) {							\
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);	\
		exit(EXIT_FAILURE);					\
	}								\
} while (0)

/* Independent GenDC 1.2 fixture with an image and a custom metadata part. */
static const uint8_t fixture[286] = {
	[0] = 'G', [1] = 'N', [2] = 'D', [3] = 'C',
	[4] = 1, [5] = 2, [9] = 0x10, [12] = 72, [16] = 23,
	[32] = 6, [40] = 0x18, [41] = 1, [48] = 0x18, [49] = 1, [52] = 2,
	[56] = 72, [64] = 128,
	[73] = 0x20, [76] = 56, [82] = 4, [84] = 3,
	[96] = 12, [104] = 1, [112] = 1, [114] = 8, [115] = 1,
	[118] = 1, [120] = 184,
	[129] = 0x20, [132] = 56, [138] = 4, [160] = 9,
	[174] = 1, [176] = 240,
	[185] = 0x42, [188] = 56, [192] = 1, [194] = 8, [195] = 1,
	[200] = 0x18, [201] = 1, [208] = 4, [216] = 0x18, [217] = 1,
	[224] = 2, [228] = 2,
	[240] = 0xf1, [241] = 0x40, [244] = 40,
	[256] = 0x1c, [257] = 1, [264] = 2, [272] = 0x1c, [273] = 1,
	[280] = 1, [281] = 2, [282] = 3, [283] = 4, [284] = 'X', [285] = 'Y',
};

int main(void)
{
	static const uint8_t pixels[] = { 17, 31, 0, 42, 59, 0, 0 };
	struct gc_gendc_image_info image = {
		.container_id = 8,
		.component_type = 1,
		.timestamp_ns = 123400,
		.group_id = 2,
		.region_id = 3,
		.flags = GC_GENDC_TIMESTAMP_PTP,
	};
	uint8_t buffer[512];
	struct gc_frame frame = {
		.data = buffer,
		.capacity = sizeof(pixels),
		.size = sizeof(pixels),
		.width = 2,
		.height = 2,
		.x_offset = 14,
		.x_padding = 1,
		.y_padding = 1,
		.pixel_format = GC_PFNC_MONO8,
		.payload_type = GC_PAYLOAD_IMAGE,
	};
	struct gc_gendc_view view;
	struct gc_gendc_component component;
	struct gc_gendc_part part;
	size_t size;
	size_t i;

	CHECK(gc_gendc_open(fixture, sizeof(fixture), &view) == GC_OK);
	CHECK(view.component_count == 2 && view.id == 23 && view.descriptor_size == 280);
	CHECK(gc_gendc_get_component(&view, 0, &component) == GC_OK);
	CHECK(component.part_count == 1 && component.format == GC_PFNC_MONO8);
	CHECK(component.timestamp_ns == 12 && component.group_id == 4);
	CHECK(gc_gendc_get_part(&view, 0, 0, &part) == GC_OK);
	CHECK(part.width == 2 && part.height == 2 && part.size == 4);
	CHECK(!memcmp(part.data, fixture + 280, 4));
	CHECK(gc_gendc_get_part(&view, 1, 0, &part) == GC_OK);
	CHECK(part.type == 0x40f1 && part.size == 2 && !memcmp(part.data, "XY", 2));
	CHECK(gc_gendc_get_part(&view, 2, 0, &part) == GC_NOT_FOUND);
	CHECK(gc_gendc_get_part(&view, 1, 1, &part) == GC_NOT_FOUND);

	for (i = 0; i < sizeof(fixture); i++)
		CHECK(gc_gendc_open(fixture, i, &view) == GC_PROTOCOL);
	memcpy(buffer, fixture, sizeof(fixture));
	buffer[4] = 2;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_UNSUPPORTED);
	buffer[4] = 1;
	buffer[24] = 1;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_UNSUPPORTED);

	memcpy(buffer, fixture, sizeof(fixture));
	buffer[121] = 0xff;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_PROTOCOL);
	memcpy(buffer, fixture, sizeof(fixture));
	buffer[216] = 0x17;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_PROTOCOL);
	memcpy(buffer, fixture, sizeof(fixture));
	buffer[224] = 250;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_PROTOCOL);
	memcpy(buffer, fixture, sizeof(fixture));
	buffer[130] = 1;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_PROTOCOL);
	buffer[10] = GC_GENDC_COMPONENT_INVALID;
	CHECK(gc_gendc_open(buffer, sizeof(fixture), &view) == GC_OK);
	CHECK(gc_gendc_get_component(&view, 1, &component) == GC_OK && component.invalid);
	CHECK(gc_gendc_get_part(&view, 1, 0, &part) == GC_NOT_FOUND);

	memcpy(buffer, pixels, sizeof(pixels));
	CHECK(gc_gendc_encode_image(NULL, 0, &frame, &image, &size) == GC_BUFFER_SMALL);
	CHECK(size == 176 + sizeof(pixels));
	CHECK(gc_gendc_encode_image(buffer, sizeof(buffer), &frame, &image, &size) == GC_OK);
	CHECK(gc_gendc_open(buffer, size, &view) == GC_OK);
	CHECK(view.id == image.container_id && view.flags == GC_GENDC_TIMESTAMP_PTP);
	CHECK(gc_gendc_get_component(&view, 0, &component) == GC_OK);
	CHECK(component.region_id == 3 && component.x_offset == 14);
	CHECK(gc_gendc_get_part(&view, 0, 0, &part) == GC_OK);
	CHECK(part.x_padding == 1 && part.y_padding == 1 && part.size == sizeof(pixels));
	CHECK(!memcmp(part.data, pixels, sizeof(pixels)));
	CHECK(gc_pfnc_row_size(GC_PFNC_BAYER_RG12P, 5, &size) == GC_OK && size == 8);
	return EXIT_SUCCESS;
}
