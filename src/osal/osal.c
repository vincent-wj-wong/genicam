/*
 * Threading and clock primitives adapted from ipc_core/lib/osal.c.
 *
 * Copyright (C) 2025 - UNI-T Inc.
 * Author: Vincent Wong <vincent.wj.wong@gmail.com>
 */
#include "osal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

struct osal_mutex {
#ifdef _WIN32
	CRITICAL_SECTION native;
#else
	pthread_mutex_t native;
#endif
};

struct osal_thread {
#ifdef _WIN32
	HANDLE native;
#else
	pthread_t native;
#endif
	void (*run)(void *);
	void *arg;
};

struct osal_mutex *osal_mutex_create(void)
{
	struct osal_mutex *lock = malloc(sizeof(*lock));

	if (!lock)
		return NULL;
#ifdef _WIN32
	if (!InitializeCriticalSectionAndSpinCount(&lock->native, 0)) {
#else
	if (pthread_mutex_init(&lock->native, NULL)) {
#endif
		free(lock);
		return NULL;
	}
	return lock;
}

void osal_mutex_destroy(struct osal_mutex *lock)
{
#ifdef _WIN32
	DeleteCriticalSection(&lock->native);
#else
	pthread_mutex_destroy(&lock->native);
#endif
	free(lock);
}

void osal_mutex_lock(struct osal_mutex *lock)
{
#ifdef _WIN32
	EnterCriticalSection(&lock->native);
#else
	pthread_mutex_lock(&lock->native);
#endif
}

void osal_mutex_unlock(struct osal_mutex *lock)
{
#ifdef _WIN32
	LeaveCriticalSection(&lock->native);
#else
	pthread_mutex_unlock(&lock->native);
#endif
}

#ifdef _WIN32
static DWORD WINAPI run_thread(void *arg)
#else
static void *run_thread(void *arg)
#endif
{
	struct osal_thread *thread = arg;

	thread->run(thread->arg);
#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

struct osal_thread *osal_thread_create(void (*run)(void *), void *arg)
{
	struct osal_thread *thread = malloc(sizeof(*thread));

	if (!thread)
		return NULL;
	thread->run = run;
	thread->arg = arg;
#ifdef _WIN32
	thread->native = CreateThread(NULL, 0, run_thread, thread, 0, NULL);
	if (!thread->native) {
#else
	if (pthread_create(&thread->native, NULL, run_thread, thread)) {
#endif
		free(thread);
		return NULL;
	}
	return thread;
}

void osal_thread_join(struct osal_thread *thread)
{
#ifdef _WIN32
	WaitForSingleObject(thread->native, INFINITE);
	CloseHandle(thread->native);
#else
	pthread_join(thread->native, NULL);
#endif
	free(thread);
}

uint64_t osal_monotonic_us(void)
{
#ifdef _WIN32
	LARGE_INTEGER freq;
	LARGE_INTEGER cnt;
	uint64_t f;
	uint64_t c;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&cnt);
	f = (uint64_t)freq.QuadPart;
	c = (uint64_t)cnt.QuadPart;
	return (c / f) * 1000000ULL + ((c % f) * 1000000ULL) / f;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
#endif
}

void osal_msleep(unsigned int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000L,
	};

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
#endif
}

uint64_t osal_deadline(unsigned int ms)
{
	return osal_monotonic_us() + (uint64_t)ms * 1000;
}

unsigned int osal_remaining_ms(uint64_t deadline)
{
	uint64_t now = osal_monotonic_us();
	uint64_t ms;

	if (now >= deadline)
		return 0;
	ms = (deadline - now + 999) / 1000;
	return ms > INT_MAX ? INT_MAX : (unsigned int)ms;
}

int osal_ipv4_parse(const char *text, uint32_t *address)
{
	struct in_addr native;

	if (!text || inet_pton(AF_INET, text, &native) != 1)
		return GC_INVALID;
	*address = ntohl(native.s_addr);
	return GC_OK;
}

void osal_ipv4_format(uint32_t address, char text[16])
{
	snprintf(text, 16, "%u.%u.%u.%u", (unsigned int)(address >> 24),
		 (unsigned int)(address >> 16) & 255,
		 (unsigned int)(address >> 8) & 255,
		 (unsigned int)address & 255);
}

#ifdef _WIN32
int osal_ipv4_interfaces(struct osal_interface *list, size_t capacity,
			 size_t *count)
{
	IP_ADAPTER_ADDRESSES *adapters = NULL;
	IP_ADAPTER_ADDRESSES *adapter;
	IP_ADAPTER_UNICAST_ADDRESS *entry;
	ULONG size = 16384;
	ULONG rc;
	unsigned int attempt;

	*count = 0;
	for (attempt = 0; attempt < 3 && size <= 1024 * 1024; attempt++) {
		adapters = malloc(size);
		if (!adapters)
			return GC_NOMEM;
		rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
					  GAA_FLAG_SKIP_MULTICAST |
					  GAA_FLAG_SKIP_DNS_SERVER,
					  NULL, adapters, &size);
		if (rc == NO_ERROR)
			break;
		free(adapters);
		adapters = NULL;
		if (rc != ERROR_BUFFER_OVERFLOW)
			return rc == ERROR_NO_DATA ? GC_OK : GC_IO;
	}
	if (!adapters)
		return GC_IO;
	for (adapter = adapters; adapter; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp)
			continue;
		for (entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
			const struct sockaddr_in *addr;
			unsigned int prefix = entry->OnLinkPrefixLength;

			addr = (const struct sockaddr_in *)entry->Address.lpSockaddr;
			if (!addr || addr->sin_family != AF_INET || prefix > 32)
				continue;
			if (*count == capacity)
				goto out;
			list[*count].address = ntohl(addr->sin_addr.s_addr);
			list[*count].netmask = prefix ? UINT32_MAX << (32 - prefix) : 0;
			(*count)++;
		}
	}
