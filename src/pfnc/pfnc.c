#include <genicam/pfnc.h>

#include "core/internal.h"

struct pixel_format {
	uint32_t code;
	const char *name;
	unsigned int bits;
};

static const struct pixel_format formats[] = {
	{ GC_PFNC_MONO8, "Mono8", 8 },
	{ GC_PFNC_MONO10, "Mono10", 16 },
	{ GC_PFNC_MONO12, "Mono12", 16 },
	{ GC_PFNC_MONO16, "Mono16", 16 },
	{ GC_PFNC_BAYER_GR8, "BayerGR8", 8 },
	{ GC_PFNC_BAYER_RG8, "BayerRG8", 8 },
	{ GC_PFNC_BAYER_GB8, "BayerGB8", 8 },
	{ GC_PFNC_BAYER_BG8, "BayerBG8", 8 },
	{ GC_PFNC_RGB8, "RGB8", 24 },
	{ GC_PFNC_BGR8, "BGR8", 24 },
	{ GC_PFNC_MONO10P, "Mono10p", 10 },
	{ GC_PFNC_MONO12P, "Mono12p", 12 },
};

static const struct pixel_format *find_format(uint32_t code)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if (formats[i].code == code)
			return &formats[i];
	}
	return NULL;
}

const char *gc_pfnc_name(uint32_t code)
{
	const struct pixel_format *format = find_format(code);

	return format ? format->name : NULL;
}

int gc_pfnc_row_size(uint32_t code, uint32_t width, size_t *size)
{
	const struct pixel_format *format = find_format(code);
	uint64_t bytes;

	if (!size || !width)
		return GC_INVALID;
	if (!format)
		return GC_UNSUPPORTED;
	bytes = ((uint64_t)width * format->bits + 7) / 8;
	if (bytes > SIZE_MAX)
		return GC_INVALID;
	*size = (size_t)bytes;
	return GC_OK;
}
