/* GenICam GenDC 1.2 sections 2.2 and 4.1, final linear containers. */
#include <genicam/gendc.h>
#include <genicam/pfnc.h>

#include "common/internal.h"

#include <string.h>

#define CONTAINER_TYPE	0x1000
#define COMPONENT_TYPE	0x2000
#define PART_TYPE_MASK	0xf000
#define PART_TYPE_VALUE	0x4000
#define CONTAINER_FIXED	56
#define COMPONENT_FIXED	48
#define PART_FIXED	40
#define IMAGE_HEADER	56
#define ARRAY_OFFSET	8
#define IMAGE_DESCRIPTOR (CONTAINER_FIXED + COMPONENT_FIXED + IMAGE_HEADER + 2 * ARRAY_OFFSET)

static int container_header(const struct gc_gendc_view *view, size_t *size)
{
	uint32_t count = view->component_count;
	uint32_t header_size;

	if (view->size < CONTAINER_FIXED || view->descriptor_size > view->data_offset ||
	    view->data_offset > view->size || view->data_size > view->size - view->data_offset)
		return GC_PROTOCOL;
	header_size = gc_le32(view->bytes + 12);
	if (header_size < CONTAINER_FIXED || header_size > view->descriptor_size ||
	    !count || count > (header_size - CONTAINER_FIXED) / ARRAY_OFFSET)
		return GC_PROTOCOL;
	*size = header_size;
	return GC_OK;
}

static int header_at(const struct gc_gendc_view *view, uint64_t offset,
		     size_t minimum, const uint8_t **header, size_t *length)
{
	const uint8_t *data;
	size_t container_size;
	uint32_t size;
	int rc;

	rc = container_header(view, &container_size);
	if (rc)
		return rc;
	if (offset < container_size || offset > view->descriptor_size ||
	    view->descriptor_size - (size_t)offset < minimum)
		return GC_PROTOCOL;
	data = view->bytes + (size_t)offset;
	size = gc_le32(data + 4);
	if (size < minimum || size > view->descriptor_size - (size_t)offset)
		return GC_PROTOCOL;
	*header = data;
	*length = size;
	return GC_OK;
}

static int read_component(const struct gc_gendc_view *view, uint32_t index,
			  const uint8_t **header, struct gc_gendc_component *component)
{
	const uint8_t *data;
	uint64_t offset;
	size_t length;
	size_t table_size;
	uint16_t count;
	int rc;

	if (index >= view->component_count)
		return GC_NOT_FOUND;
	rc = container_header(view, &table_size);
	if (rc)
		return rc;
	offset = gc_le64(view->bytes + CONTAINER_FIXED + (size_t)index * ARRAY_OFFSET);
	rc = header_at(view, offset, COMPONENT_FIXED, &data, &length);
	if (rc)
		return rc;
	count = gc_le16(data + 46);
	if (gc_le16(data) != COMPONENT_TYPE || !count ||
	    count > (length - COMPONENT_FIXED) / ARRAY_OFFSET)
		return GC_PROTOCOL;

	*component = (struct gc_gendc_component) {
		.group_id = gc_le16(data + 10),
		.source_id = gc_le16(data + 12),
		.region_id = gc_le16(data + 14),
		.x_offset = gc_le32(data + 16),
		.y_offset = gc_le32(data + 20),
		.timestamp_ns = gc_le64(data + 24),
		.type_id = gc_le64(data + 32),
		.format = gc_le32(data + 40),
		.part_count = count,
		.invalid = !!(gc_le16(data + 2) & 1),
	};
	if (component->invalid && !(view->flags & GC_GENDC_COMPONENT_INVALID))
		return GC_PROTOCOL;
	if (!component->invalid && component->timestamp_ns > INT64_MAX)
		return GC_PROTOCOL;
	*header = data;
	return GC_OK;
}

static int check_image_size(const struct gc_gendc_part *part)
{
	size_t row;
	int rc;

	rc = gc_pfnc_row_size(part->format, part->width, &row);
	if (rc == GC_UNSUPPORTED)
		return GC_OK;
	if (rc || row > SIZE_MAX - part->x_padding || part->size < part->y_padding)
		return GC_PROTOCOL;
	row += part->x_padding;
	return part->height <= (part->size - part->y_padding) / row ? GC_OK : GC_PROTOCOL;
}

