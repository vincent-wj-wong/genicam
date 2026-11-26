/* GenICam GenApi 2.1.1 compressed device descriptions. */
#include "host/internal.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)le16(p + 2) << 16 | le16(p);
}

static int unpack_entry(const uint8_t *zip, size_t size, const uint8_t *entry,
			void **xml, size_t *xml_size)
{
	uint32_t offset = le32(entry + 42);
	uint32_t compressed = le32(entry + 20);
	uint32_t expanded = le32(entry + 24);
	unsigned int method = le16(entry + 10);
	unsigned int name_size = le16(entry + 28);
	const uint8_t *local;
	uint8_t *data;
	size_t start;
	z_stream stream = { 0 };
	int rc;

	if (le16(entry + 8) & 1)
		return GC_UNSUPPORTED;
	if (!expanded || expanded > GC_MAX_XML_SIZE ||
	    offset > size || size - offset < 30)
		return GC_PROTOCOL;
	local = zip + offset;
	start = (size_t)offset + 30 + le16(local + 26) + le16(local + 28);
	if (le32(local) != 0x04034b50 || le16(local + 8) != method ||
	    start > size || compressed > size - start ||
	    le16(local + 26) != name_size ||
	    memcmp(local + 30, entry + 46, name_size))
		return GC_PROTOCOL;

	data = malloc(expanded);
	if (!data)
		return GC_NOMEM;
	switch (method) {
	case 0:
		if (compressed != expanded)
			goto malformed;
		memcpy(data, zip + start, expanded);
		break;
	case 8:
		stream.next_in = (Bytef *)(zip + start);
		stream.avail_in = compressed;
		stream.next_out = data;
		stream.avail_out = expanded;
		if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
			goto malformed;
		rc = inflate(&stream, Z_FINISH);
		inflateEnd(&stream);
		if (rc != Z_STREAM_END || stream.total_out != expanded ||
		    stream.total_in != compressed)
			goto malformed;
		break;
	default:
		free(data);
		return GC_UNSUPPORTED;
	}
	if (crc32(0, data, expanded) != le32(entry + 16))
		goto malformed;
	*xml = data;
	*xml_size = expanded;
	return GC_OK;

malformed:
	free(data);
	return GC_PROTOCOL;
}

int gc_zip_xml(const uint8_t *zip, size_t size, void **xml, size_t *xml_size)
{
	size_t end;
	size_t offset;
	size_t remaining;
	size_t length;
	unsigned int entries;
	unsigned int name_size;
	const uint8_t *entry;

	if (size < 22 || size > GC_MAX_XML_SIZE)
		return GC_PROTOCOL;
	end = size - 22;
	while (le32(zip + end) != 0x06054b50 ||
	       le16(zip + end + 20) != size - end - 22) {
		if (!end || size - end > 65557)
			return GC_PROTOCOL;
		end--;
	}
	entries = le16(zip + end + 10);
	if (le16(zip + end + 4) || le16(zip + end + 6) ||
	    entries != le16(zip + end + 8) || entries > 1024)
		return GC_UNSUPPORTED;
	offset = le32(zip + end + 16);
	remaining = le32(zip + end + 12);
	if (offset > end || remaining > end - offset)
		return GC_PROTOCOL;

	while (entries--) {
		if (remaining < 46)
			return GC_PROTOCOL;
		entry = zip + offset;
		if (le32(entry) != 0x02014b50)
			return GC_PROTOCOL;
		name_size = le16(entry + 28);
		length = 46U + name_size + le16(entry + 30) + le16(entry + 32);
		if (length > remaining)
			return GC_PROTOCOL;
		if (name_size >= 4 &&
		    !memcmp(entry + 46 + name_size - 4, ".xml", 4))
			return unpack_entry(zip, offset, entry, xml, xml_size);
		offset += length;
		remaining -= length;
	}
	return GC_NOT_FOUND;
}
