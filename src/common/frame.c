/* PFNC 2.5 image layout with transport padding. */
#include <genicam/pfnc.h>

#include "internal.h"

int gc_image_layout(struct gc_frame *frame, size_t *size)
{
	size_t row;
	int rc;

	if (!frame->width || !frame->height)
		return GC_PROTOCOL;
	rc = gc_pfnc_row_size(frame->pixel_format, frame->width, &row);
	if (rc)
		return rc;
	if (row > GC_IMAGE_LIMIT || frame->x_padding > GC_IMAGE_LIMIT - row)
		return GC_PROTOCOL;
	frame->stride = row + frame->x_padding;
	if (frame->height > (GC_IMAGE_LIMIT - frame->y_padding) / frame->stride)
		return GC_PROTOCOL;
	*size = frame->stride * frame->height + frame->y_padding;
	return GC_OK;
}