static int read_part(const struct gc_gendc_view *view, const uint8_t *component,
		     uint16_t index, struct gc_gendc_part *part)
{
	const uint8_t *header;
	uint64_t offset;
	uint64_t data_offset;
	uint64_t data_size;
	size_t length;
	uint16_t type;
	int rc;

	offset = gc_le64(component + COMPONENT_FIXED + (size_t)index * ARRAY_OFFSET);
	rc = header_at(view, offset, PART_FIXED, &header, &length);
	if (rc)
		return rc;
	type = gc_le16(header);
	if ((type & PART_TYPE_MASK) != PART_TYPE_VALUE)
		return GC_PROTOCOL;
	data_size = gc_le64(header + 24);
	offset = gc_le64(header + 32);
	if (offset < view->data_offset)
		return GC_PROTOCOL;
	data_offset = offset - view->data_offset;
	if (data_offset > view->data_size || data_size > view->data_size - data_offset ||
	    (!gc_le16(header + 14) && gc_le64(header + 16) != offset))
		return GC_PROTOCOL;

	*part = (struct gc_gendc_part) {
		.type = type,
		.flags = gc_le16(header + 2),
		.format = gc_le32(header + 8),
		.flow_id = gc_le16(header + 14),
		.flow_offset = gc_le64(header + 16),
		.data = view->bytes + (size_t)offset,
		.size = (size_t)data_size,
	};
	if (type >= GC_GENDC_IMAGE && type <= GC_GENDC_JPEG_XS) {
		if (length < IMAGE_HEADER)
			return GC_PROTOCOL;
		part->width = gc_le32(header + 40);
		part->height = gc_le32(header + 44);
		part->x_padding = gc_le16(header + 48);
		part->y_padding = gc_le16(header + 50);
		if (!part->width || !part->height)
			return GC_PROTOCOL;
		if (type == GC_GENDC_IMAGE)
			return check_image_size(part);
	}
	return GC_OK;
}

int gc_gendc_open(const void *data, size_t size, struct gc_gendc_view *view)
{
	const uint8_t *bytes = data;
	struct gc_gendc_component component;
	struct gc_gendc_part part;
	struct gc_gendc_view result;
	const uint8_t *header;
	uint64_t data_offset;
	uint64_t data_size;
	size_t part_budget;
	size_t header_size;
	uint32_t i;
	uint16_t j;
	int rc;

	if (!data || !view)
		return GC_INVALID;
	*view = (struct gc_gendc_view){ 0 };
	if (size < CONTAINER_FIXED || memcmp(bytes, "GNDC", 4))
		return GC_PROTOCOL;
	if (bytes[4] != GC_GENDC_VERSION_MAJOR || gc_le16(bytes + 24))
		return GC_UNSUPPORTED;
	if (gc_le16(bytes + 8) != CONTAINER_TYPE)
		return GC_PROTOCOL;
	data_offset = gc_le64(bytes + 40);
	data_size = gc_le64(bytes + 32);
	result = (struct gc_gendc_view) {
		.bytes = bytes,
		.size = size,
		.descriptor_size = gc_le32(bytes + 48),
		.component_count = gc_le32(bytes + 52),
		.id = gc_le64(bytes + 16),
		.flags = gc_le16(bytes + 10),
		.version_major = bytes[4],
		.version_minor = bytes[5],
		.version_subminor = bytes[6],
	};
	if (data_offset < result.descriptor_size || data_offset > size ||
	    data_size > size - (size_t)data_offset)
		return GC_PROTOCOL;
	result.data_offset = (size_t)data_offset;
	result.data_size = (size_t)data_size;
	rc = container_header(&result, &header_size);
	if (rc)
		return rc;
	part_budget = (result.descriptor_size - header_size) / PART_FIXED;
	for (i = 0; i < result.component_count; i++) {
		rc = read_component(&result, i, &header, &component);
		if (rc)
			return rc;
		if (component.part_count > part_budget)
			return GC_PROTOCOL;
		part_budget -= component.part_count;
		if (component.invalid)
			continue;
		for (j = 0; j < component.part_count; j++) {
			rc = read_part(&result, header, j, &part);
			if (rc)
				return rc;
		}
	}
	*view = result;
	return GC_OK;
}

int gc_gendc_get_component(const struct gc_gendc_view *view, uint32_t index,
			    struct gc_gendc_component *component)
{
	const uint8_t *header;

