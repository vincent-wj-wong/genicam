#ifndef GENICAM_STATUS_H
#define GENICAM_STATUS_H


#ifdef __cplusplus
extern "C" {
#endif

enum gc_status {
	GC_OK = 0,
	GC_INVALID = -1,
	GC_NOMEM = -2,
	GC_IO = -3,
	GC_TIMEOUT = -4,
	GC_PROTOCOL = -5,
	GC_ACCESS = -6,
	GC_UNSUPPORTED = -7,
	GC_NOT_FOUND = -8,
	GC_BUFFER_SMALL = -9,
	GC_BUSY = -10,
	GC_STOPPED = -11,
};

const char *gc_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif /* GENICAM_STATUS_H */
