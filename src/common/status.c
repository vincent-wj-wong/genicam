#include <genicam/status.h>

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
