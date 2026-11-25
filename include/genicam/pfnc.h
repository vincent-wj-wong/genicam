#ifndef GENICAM_PFNC_H
#define GENICAM_PFNC_H

#include <genicam/genicam.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gc_pfnc_code {
	GC_PFNC_MONO8 = 0x01080001,
	GC_PFNC_MONO10 = 0x01100003,
	GC_PFNC_MONO12 = 0x01100005,
	GC_PFNC_MONO16 = 0x01100007,
	GC_PFNC_BAYER_GR8 = 0x01080008,
	GC_PFNC_BAYER_RG8 = 0x01080009,
	GC_PFNC_BAYER_GB8 = 0x0108000a,
	GC_PFNC_BAYER_BG8 = 0x0108000b,
	GC_PFNC_RGB8 = 0x02180014,
	GC_PFNC_BGR8 = 0x02180015,
	GC_PFNC_MONO10P = 0x010a0046,
	GC_PFNC_MONO12P = 0x010c0047,
};

const char *gc_pfnc_name(uint32_t code);
int gc_pfnc_row_size(uint32_t code, uint32_t width, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_PFNC_H */
