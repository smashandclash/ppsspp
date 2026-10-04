// The platform layer on the PSP: sceNetInet sockets, the system resolver, the RTC.
// Runs on the network thread (lower priority than the screen's, so drawing never waits).
#include <netinet/in.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psprtc.h>
#include <pspctrl.h>
#include <string.h>
#include <time.h>

#include "snc_platform.h"

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x01
#endif

int snc_net_resolve(const char *host, uint32_t *ip) {
	static char buf[1024];
	int rid = -1;
	if (sceNetResolverCreate(&rid, buf, sizeof buf) < 0) return -1;
	struct in_addr addr;
	memset(&addr, 0, sizeof addr);
	int r = sceNetResolverStartNtoA(rid, host, &addr, 5, 3);
	sceNetResolverDelete(rid);
	if (r < 0 || !addr.s_addr) return -1;
	*ip = addr.s_addr;
	return 0;
}

int snc_net_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
	(void)timeout_ms;
	int fd = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_len = sizeof sa;
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = ip;
	if (sceNetInetConnect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		sceNetInetClose(fd);
		return -1;
	}
	int one = 1;
	sceNetInetSetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

int snc_net_send(int fd, const unsigned char *buf, size_t len) {
	int n = (int)sceNetInetSend(fd, buf, len, 0);
	return n > 0 ? n : -1;
}

int snc_net_recv(int fd, unsigned char *buf, size_t len, int timeout_ms) {
	SceNetInetPollfd p = {fd, SCE_NET_INET_POLLIN, 0};
	int r = sceNetInetPoll(&p, 1, timeout_ms);
	if (r == 0) return SNC_NET_TIMEOUT;
	if (r < 0) return -1;
	int n = (int)sceNetInetRecv(fd, buf, len, 0);
	return n >= 0 ? n : -1;
}

void snc_net_close(int fd) {
	sceNetInetClose(fd);
}

int snc_net_udp_exchange(uint32_t ip, uint16_t port, const unsigned char *req, size_t reqlen, unsigned char *resp, size_t cap, int timeout_ms) {
	int fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_len = sizeof sa;
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = ip;
	int n = -1;
	if ((int)sceNetInetSendto(fd, req, reqlen, 0, (struct sockaddr *)&sa, sizeof sa) >= 0) {
		SceNetInetPollfd p = {fd, SCE_NET_INET_POLLIN, 0};
		if (sceNetInetPoll(&p, 1, timeout_ms) == 1) n = (int)sceNetInetRecvfrom(fd, resp, cap, 0, NULL, NULL);
	}
	sceNetInetClose(fd);
	return n;
}

uint32_t snc_now_ms(void) {
	return (uint32_t)(sceKernelGetSystemTimeWide() / 1000);
}

void snc_entropy(unsigned char *out, size_t len) {
	// No hardware RNG that a game can reach: stir the microsecond clock, the RTC tick,
	// the stick and a Mersenne twister seeded from all of them.
	u64 tick = 0;
	sceRtcGetCurrentTick(&tick);
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);
	uint32_t s = (uint32_t)sceKernelGetSystemTimeWide() ^ (uint32_t)tick ^ ((uint32_t)(tick >> 32) * 2654435761u) ^ ((uint32_t)pad.Lx << 24) ^
	             ((uint32_t)pad.Ly << 16) ^ pad.Buttons;
	SceKernelUtilsMt19937Context mt;
	sceKernelUtilsMt19937Init(&mt, s);
	for (size_t i = 0; i < len; i++) {
		uint32_t r = sceKernelUtilsMt19937UInt(&mt) ^ (uint32_t)sceKernelGetSystemTimeWide() * 2246822519u;
		r ^= r >> 15;
		out[i] = (unsigned char)(r >> 7);
	}
}

int snc_clock_year(void) {
	ScePspDateTime t;
	if (sceRtcGetCurrentClockLocalTime(&t) < 0) return 2000;
	return t.year;
}