out:
	free(adapters);
	return GC_OK;
}
#else
int osal_ipv4_interfaces(struct osal_interface *list, size_t capacity,
			 size_t *count)
{
	struct ifaddrs *interfaces;
	struct ifaddrs *entry;

	*count = 0;
	if (getifaddrs(&interfaces))
		return GC_IO;
	for (entry = interfaces; entry && *count < capacity; entry = entry->ifa_next) {
		const struct sockaddr_in *addr;
		const struct sockaddr_in *mask;

		if (!entry->ifa_addr || !entry->ifa_netmask ||
		    entry->ifa_addr->sa_family != AF_INET ||
		    !(entry->ifa_flags & IFF_UP))
			continue;
		addr = (const struct sockaddr_in *)entry->ifa_addr;
		mask = (const struct sockaddr_in *)entry->ifa_netmask;
		list[*count].address = ntohl(addr->sin_addr.s_addr);
		list[*count].netmask = ntohl(mask->sin_addr.s_addr);
		(*count)++;
	}
	freeifaddrs(interfaces);
	return GC_OK;
}
#endif

#ifdef _WIN32
typedef SOCKET native_socket;
typedef int native_socklen;
#else
typedef int native_socket;
typedef socklen_t native_socklen;
#endif

static struct sockaddr_in make_address(struct osal_endpoint endpoint)
{
	struct sockaddr_in addr = { 0 };

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(endpoint.address);
	addr.sin_port = htons(endpoint.port);
	return addr;
}

int osal_udp_open(struct osal_endpoint local, int broadcast, int rx_buffer,
		  osal_socket *sock)
{
	struct sockaddr_in addr = make_address(local);
	native_socket fd;
#ifdef _WIN32
	WSADATA wsa;
	u_long nonblocking = 1;

	if (WSAStartup(MAKEWORD(2, 2), &wsa))
		return GC_IO;
#endif

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if ((osal_socket)fd == OSAL_INVALID_SOCKET) {
#ifdef _WIN32
		WSACleanup();
#endif
		return GC_IO;
	}
	if (broadcast && setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
				    (const char *)&broadcast, sizeof(broadcast)))
		goto error;
	if (rx_buffer)
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rx_buffer,
			   sizeof(rx_buffer));
