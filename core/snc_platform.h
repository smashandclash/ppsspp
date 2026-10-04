// What the core needs from a platform (Nintendo DS, PSP, or a desktop test build).
// Each platform implements these in its own net_*.c. Everything here runs on the
// network thread, except snc_now_ms(), which any thread may call.
#ifndef SNC_PLATFORM_H
#define SNC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define SNC_NET_TIMEOUT (-2)

// The system resolver. ip is an IPv4 address in network byte order. 0 = ok.
int snc_net_resolve(const char *host, uint32_t *ip);

// A TCP connection to ip:port. Returns a socket (>= 0) or a negative error.
int snc_net_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);

// Bytes sent (> 0), or a negative error.
int snc_net_send(int fd, const unsigned char *buf, size_t len);

// Bytes received (> 0), 0 when the other side closed, SNC_NET_TIMEOUT, or a negative error.
int snc_net_recv(int fd, unsigned char *buf, size_t len, int timeout_ms);

void snc_net_close(int fd);

// One UDP request and its reply (used for DNS when the system resolver fails).
// Returns the reply's length, or a negative error.
int snc_net_udp_exchange(uint32_t ip, uint16_t port, const unsigned char *req, size_t reqlen,
                         unsigned char *resp, size_t cap, int timeout_ms);

// Milliseconds from any fixed point (monotonic).
uint32_t snc_now_ms(void);

// Seed bytes for the TLS random generator: the best the platform has.
void snc_entropy(unsigned char *out, size_t len);

// The calendar year by the system clock (to tell an unset clock from a real one).
int snc_clock_year(void);

#endif
