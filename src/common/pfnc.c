/* GenICam PFNC 2.5 pixel formats and row layout subset. */
#include <genicam/pfnc.h>

#include "common/internal.h"

struct pixel_format {
	uint32_t code;
	const char *name;
	unsigned int bits;
};

static const struct pixel_format formats[] = {
	{ GC_PFNC_MONO8, "Mono8", 8 },
	{ GC_PFNC_MONO8S, "Mono8s", 8 },
	{ GC_PFNC_MONO10, "Mono10", 16 },
	{ GC_PFNC_MONO12, "Mono12", 16 },
	{ GC_PFNC_MONO16, "Mono16", 16 },
	{ GC_PFNC_BAYER_GR8, "BayerGR8", 8 },
	{ GC_PFNC_BAYER_RG8, "BayerRG8", 8 },
	{ GC_PFNC_BAYER_GB8, "BayerGB8", 8 },
	{ GC_PFNC_BAYER_BG8, "BayerBG8", 8 },
	{ GC_PFNC_BAYER_GR10, "BayerGR10", 16 },
	{ GC_PFNC_BAYER_RG10, "BayerRG10", 16 },
	{ GC_PFNC_BAYER_GB10, "BayerGB10", 16 },
	{ GC_PFNC_BAYER_BG10, "BayerBG10", 16 },
	{ GC_PFNC_BAYER_GR12, "BayerGR12", 16 },
	{ GC_PFNC_BAYER_RG12, "BayerRG12", 16 },
	{ GC_PFNC_BAYER_GB12, "BayerGB12", 16 },
	{ GC_PFNC_BAYER_BG12, "BayerBG12", 16 },
	{ GC_PFNC_RGB8, "RGB8", 24 },
	{ GC_PFNC_BGR8, "BGR8", 24 },
	{ GC_PFNC_RGBA8, "RGBa8", 32 },
	{ GC_PFNC_BGRA8, "BGRa8", 32 },
	{ GC_PFNC_MONO14, "Mono14", 16 },
	{ GC_PFNC_BAYER_GR16, "BayerGR16", 16 },
	{ GC_PFNC_BAYER_RG16, "BayerRG16", 16 },
	{ GC_PFNC_BAYER_GB16, "BayerGB16", 16 },
	{ GC_PFNC_BAYER_BG16, "BayerBG16", 16 },
	{ GC_PFNC_RGB16, "RGB16", 48 },
	{ GC_PFNC_MONO10P, "Mono10p", 10 },
	{ GC_PFNC_MONO12P, "Mono12p", 12 },
	{ GC_PFNC_BGR16, "BGR16", 48 },
	{ GC_PFNC_BAYER_BG10P, "BayerBG10p", 10 },
	{ GC_PFNC_BAYER_BG12P, "BayerBG12p", 12 },
	{ GC_PFNC_BAYER_GB10P, "BayerGB10p", 10 },
	{ GC_PFNC_BAYER_GB12P, "BayerGB12p", 12 },
	{ GC_PFNC_BAYER_GR10P, "BayerGR10p", 10 },
	{ GC_PFNC_BAYER_GR12P, "BayerGR12p", 12 },
	{ GC_PFNC_BAYER_RG10P, "BayerRG10p", 10 },
	{ GC_PFNC_BAYER_RG12P, "BayerRG12p", 12 },
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
