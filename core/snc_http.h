// A small HTTPS/1.1 client on mbedTLS: one keep-alive connection to one host.
// Blocking; run it on the network thread. Works with mbedTLS 2.28 and 3.x.
#ifndef SNC_HTTP_H
#define SNC_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "snc_errors.h"  // the SNC_E_* transport errors (an HTTP error status is not one of them)

typedef struct {
	char host[64];
	char user_agent[96];
	char sdk[48];
	uint32_t ip;          // cached address of host (0 = not yet)
	int fd;               // the socket, or -1
	int connected;        // TLS session up
	int reused;           // this request rides an existing connection
	int setup;            // mbedTLS contexts initialised
	volatile int cancel;  // set from any thread to abort the request in flight
	char err[120];        // what went wrong, for the screen
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_ssl_config conf;
	mbedtls_ssl_context ssl;
	mbedtls_x509_crt ca;
} snc_http;

// host: "www.smashandclash.in". user_agent and sdk identify the client in requests.
int snc_http_init(snc_http *h, const char *host, const char *user_agent, const char *sdk);

// One request. body is JSON (or NULL). The answer's body lands in out (NUL-terminated).
// Returns the body's length (>= 0) with *status set, or an SNC_E_* error.
int snc_http_request(snc_http *h, const char *method, const char *path, const char *token,
                     const char *body, char *out, size_t cap, int timeout_ms, int *status);

// Abort the request in flight (it returns SNC_E_CANCEL) and drop the connection.
void snc_http_cancel(snc_http *h);

void snc_http_close(snc_http *h);

#endif
