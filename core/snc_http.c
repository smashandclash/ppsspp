// A small HTTPS/1.1 client on mbedTLS (2.28 or 3.x): one keep-alive connection to one host.
#include "snc_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/error.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#include "snc_certs.h"
#include "snc_platform.h"

#define SLICE_MS 200  // how long one socket read may block before we look at cancel/deadline

/* ------------------------------------ DNS ------------------------------------ */

// Public resolvers to ask when the system's fails (a WFC DNS server may not answer
// for every name). 1.1.1.1, 8.8.8.8, 9.9.9.9 in network byte order.
static const uint8_t FALLBACK_DNS[][4] = {{1, 1, 1, 1}, {8, 8, 8, 8}, {9, 9, 9, 9}};

static int dns_query(uint32_t server, const char *host, uint32_t *ip) {
	unsigned char q[300], r[512];
	size_t n = 0;
	uint16_t id = (uint16_t)(snc_now_ms() * 2654435761u >> 16);
	q[n++] = id >> 8;
	q[n++] = id & 0xff;
	q[n++] = 0x01;  // recursion desired
	q[n++] = 0x00;
	q[n++] = 0, q[n++] = 1;                      // one question
	q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0;  // no answers, authority, additional
	q[n++] = 0, q[n++] = 0;
	const char *p = host;
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		if (len == 0 || len > 63 || n + len + 6 > sizeof q) return -1;
		q[n++] = (unsigned char)len;
		memcpy(q + n, p, len);
		n += len;
		p += len;
		if (*p == '.') p++;
	}
	q[n++] = 0;
	q[n++] = 0, q[n++] = 1;  // type A
	q[n++] = 0, q[n++] = 1;  // class IN
	int got = snc_net_udp_exchange(server, 53, q, n, r, sizeof r, 2500);
	if (got < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 0x0f) != 0) return -1;
	int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
	size_t i = 12;
	for (int k = 0; k < qd; k++) {  // skip the questions
		while (i < (size_t)got && r[i] && (r[i] & 0xc0) != 0xc0) i += r[i] + 1;
		i += (i < (size_t)got && (r[i] & 0xc0) == 0xc0) ? 2 : 1;
		i += 4;
	}
	for (int k = 0; k < an && i + 10 <= (size_t)got; k++) {
		if ((r[i] & 0xc0) == 0xc0) i += 2;
		else {
			while (i < (size_t)got && r[i]) i += r[i] + 1;
			i++;
		}
		if (i + 10 > (size_t)got) break;
		int type = (r[i] << 8) | r[i + 1];
		int len = (r[i + 8] << 8) | r[i + 9];
		i += 10;
		if (type == 1 && len == 4 && i + 4 <= (size_t)got) {
			memcpy(ip, r + i, 4);
			return 0;
		}
		i += len;  // a CNAME: the A record follows it
	}
	return -1;
}

static int resolve(snc_http *h) {
	if (h->ip) return 0;
	uint32_t ip = 0;
	if (snc_net_resolve(h->host, &ip) == 0 && ip) {
		h->ip = ip;
		return 0;
	}
	for (size_t k = 0; k < sizeof FALLBACK_DNS / sizeof FALLBACK_DNS[0]; k++) {
		uint32_t server;
		memcpy(&server, FALLBACK_DNS[k], 4);
		if (dns_query(server, h->host, &ip) == 0 && ip) {
			h->ip = ip;
			return 0;
		}
	}
	snprintf(h->err, sizeof h->err, "Could not look up %s.", h->host);
	return SNC_E_DNS;
}

/* ---------------------------------- the socket --------------------------------- */

