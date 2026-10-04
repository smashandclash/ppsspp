// Transport errors shared by every way the core reaches the game (HTTPS, or the SDK
// bridge on the GBA). An API error is not one of these: it is its HTTP status (> 0).
#ifndef SNC_ERRORS_H
#define SNC_ERRORS_H

#define SNC_E_NET      (-1)  // no network, or the connection broke
#define SNC_E_DNS      (-3)  // the host name did not resolve
#define SNC_E_CONNECT  (-4)  // no TCP connection
#define SNC_E_TLS      (-5)  // the TLS handshake or certificate failed
#define SNC_E_TIMEOUT  (-6)  // no answer in time
#define SNC_E_CANCEL   (-7)  // cancelled
#define SNC_E_TOOBIG   (-8)  // the answer does not fit the buffer
#define SNC_E_PROTO    (-9)  // not a valid answer
#define SNC_E_BRIDGE   (-10) // the SDK bridge is not running (GBA)

#endif
