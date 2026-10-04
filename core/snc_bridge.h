// The core's other way to the game: an SDK bridge. Built with SNC_BRIDGE (the GBA,
// which has no network), every call snc_api makes goes out as an SDK call, named the
// way @smashandclash/sdk names it ("games.startHouse", "game.play", ...), together with
// the API request it stands for; the bridge (gba/bridge/bridge.mjs, on the SDK) makes the
// call and answers with what the SDK returned, in the API's JSON shapes.
#ifndef SNC_BRIDGE_H
#define SNC_BRIDGE_H

#include <stddef.h>

// One SDK call. Returns the answer's length (>= 0) with *status set (200 on success, the
// API's status on an API error), or an SNC_E_* transport error with err filled in.
int snc_bridge_request(const char *op, const char *method, const char *path, const char *token, const char *body, char *out,
                       size_t cap, int timeout_ms, int *status, char *err, size_t errcap);

// Abort the call in flight (it returns SNC_E_CANCEL).
void snc_bridge_cancel(void);

#endif