	if (!view || !view->bytes || !component)
		return GC_INVALID;
	return read_component(view, index, &header, component);
}

int gc_gendc_get_part(const struct gc_gendc_view *view, uint32_t component_index,
		       uint16_t index, struct gc_gendc_part *part)
{
	struct gc_gendc_component component;
	const uint8_t *header;
	int rc;

	if (!view || !view->bytes || !part)
		return GC_INVALID;
	rc = read_component(view, component_index, &header, &component);
	if (rc)
		return rc;
	if (component.invalid || index >= component.part_count)
		return GC_NOT_FOUND;
	return read_part(view, header, index, part);
}

int gc_gendc_encode_image(void *data, size_t capacity, const struct gc_frame *frame,
			   const struct gc_gendc_image_info *info, size_t *size)
{
	uint8_t *bytes = data;
	uint8_t *component;
	uint8_t *part;
	struct gc_frame image;
	size_t expected;
	int rc;

	if (!frame || !frame->data || !info || !size ||
	    info->timestamp_ns > INT64_MAX || (info->flags & ~GC_GENDC_TIMESTAMP_PTP))
		return GC_INVALID;
	if (frame->payload_type != GC_PAYLOAD_IMAGE)
		return GC_UNSUPPORTED;
	image = *frame;
	rc = gc_image_layout(&image, &expected);
	if (rc)
		return rc;
	if (frame->size != expected || frame->capacity < expected ||
	    (frame->stride && frame->stride != image.stride) ||
	    expected > SIZE_MAX - IMAGE_DESCRIPTOR)
		return GC_INVALID;

	*size = IMAGE_DESCRIPTOR + expected;
	if (!bytes || capacity < *size)
		return GC_BUFFER_SMALL;
	component = bytes + CONTAINER_FIXED + ARRAY_OFFSET;
	part = component + COMPONENT_FIXED + ARRAY_OFFSET;
	memmove(bytes + IMAGE_DESCRIPTOR, frame->data, expected);
	memset(bytes, 0, IMAGE_DESCRIPTOR);

	memcpy(bytes, "GNDC", 4);
	bytes[4] = GC_GENDC_VERSION_MAJOR;
	bytes[5] = GC_GENDC_VERSION_MINOR;
	gc_put_le16(bytes + 8, CONTAINER_TYPE);
	gc_put_le16(bytes + 10, info->flags);
	gc_put_le32(bytes + 12, CONTAINER_FIXED + ARRAY_OFFSET);
	gc_put_le64(bytes + 16, info->container_id);
	gc_put_le64(bytes + 32, expected);
	gc_put_le64(bytes + 40, IMAGE_DESCRIPTOR);
	gc_put_le32(bytes + 48, IMAGE_DESCRIPTOR);
	gc_put_le32(bytes + 52, 1);
	gc_put_le64(bytes + CONTAINER_FIXED, CONTAINER_FIXED + ARRAY_OFFSET);

	gc_put_le16(component, COMPONENT_TYPE);
	gc_put_le32(component + 4, COMPONENT_FIXED + ARRAY_OFFSET);
	gc_put_le16(component + 10, info->group_id);
	gc_put_le16(component + 12, info->source_id);
	gc_put_le16(component + 14, info->region_id);
	gc_put_le32(component + 16, frame->x_offset);
	gc_put_le32(component + 20, frame->y_offset);
	gc_put_le64(component + 24, info->timestamp_ns);
	gc_put_le64(component + 32, info->component_type);
	gc_put_le32(component + 40, frame->pixel_format);
	gc_put_le16(component + 46, 1);
	gc_put_le64(component + COMPONENT_FIXED, (size_t)(part - bytes));

	gc_put_le16(part, GC_GENDC_IMAGE);
	gc_put_le32(part + 4, IMAGE_HEADER);
	gc_put_le32(part + 8, frame->pixel_format);
	gc_put_le64(part + 16, IMAGE_DESCRIPTOR);
	gc_put_le64(part + 24, expected);
	gc_put_le64(part + 32, IMAGE_DESCRIPTOR);
	gc_put_le32(part + 40, frame->width);
	gc_put_le32(part + 44, frame->height);
	gc_put_le16(part + 48, frame->x_padding);
	gc_put_le16(part + 50, frame->y_padding);
	return GC_OK;
}
