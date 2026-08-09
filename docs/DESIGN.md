# RoN Tactical Radio — Technical Design

TFAR-style proximity voice + simulated radio for **Ready or Not**, delivered as a
TeamSpeak 3 client plugin fed by a game-side UE4SS mod.

## 1. Goals

- **Proximity voice:** teammates are heard positionally (direction + distance
  attenuation) based on in-game positions. Out of range = silent.
- **Radio voice:** push-to-talk radio on shared frequencies with radio DSP
  (bandpass, distortion, noise, squelch clicks), heard regardless of distance.
- **TFAR behavior:** a nearby transmitting player is heard both directly
  (proximity) and over the radio.

## 2. Constraints and feasibility (verified Aug 2026)

| Fact | Consequence |
|---|---|
| RoN runs on UE5 (5.3.2) since mid-2024 | Use **UE4SS** (supports UE4.12–5.7); official SDK has no runtime scripting for this |
| No active anti-cheat in co-op; UE4SS mods (e.g. 16-player co-op) are common | DLL injection / UE4SS is viable and accepted in the community |
| TS3 Client Plugin SDK still published (API 26) | Same platform TFAR uses; native C plugin exports |
| TFAR uses **shared memory** game↔TS IPC | Proven pattern, copy it |
| Co-op pawns replicate to every client | Each client can read all player positions locally — no server mod needed |

## 3. Architecture

```
┌───────────────────────────┐        ┌──────────────────────────────┐
│ Ready or Not (UE5)        │        │ TeamSpeak 3 Client           │
│ ┌───────────────────────┐ │        │ ┌──────────────────────────┐ │
│ │ UE4SS C++ mod         │ │ shared │ │ RoNTacticalRadio plugin  │ │
│ │ - poll pawns @20Hz    │ ├─memory─► │ - poll shm @20Hz         │ │
│ │ - camera pos/rot      │ │ seqlock│ │ - nickname↔player match  │ │
│ │ - PTT keys, radio UI  │ │        │ │ - PTT → TS transmit      │ │
│ └───────────────────────┘ │        │ │ - playback DSP:          │ │
└───────────────────────────┘        │ │   proximity pan/gain OR  │ │
                                     │ │   radio bandpass+noise   │ │
                                     │ └────────────┬─────────────┘ │
                                     └──────────────┼───────────────┘
                                                    │ TS plugin commands
                                                    ▼ (radio state sync)
                                            other clients' plugins
```

Three components:

1. **Game mod** (`game-mod/`) — UE4SS C++ mod loaded into RoN. Each tick it
   reads local camera transform, every player pawn's position and name, and
   PTT/channel key state, then writes one struct to named shared memory.
2. **Shared memory bridge** (`shared/RadioLink.h`) — one packed struct,
   `Local\RoNTacticalRadio`, seqlock-versioned (writer increments `sequence`
   to odd before writing, even after; reader retries on odd/changed).
   Positions converted from UE cm to meters.
3. **TS3 plugin** (`ts3-plugin/`) — native DLL in the TeamSpeak client.
   Reads shared memory on a 50 ms poll thread and processes every incoming
   voice stream in `ts3plugin_onEditPlaybackVoiceDataEvent`.

## 4. Why custom DSP instead of TS3's built-in 3D audio

TS3 has `systemset3DListenerAttributes`/`channelset3DAttributes`, but TFAR
doesn't use them — and neither do we. Doing gain/pan ourselves in the playback
callback gives one code path for proximity + radio mixing, per-stream control
(a client can be simultaneously "direct" and "on radio"), and custom falloff.

**Proximity model** (per 20 ms frame, smoothed to avoid zipper noise):
- `gain = clamp(1 - dist/maxDist, 0, 1)^rolloff` (default `maxDist` 40 m,
  `rolloff` 1.5; whisper/shout could scale `maxDist` later)
- Stereo pan from `dot(normalize(toSpeaker), listenerRight)`
- `dist > maxDist` → zero the buffer
- Speaker not matched to a game player, or listener not in-game → passthrough
  (TS behaves normally in lobby/menus)

**Radio model:**
- Biquad bandpass ~300–3400 Hz → soft-clip drive → mix constant white noise
  (fixed level; signal-quality-based later) → squelch click at TX start/end
- Received at full volume when transmitter's frequency matches any of the
  listener's programmed frequencies

## 5. Radio state sync (between TS plugins)

No game server component, so radio state propagates via TS
`sendPluginCommand` (JSON-ish key=value payload):

| Command | When | Payload |
|---|---|---|
| `HELLO` | plugin connects / enters game | `name=<in-game name>` (binds TS clientID ↔ game player) |
| `FREQ` | channel/freq changed in-game | `freqs=246000,247000,...` (kHz) |
| `TX_START` | PTT down | `freq=<kHz>` |
| `TX_STOP` | PTT up | — |

Receivers keep a `clientID → {name, freqs, txFreq}` map. Matching falls back
to TS nickname == in-game name if no `HELLO` seen.

**PTT:** game mod reports key state in shared memory; the plugin drives TS
transmission itself via `CLIENT_INPUT_DEACTIVATED` (TFAR's approach), so users
set TS to continuous transmission and the plugin gates the mic:
mic open when *radio PTT* or *proximity voice-activation key* is down.

## 6. Game-side details

- **UE4SS C++ mod** (not Lua): Lua has no shared-memory/FFI path; C++ mod gets
  native access and ships as one DLL in `ue4ss/Mods/`.
- Find `PlayerCameraManager` for listener transform; `FindAllOf(L"...PlayerState")`
  for player names + pawn positions. Exact class/property names must be
  confirmed against a UE4SS SDK dump of the current RoN build (`UHT` dump) —
  they shift between patches, so they live in one mapping file.
- Keybinds read via `GetAsyncKeyState` (configurable ini): PTT, cycle channel.
- In-game radio UI (freq display) is a later milestone — start with ini-configured
  frequencies + on-screen debug text via UE4SS.

## 7. Risks

- **Game patches** rename/move properties → keep all class/property names in
  one config, expect maintenance per patch (TFAR has the same burden).
- **Name matching** breaks if TS nickname ≠ Steam name → `HELLO` handshake is
  primary, nickname only fallback.
- **TS3 long-term:** TeamSpeak 6 client plugin story is unproven; TS3 client
  still works and is what TFAR communities run. Revisit if TS6 gains a plugin API.
- **Everyone must install both parts** (like TFAR). Unmodded players are heard
  at normal TS volume (passthrough) — acceptable degradation.

## 8. Milestones

1. **M1 – Bridge:** game mod writes positions; TS plugin logs them. (Console
   `print` on both sides validates the whole pipeline.)
2. **M2 – Proximity:** distance gain + pan + mute; smoothing.
3. **M3 – Radio:** PTT, plugin-command sync, radio DSP, mic gating.
4. **M4 – Polish:** squelch tails, stereo radio ear, channel UI, occlusion
   (line-trace based muffle), installer (.ts3_plugin package + pak/UE4SS zip).
