# How it works

Smash&Clash on the DS, the GBA and the PSP is one C client with three faces. The game itself (rules, the house opponent, ratings, matchmaking, replays) runs on smashandclash.in; the consoles are clients of the same API that [`@smashandclash/sdk`](https://www.npmjs.com/package/@smashandclash/sdk) wraps, and use it the same way.

```
 console main.c ──▶ snc_client ──▶ snc_api ──▶ snc_http (mbedTLS) ──────────────────────────▶ smashandclash.in
 (input, frames)    (state, jobs)  (SDK calls) └▶ snc_bridge (GBA): RAM mailbox ▶ mGBA script ▶ bridge.mjs (@smashandclash/sdk)
       │
       └──▶ view (each console's screens) ── snc_ui (targets, D-pad) ── snc_gfx / snc_draw (DS, GBA) or the GPU (PSP)
```

## The parts

| File | What it does |
| --- | --- |
| `core/snc_http.c` | An HTTPS/1.1 client on mbedTLS (2.28 on the PSP, 3.6 on the DS): one keep-alive connection, chunked bodies, one retry when a kept connection has gone stale, cancellation, a DNS fallback over UDP, Let's Encrypt roots built in. |
| `core/snc_api.c` | **The SDK's calls in C.** Each function is one SDK method (`snc_start_house` is `sc.games.startHouse`, `snc_play` is `game.play`, …) and reads the API's JSON with [jsmn](https://github.com/zserge/jsmn) into an `snc_game`. Built with `SNC_BRIDGE`, it hands each call to the SDK bridge instead of HTTPS. |
| `core/snc_game.c` | Rules of thumb shared by every console: reading move names, which tiles a picked card can reach, the house opponent's persona, the rating estimate. |
| `core/snc_client.c` | The client minus the drawing: screens, the game in progress, what is picked, notices, the player's record, and the network job. |
| `core/snc_ui.c` | The touch and focus targets a draw leaves behind, D-pad navigation between them, and what pressing each one does. |
| `core/snc_gfx.c`, `snc_draw.c` | A 16-bit software renderer (fills, blends, images, alpha masks, UTF-8 text, QR codes) and the Arena Pop pieces. The DS and GBA draw with it; the PSP draws with its GPU. |
| `<console>/source/net_*.c` | The platform: sockets, the resolver, a clock, entropy for TLS. |
| `<console>/source/view*.c` | That console's screens. |

## One job at a time

The network is slow and the screen must never freeze, so the UI and the network meet in one place: `snc_client.job`.

- The UI thread calls `snc_client_tick()` every frame and the `snc_act_*()` actions (pick a card, start a game, resign). An action that needs the server **queues a job** (`state = 1`) and returns at once.
- The network side loops on `snc_client_net_step()`, which runs the queued job (`state = 2`) and leaves the result (`state = 3`). The next tick applies it to the game.
- Jobs that arrive while one runs wait in a short queue. A long poll for the other player's move (`game.waitForTurn`, up to 20 seconds) is **cancelled** the moment you act, so a resign never waits behind it.
- When it is not your turn, the client keeps exactly one long poll out, and backs off after failures.

The DS runs the network on a cooperative thread (BlocksDS cothreads: a blocking socket yields to the UI); the PSP on a lower-priority kernel thread. The GBA has no threads: its main loop runs the job itself, and the bridge keeps calling the frame function while it waits, so the screen and the buttons stay alive.

## Moves

Moves are strings, named the way the SDK names them, and the client only ever sends names from `legalMoves`:

| Move | Means |
| --- | --- |
| `Pengu@C2` | place Pengu on C2 |
| `Teddy!C2` | overrun C2 with Teddy (Mutators) |
| `hop→E3`, `hop: stay` | resolve a hop on a chess tile |
| `BOULDER(D2)`, `FREEZE(C1)`, `RECRUIT(D3→B1)`, `FLIP`, `SWAP` | play an effect |

`snc_targets()` turns the picked card and the tiles chosen so far into the set of tiles that continue a legal move (the green tiles), and `snc_complete()` spots the move the picks spell out exactly. So the board is never told the rules: it reads them back from `legalMoves`.

## Drawing either seat

The API gives the board in **board frame** (A1 is seat A's corner) with each card's `sides` as they lie on the board, and each tile's owner from your point of view. You always sit at the bottom: seat B's view turns the board round (`view_cell_slot`) and reads the sides from the other end (`view_tile_vals`). The other player's cards face them, so their art is drawn upside down, the way the web edition does it.

Two cards share a name (there are two Lizzies). The client tells them apart by their printed sides, turning a board card's board-frame sides back to the printed ones from its owner's seat (`sd_tile_card`).

## The house opponent

New game plays the Smash&Clash house opponent at your level: `strength` is your rating, kept between 800 and 1600. The opponent wears a player's handle derived from the game id (`snc_persona`, the same names the web lobby shows), and the client keeps your rating with the standard Elo update after each game at your level.

## The GBA's SDK bridge

The Game Boy Advance has no network hardware, so its calls travel through the emulator:

1. `gba/source/bridge.c` writes the call into a **mailbox** in the cartridge's RAM: the SDK method (`"op":"game.play"`), the API request it stands for, your seat's token and the body.
2. `gba/bridge/smashandclash.lua`, running in mGBA, finds the mailbox by its magic, beats its heartbeat every frame, and sends each new request to the bridge over a local socket.
3. `gba/bridge/bridge.mjs` makes the call with `@smashandclash/sdk` (it keeps the SDK's `Game` objects, so `game.play` is the SDK's own) and answers with what the SDK returned.
4. The script writes the answer straight into the buffer the request named, then its length and status, then the sequence number. The cartridge, which has been running frames all along, sees the number change and parses the answer with the same C code the DS uses.

The mailbox (little-endian words after a 16-byte magic, `SNC-GBA-BRIDGE1`):

| Offset | Field | Written by |
| --- | --- | --- |
| 16 | `req_seq`: the request's number, written last | the game |
| 20 | `req_len` | the game |
| 24 | `resp_seq`: the answered request's number, written last | the script |
| 28 | `resp_len` (more than `resp_cap`: too big, nothing written) | the script |
| 32 | `resp_status`: 200, or the API's error status | the script |
| 36 | `heartbeat`: a frame counter | the script |
| 40 | `connected`: the bridge is on the line | the script |
| 44 | `resp_cap` | the game |
| 48 | `resp_addr`: where to write the answer | the game |
| 64 | the request JSON (1024 bytes) | the game |

On the wire, both ways: a header line, then exactly that many bytes of JSON (`REQ <seq> <len>\n…` and `RES <seq> <status> <len>\n…`). A cancelled call simply gets a new number: an answer for an old one is dropped.

## Speed on the GBA

A 16.78 MHz ARM7 redrawing a 240 × 160 16-bit screen in software has to be careful:

- The screen is drawn into a buffer in EWRAM and copied up by DMA at the start of the vertical blank. The copy outruns the beam, so there is no tearing.
- The renderer is compiled as ARM code and runs from IWRAM, several times faster than Thumb code from the cartridge.
- The selection brackets and the thinking dots are **hardware sprites**: moving around the board never redraws the screen.
- The buttons are read in the vertical-blank interrupt, so a press during a redraw is never lost.

## Gotchas we hit

- **Delta's WFC DNS servers** (AltWFC in particular) do not resolve smashandclash.in, so the DS falls back to public resolvers over UDP.
- **Clocks.** A DS or PSP clock can be years off; certificate dates are only checked once the clock reads 2026 or later.
- **The PSP has no `/dev/urandom`**: TLS's random generator is seeded from the platform's own entropy.
- **Two Lizzies**: identify cards by name *and* sides.
- **mGBA's Lua socket** can say "readable" on a socket the other side closed; the script counts silent reads and reconnects.
