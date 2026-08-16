# HTTP Bridge — integrating a game with the TS plugin

The plugin's HTTP bridge lets **any game whose mods can make HTTP requests**
drive the full RoNTR audio pipeline — proximity falloff, stereo pan, radio
channels with DSP effect, occlusion muffling, live-tunable settings — without
sockets, shared memory, or native code on the game side. Arma Reforger
(sandboxed EnforceScript + RestApi) is the first integration; this document
is the contract for adding others.

Transport summary: the game POSTs its state to the plugin ~10x/second; each
response carries back who is currently audible in TeamSpeak (for talk
indicators / mouth animation). That's the whole protocol — one endpoint each
way, plain text, no dependencies.

> **The bridge is a compile-time plugin option, OFF by default.** Integrations
> need a plugin built with it enabled: `scripts\build.ps1 -TsPluginOnly
> -HttpBridge` (Windows) or `RTR_HTTP_BRIDGE=1 ./scripts/build-linux-plugin.sh`
> (Linux). A bridge-free plugin logs `http bridge disabled (build option)` at
> load and does not listen on the port at all.

```
game mod --- POST /state (10 Hz) --->  127.0.0.1:39442 (TS plugin)
         <-- 200 + talk list --------
```

## Endpoints

| Endpoint | Purpose |
| --- | --- |
| `GET /health` (or `/`) | Presence/version check. Returns `rtr=3`, `plugin=<version>`, and `game=<id>` of the currently connected game if any. |
| `POST /state` | Push one game-state snapshot; response is the reverse channel. |

Status codes: `200` accepted; `409` protocol version mismatch (body carries
the expected `rtr=` version — tell the user to update mod/plugin together);
`400` malformed payload; `404` unknown path. Rejected payloads are logged
(throttled) to the TS client log with a body preview — the first thing to
check when integrating.

The listener binds loopback only, accepts `Content-Length` and chunked
bodies, tolerates form-encoded (`data=`+percent-encoding) and literal-`\n`
payloads, and closes the connection after each response.

## POST /state payload

UTF-8 `key=value` lines, `\n`-separated. Unknown keys are ignored (add
game-specific extras freely); missing keys default to zero, which always
fails safe. Order doesn't matter.

| Key | Value | Notes |
| --- | --- | --- |
| `rtr` | `3` | Protocol version. Required — its absence or mismatch is a 409. |
| `game` | short id, e.g. `arma-reforger` | Optional but recommended; logged by the plugin on connect. |
| `ingame` | `0`/`1` | `0` (menus, loading, dead link) = plugin passes TS audio through untouched. |
| `name` | local player's in-game name | Must match the `player=` entry for this client; used to map TS clients to game players (with the TS nickname as fallback). |
| `lpos` | `x y z` floats | Listener (camera) position, **meters**. |
| `lfwd` | `x y z` | Camera forward, unit length. |
| `lup` | `x y z` | Camera up, unit length. |
| `radioptt` | `0`/`1` | Radio transmit key held. Drives TX_START/TX_STOP announcements to other TS clients. |
| `voiceptt` | `0`/`1` | Reserved (proximity voice is always-on in current integrations). |
| `freq` | kHz integer | Shorthand: fills radio slot 0. |
| `freqs` | `kHz,kHz,...` | Up to 4 tuned radio slots. `0` = slot unused. |
| `activeradio` | `0..3` | Which slot transmits on PTT. Default 0. |
| `player` | `Name\|x\|y\|z\|alive\|occl` | One line per player, **including the local player**. |

`player` fields: position in meters (same space as `lpos`); `alive` `0`/`1`
(dead players: reserved for future gating, send `1` if unknown); `occl`
`0-255` occlusion (0 clear, ~85 per wall, saturating — send `0` until the
game implements line-of-sight checks; the muffling DSP consumes it
automatically once real values arrive). Sanitize names: strip `|`, `\n`,
`\r` — they are the only reserved characters.

**Coordinates:** any handedness/up-axis works — the spatial math uses only
relative vectors (`pos`, `fwd`, `up` cross products). The one rule is that
all positions and vectors in one payload share the same space and use meters.
UE games divide cm by 100; Enfusion sends native meters.

**Rate & staleness:** 5–20 Hz recommended (10 Hz typical). The plugin treats
the link as dead after 1 s without a valid POST and reverts to plain TS
audio, so a crashed game degrades gracefully. Don't batch: each POST should
be the current snapshot.

**Transport arbitration:** the native transports (shared memory / UDP, used
by RoN) own the game link while they deliver fresh data; HTTP state is
ignored during that time (the POST still gets a 200 + talk response, and the
TS log notes it, throttled). HTTP takes over automatically once the native
link has been stale for >1 s. Practically: a game left idling in the
background can never stomp the game actually being played.

**Frequencies match by exact integer equality** — transmitter and listeners
hear each other only when tuned to the same kHz value. Integrations should
step frequencies on a fixed grid so equality is reachable.

## Response body (200)

```
rtr=3
talk=Name|0.83
talk=OtherName|0.10
```

One `talk=` line per player currently audible in TeamSpeak, amplitude
smoothed 0..1. Empty list = nobody talking. Use for talk indicators or mouth
animation. Names are the same ids as the `player=`/`name` fields.

## Integration checklist for a new game

1. `GET /health` on startup; warn the player if absent or `rtr=` differs.
2. Post `ingame=0` whenever there is no possessed character; resume real
   state on possession. Never stop posting while the game runs.
3. Send `game=<your-id>` so multi-game debugging stays sane.
4. Map keys game-side (PTT, frequency stepping) — the plugin only consumes
   the resulting state.
5. Mute/suppress the game's native voice system; TeamSpeak is the only
   voice path.
6. Verify with the plugin's `debug.log` setting (`/rtr set debug.log 1` in
   TS chat): it dumps parsed positions/distances every ~2 s to compare
   against the game's own logs.
7. Occlusion, when ready: fill per-player `occl` from line-of-sight checks
   (see `docs/OCCLUSION.md` for the RoN reference implementation and the
   85-per-wall convention).

Smoke test without a game:

```
curl -X POST --data-binary $'rtr=3\ningame=1\nname=Me\nlpos=0 0 0\nlfwd=1 0 0\nlup=0 0 1\nplayer=Me|0|0|0|1|0' http://127.0.0.1:39442/state
curl http://127.0.0.1:39442/health
```

## Versioning

The `rtr` number tracks `RTR_VERSION` in `shared/RadioLink.h` and bumps on
any breaking change to this contract (field semantics, struct layout for the
shm/UDP transports). Additive keys do NOT bump it — parsers ignore unknown
keys by design. On mismatch the plugin rejects with 409 rather than
misinterpreting: game mod and plugin ship as a matched pair.

Implementation: `ts3-plugin/src/httpbridge.hpp` (listener + pure parse/build
functions, host-tested in `tests/test_core.cpp`).