#ifdef _WIN32
	if (ioctlsocket(fd, FIONBIO, &nonblocking))
#else
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1 ||
	    fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
#endif
		goto error;
	if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)))
		goto error;
	*sock = (osal_socket)fd;
	return GC_OK;

error:
	osal_udp_close((osal_socket)fd);
	return GC_IO;
}

void osal_udp_close(osal_socket sock)
{
	if (sock == OSAL_INVALID_SOCKET)
		return;
#ifdef _WIN32
	closesocket((native_socket)sock);
	WSACleanup();
#else
	close((native_socket)sock);
#endif
}

int osal_udp_connect(osal_socket sock, struct osal_endpoint peer)
{
	struct sockaddr_in addr = make_address(peer);

	if (connect((native_socket)sock, (const struct sockaddr *)&addr,
		    sizeof(addr)))
		return GC_IO;
	return GC_OK;
}

int osal_udp_local(osal_socket sock, struct osal_endpoint *local)
{
	struct sockaddr_in addr;
	native_socklen size = sizeof(addr);

	if (getsockname((native_socket)sock, (struct sockaddr *)&addr, &size))
		return GC_IO;
	local->address = ntohl(addr.sin_addr.s_addr);
	local->port = ntohs(addr.sin_port);
	return GC_OK;
}

int osal_udp_send(osal_socket sock, struct osal_endpoint peer,
		  const void *data, size_t length)
{
	struct sockaddr_in addr = make_address(peer);
	int sent;

	if (length > 65507)
		return GC_INVALID;
	if (peer.port)
		sent = (int)sendto((native_socket)sock, data, (int)length, 0,
				   (const struct sockaddr *)&addr, sizeof(addr));
	else
		sent = (int)send((native_socket)sock, data, (int)length, 0);
	return sent == (int)length ? GC_OK : GC_IO;
}

int osal_udp_receive(osal_socket sock, struct osal_endpoint *peer,
		     void *data, size_t capacity, size_t *size,
		     unsigned int timeout_ms)
{
	uint64_t deadline = osal_deadline(timeout_ms);
	native_socket fd = (native_socket)sock;
	struct sockaddr_in addr;
	native_socklen addr_size;
	unsigned int ms;
	int received;
	int rc;

	do {
		ms = osal_remaining_ms(deadline);
#ifdef _WIN32
		struct timeval tv = { (long)(ms / 1000), (long)(ms % 1000) * 1000 };
		fd_set set;

		FD_ZERO(&set);
		FD_SET(fd, &set);
		rc = select(0, &set, NULL, NULL, &tv);
#else
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		rc = poll(&pfd, 1, (int)ms);
		if (rc < 0 && errno == EINTR)
			continue;
#endif
		if (rc <= 0)
			return rc ? GC_IO : GC_TIMEOUT;
		addr_size = sizeof(addr);
#ifdef _WIN32
		received = recvfrom(fd, data, (int)capacity, 0,
				    (struct sockaddr *)&addr, &addr_size);
		if (received < 0 && WSAGetLastError() == WSAEMSGSIZE)
			return GC_BUFFER_SMALL;
		if (received < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
			continue;
#else
		received = (int)recvfrom(fd, data, capacity, MSG_TRUNC,
					 (struct sockaddr *)&addr, &addr_size);
		if (received < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
#endif
		if (received < 0)
			return GC_IO;
		if ((size_t)received > capacity)
			return GC_BUFFER_SMALL;
		if (peer) {
			peer->address = ntohl(addr.sin_addr.s_addr);
			peer->port = ntohs(addr.sin_port);
		}
		*size = (size_t)received;
		return GC_OK;
	} while (osal_remaining_ms(deadline));

	return GC_TIMEOUT;
}