// mbedTLS's own socket layer may be compiled out; its error codes are still the
// ones the TLS layer understands.
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif
#ifndef MBEDTLS_ERR_NET_CONN_RESET
#define MBEDTLS_ERR_NET_CONN_RESET -0x0050
#endif

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
	snc_http *h = ctx;
	int n = snc_net_send(h->fd, buf, len);
	return n > 0 ? n : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) {
	snc_http *h = ctx;
	int n = snc_net_recv(h->fd, buf, len, timeout ? (int)timeout : SLICE_MS);
	if (n > 0) return n;
	if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
	if (n == SNC_NET_TIMEOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

// The random generator's seed: mbedTLS's own sources where the platform has them
// (they fail on a PSP, which has no /dev/urandom), mixed with the console's.
static int seed(void *ctx, unsigned char *out, size_t len) {
	snc_http *h = ctx;
	unsigned char mine[64];
	size_t done = 0;
	while (done < len) {
		size_t n = len - done < sizeof mine ? len - done : sizeof mine;
		if (mbedtls_entropy_func(&h->entropy, out + done, n) != 0) memset(out + done, 0, n);
		snc_entropy(mine, n);
		for (size_t i = 0; i < n; i++) out[done + i] ^= mine[i];
		done += n;
	}
	return 0;
}

// A clock that was never set (a console fresh out of the box) would fail every
// certificate on its dates; then the dates are skipped. The chain and the host
// name are always checked.
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
	(void)ctx, (void)crt, (void)depth;
	if (snc_clock_year() < 2026) *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
	return 0;
}

static void drop(snc_http *h) {
	if (h->fd >= 0) snc_net_close(h->fd);
	h->fd = -1;
	h->connected = 0;
}

int snc_http_init(snc_http *h, const char *host, const char *user_agent, const char *sdk) {
	memset(h, 0, sizeof *h);
	h->fd = -1;
	snprintf(h->host, sizeof h->host, "%s", host);
	snprintf(h->user_agent, sizeof h->user_agent, "%s", user_agent);
	snprintf(h->sdk, sizeof h->sdk, "%s", sdk);
	mbedtls_entropy_init(&h->entropy);
	mbedtls_ctr_drbg_init(&h->drbg);
	mbedtls_ssl_config_init(&h->conf);
	mbedtls_ssl_init(&h->ssl);
	mbedtls_x509_crt_init(&h->ca);
	h->setup = 1;
#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_PSA_CRYPTO_C)
	if (psa_crypto_init() != PSA_SUCCESS) {
		snprintf(h->err, sizeof h->err, "Crypto init failed.");
		return SNC_E_TLS;
	}
#endif
	static const char pers[] = "smashandclash";
	int r = mbedtls_ctr_drbg_seed(&h->drbg, seed, h, (const unsigned char *)pers, sizeof pers - 1);
	if (r == 0) r = mbedtls_x509_crt_parse(&h->ca, (const unsigned char *)SNC_ROOT_CERTS, sizeof SNC_ROOT_CERTS);
	if (r == 0) r = mbedtls_ssl_config_defaults(&h->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (r != 0) {
		snprintf(h->err, sizeof h->err, "TLS setup failed (-0x%04x).", (unsigned)-r);
		return SNC_E_TLS;
	}
	mbedtls_ssl_conf_authmode(&h->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&h->conf, &h->ca, NULL);
	mbedtls_ssl_conf_rng(&h->conf, mbedtls_ctr_drbg_random, &h->drbg);
	mbedtls_ssl_conf_verify(&h->conf, verify_cb, NULL);
	mbedtls_ssl_conf_read_timeout(&h->conf, SLICE_MS);
	r = mbedtls_ssl_setup(&h->ssl, &h->conf);
	if (r == 0) r = mbedtls_ssl_set_hostname(&h->ssl, h->host);
	if (r != 0) {
		snprintf(h->err, sizeof h->err, "TLS setup failed (-0x%04x).", (unsigned)-r);
		return SNC_E_TLS;
	}
	return 0;
}

static int tls_connect(snc_http *h, uint32_t deadline) {
	int r = resolve(h);
	if (r) return r;
	int left = (int)(deadline - snc_now_ms());
	h->fd = snc_net_tcp_connect(h->ip, 443, left > 1000 ? left : 1000);
	if (h->fd < 0) {
		h->fd = -1;
		h->ip = 0;  // look it up again next time (the address may have moved)
		snprintf(h->err, sizeof h->err, "Could not connect to %s.", h->host);
		return SNC_E_CONNECT;
	}
	mbedtls_ssl_session_reset(&h->ssl);
	mbedtls_ssl_set_bio(&h->ssl, h, bio_send, NULL, bio_recv_timeout);
	for (;;) {
		r = mbedtls_ssl_handshake(&h->ssl);
		if (r == 0) break;
		if (h->cancel) {
			drop(h);
			return SNC_E_CANCEL;
		}
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT) {
			if ((int32_t)(snc_now_ms() - deadline) > 0) {
				drop(h);
				snprintf(h->err, sizeof h->err, "%s took too long to answer.", h->host);
				return SNC_E_TIMEOUT;
			}
			continue;
		}
		uint32_t flags = mbedtls_ssl_get_verify_result(&h->ssl);
		if (flags && flags != (uint32_t)-1) {
			if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
				snprintf(h->err, sizeof h->err, "The secure connection failed: check your system clock.");
			else
				snprintf(h->err, sizeof h->err, "The server's certificate did not check out.");
		} else {
			snprintf(h->err, sizeof h->err, "The secure connection failed (-0x%04x).", (unsigned)-r);
		}
		drop(h);
		return SNC_E_TLS;
	}
	h->connected = 1;
	return 0;
}

/* --------------------------------- reading ------------------------------------ */

typedef struct {
	snc_http *h;
	uint32_t deadline;
	unsigned char buf[1024];
	int pos, len;
	int any;  // received at least one byte of the answer
} reader;

static int tls_read(reader *rd) {
	snc_http *h = rd->h;
	for (;;) {
		int r = mbedtls_ssl_read(&h->ssl, rd->buf, sizeof rd->buf);
		if (r > 0) {
			rd->pos = 0;
			rd->len = r;
			rd->any = 1;
			return r;
		}
		if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_NET_CONN_RESET) return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
		if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT) {
			if (h->cancel) return SNC_E_CANCEL;
			if ((int32_t)(snc_now_ms() - rd->deadline) > 0) return SNC_E_TIMEOUT;
			continue;
		}
		return SNC_E_NET;
	}
}

