# RoNTR state viewer (dev tool)

A containerized Node dashboard that **impersonates the TS plugin's HTTP
bridge** (`docs/HTTP-BRIDGE.md` contract, port `39442`). Because the game mod
just POSTs its state over the REST bridge, no game-side changes are needed:
start this instead of TeamSpeak and the same 10 Hz state stream renders live
in a browser.

Shows: link status + game id + post rate, in-game flag, listener
position/orientation, every player with distance and an occlusion bar, a
top-down listener-centered map (occlusion color-codes the dots, dead players
gray), radio slots (PRI/SEC frequency, ear routing, active TX slot, PTT
state), any unknown/extra keys in the payload, and the raw wire text. A
**talk injector** lets you send fake `talk=Name|amp` reverse-channel entries
back to the game to test talk indicators / mouth animation.

## Run

```sh
cd tools/state-viewer
docker compose up --build
# dashboard: http://127.0.0.1:39442/
```

Or without Docker (no dependencies, Node >= 16):

```sh
node tools/state-viewer/server.js
```

Then start the game/Workbench as usual — the mod's `POST /state` lands in
the viewer. The dashboard, a JSON API (`GET /api/state`), and the bridge
endpoints (`POST /state`, `GET /health`) all share the one port.

**Port conflict:** the real plugin's bridge uses the same port by design.
Either quit TeamSpeak (or `/rtr set bridge.enable 0`) before starting the
viewer — or run both with relay mode, below.

## Relay mode — viewer + TeamSpeak at the same time

Move the plugin's bridge off 39442, then tell the viewer to forward to it:

```sh
# in TeamSpeak chat (persist with /rtr save):
/rtr set bridge.port 39500
```

Then either flip it on in the dashboard's **Relay** panel (checkbox +
target — a bare number means `127.0.0.1:<port>`; from Docker use
`host.docker.internal:39500`), or start with it already on via the env:

```sh
FORWARD=http://127.0.0.1:39500 node server.js
# or Docker: uncomment FORWARD in docker-compose.yml
```

The env only sets the initial state — on/off and the target are changeable
at runtime from the dashboard or `POST /api/relay`
(`{"enabled":true,"target":"39500"}`).

The game keeps posting to 39442 unchanged; the viewer records/visualizes
each snapshot, forwards the raw body to the plugin, and returns the
plugin's response — so the real talk list (who is audible in TeamSpeak)
reaches the game, and the audio pipeline works exactly as without the
viewer. Injected talkers are appended to the plugin's talk list; 409/400
rejects from the plugin pass through to the game. If the plugin is
unreachable the viewer answers alone (game link stays up) and the
dashboard's relay pill turns red with the error.

## Endpoints

| Endpoint | Purpose |
| --- | --- |
| `GET /` | Dashboard. |
| `POST /state` | Bridge contract: accepts the game's state, responds `rtr=3` + injected `talk=` lines. Mirrors the plugin's 409/400 rejects. |
| `GET /health` | Bridge contract: `rtr=3`, `plugin=state-viewer-dev`. |
| `GET /api/state` | Latest parsed state + meta (rate, rejects, raw body) as JSON. |
| `GET`/`POST /api/talk` | Read/replace the injected talk list (`[{"name":"Bob","amp":0.8}]`). |
| `GET`/`POST /api/relay` | Read/change relay config: `{"enabled":true,"target":"39500"}` (target: port, host:port, or http:// url). |

The parser mirrors `ts3-plugin/src/httpbridge.hpp` (same normalization:
form-encoded `data=` bodies, percent-escapes, literal `\n`), so a payload the
viewer accepts is a payload the plugin accepts — 409/400 behavior included.
Unknown keys aren't dropped; they're listed in the dashboard's "extra keys"
panel.
