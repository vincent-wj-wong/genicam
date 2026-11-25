#include "internal.h"

const char *gc_status_string(int status)
{
	switch (status) {
	case GC_OK:
		return "success";
	case GC_INVALID:
		return "invalid argument";
	case GC_NOMEM:
		return "out of memory";
	case GC_IO:
		return "I/O error";
	case GC_TIMEOUT:
		return "timed out";
	case GC_PROTOCOL:
		return "invalid protocol data";
	case GC_ACCESS:
		return "access denied";
	case GC_UNSUPPORTED:
		return "unsupported";
	case GC_NOT_FOUND:
		return "not found";
	case GC_BUFFER_SMALL:
		return "buffer too small";
	case GC_BUSY:
		return "busy";
	case GC_STOPPED:
		return "stopped";
	default:
		return "unknown error";
	}
}

void gc_device_close(struct gc_device *device)
{
	if (device)
		device->ops->close(device->context);
}

int gc_device_read(struct gc_device *device, uint64_t address, void *data,
		   size_t size)
{
	if (!device || (!data && size) || (size && size - 1 > UINT64_MAX - address))
		return GC_INVALID;
	if (!size)
		return GC_OK;
	return device->ops->read(device->context, address, data, size);
}

int gc_device_write(struct gc_device *device, uint64_t address, const void *data,
		    size_t size)
{
	if (!device || (!data && size) || (size && size - 1 > UINT64_MAX - address))
		return GC_INVALID;
	if (!size)
		return GC_OK;
	return device->ops->write(device->context, address, data, size);
}

int gc_device_read_register(struct gc_device *device, uint64_t address,
			    uint32_t *value)
{
	if (!device || !value)
		return GC_INVALID;
	return device->ops->read_reg(device->context, address, value);
}

int gc_device_write_register(struct gc_device *device, uint64_t address,
			     uint32_t value)
{
	if (!device)
		return GC_INVALID;
	return device->ops->write_reg(device->context, address, value);
}

int gc_stream_open(struct gc_device *device, struct gc_stream **stream)
{
	if (!device || !stream)
		return GC_INVALID;
	*stream = NULL;
	return device->ops->stream_open(device->context, stream);
}

int gc_stream_start(struct gc_stream *stream)
{
	return stream ? stream->ops->start(stream->context) : GC_INVALID;
}

int gc_stream_stop(struct gc_stream *stream)
{
	return stream ? stream->ops->stop(stream->context) : GC_INVALID;
}

int gc_stream_receive(struct gc_stream *stream, struct gc_frame *frame,
		      unsigned int timeout_ms)
{
	if (!stream || !frame || !frame->data || !frame->capacity)
		return GC_INVALID;
	frame->size = 0;
	return stream->ops->receive(stream->context, frame, timeout_ms);
}

int gc_stream_get_stats(struct gc_stream *stream, struct gc_stream_stats *stats)
{
	if (!stream || !stats)
		return GC_INVALID;
	return stream->ops->stats(stream->context, stats);
}

void gc_stream_close(struct gc_stream *stream)
{
	if (stream)
		stream->ops->close(stream->context);
}