static int rd_byte(reader *rd) {
	if (rd->pos >= rd->len) {
		int r = tls_read(rd);
		if (r <= 0) return r == 0 ? -100 : r;  // -100: closed
	}
	return rd->buf[rd->pos++];
}

// A header line, without its CRLF. Returns its length, or a negative error.
static int rd_line(reader *rd, char *line, int cap) {
	int n = 0;
	for (;;) {
		int c = rd_byte(rd);
		if (c < 0) return c;
		if (c == '\n') break;
		if (c != '\r' && n < cap - 1) line[n++] = (char)c;
	}
	line[n] = 0;
	return n;
}

static int rd_bytes(reader *rd, char *dst, int n) {
	int got = 0;
	while (got < n) {
		if (rd->pos >= rd->len) {
			int r = tls_read(rd);
			if (r <= 0) return r == 0 ? -100 : r;
		}
		int take = rd->len - rd->pos;
		if (take > n - got) take = n - got;
		if (dst) memcpy(dst + got, rd->buf + rd->pos, (size_t)take);
		rd->pos += take;
		got += take;
	}
	return got;
}

static int ieq_prefix(const char *s, const char *prefix) {
	for (; *prefix; s++, prefix++) {
		char a = *s, b = *prefix;
		if (a >= 'A' && a <= 'Z') a += 32;
		if (a != b) return 0;
	}
	return 1;
}

/* --------------------------------- a request ---------------------------------- */

static int send_all(snc_http *h, const unsigned char *p, size_t n) {
	while (n) {
		int r = mbedtls_ssl_write(&h->ssl, p, n);
		if (r > 0) {
			p += r;
			n -= (size_t)r;
			continue;
		}
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		return SNC_E_NET;
	}
	return 0;
}

