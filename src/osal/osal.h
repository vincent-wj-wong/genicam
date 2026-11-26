#ifndef GC_OSAL_H
#define GC_OSAL_H

#include <genicam/frame.h>
#include <genicam/status.h>

#define OSAL_INVALID_SOCKET UINTPTR_MAX

typedef uintptr_t osal_socket;

struct osal_endpoint {
	uint32_t address;
	uint16_t port;
};

struct osal_interface {
	uint32_t address;
	uint32_t netmask;
};

struct osal_mutex;
struct osal_thread;

struct osal_mutex *osal_mutex_create(void);
void osal_mutex_destroy(struct osal_mutex *lock);
void osal_mutex_lock(struct osal_mutex *lock);
void osal_mutex_unlock(struct osal_mutex *lock);
struct osal_thread *osal_thread_create(void (*run)(void *), void *arg);
void osal_thread_join(struct osal_thread *thread);
uint64_t osal_monotonic_us(void);
void osal_msleep(unsigned int ms);
uint64_t osal_deadline(unsigned int ms);
unsigned int osal_remaining_ms(uint64_t deadline);
int osal_ipv4_parse(const char *text, uint32_t *address);
void osal_ipv4_format(uint32_t address, char text[16]);
int osal_ipv4_interfaces(struct osal_interface *list, size_t capacity,
			 size_t *count);
int osal_udp_open(struct osal_endpoint local, int broadcast, int rx_buffer,
		  osal_socket *sock);
void osal_udp_close(osal_socket sock);
int osal_udp_connect(osal_socket sock, struct osal_endpoint peer);
int osal_udp_local(osal_socket sock, struct osal_endpoint *local);
int osal_udp_send(osal_socket sock, struct osal_endpoint peer,
		  const void *data, size_t length);
int osal_udp_receive(osal_socket sock, struct osal_endpoint *peer,
		     void *data, size_t capacity, size_t *size,
		     unsigned int timeout_ms);

#endif /* GC_OSAL_H */