static int once(snc_http *h, const char *method, const char *path, const char *token, const char *body,
                char *out, size_t cap, uint32_t deadline, int *status) {
	char head[768];
	size_t blen = body ? strlen(body) : 0;
	int n = snprintf(head, sizeof head,
	                 "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: application/json\r\nX-SDK: %s\r\nConnection: keep-alive\r\n",
	                 method, path, h->host, h->user_agent, h->sdk);
	if (token && *token) n += snprintf(head + n, sizeof head - (size_t)n, "Authorization: Bearer %s\r\n", token);
	if (body) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)blen);
	else if (!strcmp(method, "POST")) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: 0\r\n");
	n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
	if (n >= (int)sizeof head) return SNC_E_PROTO;

	if (send_all(h, (const unsigned char *)head, (size_t)n) || (blen && send_all(h, (const unsigned char *)body, blen))) return SNC_E_NET;

	reader *rd = malloc(sizeof *rd);
	if (!rd) return SNC_E_NET;
	memset(rd, 0, sizeof *rd);
	rd->h = h;
	rd->deadline = deadline;

	char line[512];
	int r = rd_line(rd, line, sizeof line);
	if (r < 0) goto fail;
	if (strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) {
		r = SNC_E_PROTO;
		goto fail;
	}
	*status = atoi(line + 9);
	long length = -1;
	int chunked = 0, close_after = 0;
	for (;;) {
		r = rd_line(rd, line, sizeof line);
		if (r < 0) goto fail;
		if (r == 0) break;
		if (ieq_prefix(line, "content-length:")) length = strtol(line + 15, NULL, 10);
		else if (ieq_prefix(line, "transfer-encoding:") && strstr(line, "chunked")) chunked = 1;
		else if (ieq_prefix(line, "connection:") && (strstr(line, "close") || strstr(line, "Close"))) close_after = 1;
	}
	size_t got = 0;
	if (chunked) {
		for (;;) {
			r = rd_line(rd, line, sizeof line);
			if (r < 0) goto fail;
			long size = strtol(line, NULL, 16);
			if (size <= 0) {
				while ((r = rd_line(rd, line, sizeof line)) > 0) {}  // trailers
				if (r < 0) goto fail;
				break;
			}
			if (got + (size_t)size >= cap) {
				r = SNC_E_TOOBIG;
				goto fail;
			}
			r = rd_bytes(rd, out + got, (int)size);
			if (r < 0) goto fail;
			got += (size_t)size;
			r = rd_line(rd, line, sizeof line);  // the CRLF after the chunk
			if (r < 0) goto fail;
		}
	} else if (length >= 0) {
		if ((size_t)length >= cap) {
			r = SNC_E_TOOBIG;
			goto fail;
		}
		r = rd_bytes(rd, out, (int)length);
		if (r < 0) goto fail;
		got = (size_t)length;
	} else {
		// no length: the body runs until the server closes
		for (;;) {
			int c = rd_byte(rd);
			if (c == -100) break;
			if (c < 0) {
				r = c;
				goto fail;
			}
			if (got + 1 >= cap) {
				r = SNC_E_TOOBIG;
				goto fail;
			}
			out[got++] = (char)c;
		}
		close_after = 1;
	}
	out[got] = 0;
	free(rd);
	if (close_after) drop(h);
	return (int)got;

fail:
	if (r == -100) r = rd->any ? SNC_E_NET : -101;  // -101: closed before answering
	free(rd);
	drop(h);
	return r;
}

int snc_http_request(snc_http *h, const char *method, const char *path, const char *token, const char *body,
                     char *out, size_t cap, int timeout_ms, int *status) {
	h->cancel = 0;
	h->err[0] = 0;
	*status = 0;
	if (cap) out[0] = 0;
	uint32_t deadline = snc_now_ms() + (uint32_t)timeout_ms;
	for (int attempt = 0; attempt < 2; attempt++) {
		h->reused = h->connected;
		if (!h->connected) {
			int r = tls_connect(h, deadline);
			if (r) return r;
		}
		int r = once(h, method, path, token, body, out, cap, deadline, status);
		if (r >= 0) return r;
		if (h->cancel) return SNC_E_CANCEL;
		// A kept-alive connection the server had already closed: the request never
		// arrived, so send it once more on a fresh one.
		if ((r == -101 || r == SNC_E_NET) && h->reused && attempt == 0) continue;
		if (r == -101) r = SNC_E_NET;
		if (!h->err[0]) {
			if (r == SNC_E_TIMEOUT) snprintf(h->err, sizeof h->err, "%s took too long to answer.", h->host);
			else if (r == SNC_E_TOOBIG) snprintf(h->err, sizeof h->err, "The answer was too big.");
			else if (r == SNC_E_PROTO) snprintf(h->err, sizeof h->err, "The answer made no sense.");
			else if (r != SNC_E_CANCEL) snprintf(h->err, sizeof h->err, "The connection to %s broke.", h->host);
		}
		return r;
	}
	return SNC_E_NET;
}

void snc_http_cancel(snc_http *h) {
	h->cancel = 1;
}

void snc_http_close(snc_http *h) {
	if (!h->setup) return;
	if (h->connected) mbedtls_ssl_close_notify(&h->ssl);
	drop(h);
	mbedtls_ssl_free(&h->ssl);
	mbedtls_ssl_config_free(&h->conf);
	mbedtls_x509_crt_free(&h->ca);
	mbedtls_ctr_drbg_free(&h->drbg);
	mbedtls_entropy_free(&h->entropy);
	h->setup = 0;
}
