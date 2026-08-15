# Occlusion Muffling — Plan (M4)

Voice through walls should sound muffled and quieter; open line-of-sight
should stay clear. Doors and corners should transition smoothly.

> **Status (0.5.0):** milestones 1–3 are implemented — game-side wall
> counting (`CountWalls` in dllmain.cpp, param offsets resolved at runtime
> from the UFunction layout rather than hardcoded dump offsets), the
> `occlusion` byte in `RtrPlayer` (RTR_VERSION 3), and the plugin DSP
> (`OnePoleLP`/`applyOcclusion` in radio_dsp.hpp). Milestone 4 (tuning:
> per-wall strength, head offset, glass/doors) awaits in-game testing via
> the known-spots checklist below; wall-count changes are logged live in
> UE4SS.log and `occl=` in the TS client log.
>
> All plugin-side DSP constants are live-tunable (settings.hpp), three ways:
> the plugin's Settings button in TeamSpeak's Plugins dialog (native slider
> window, config_win.hpp, changes apply instantly), the TS chat (`/rtr show`,
> `/rtr set occl.maxatten 0.7`, `/rtr save`), or editing
> `<TS config>/ron_tactical_radio.ini` directly (hot-reloaded ~1s). Game-side
> knobs (`kOcclusionPerWall`, `kHeadOffsetCm`) stay compile-time, but
> `occl.strength` scales the shipped value, which covers most of the same
> ground while tuning.

## Architecture

Occlusion is a GEOMETRY question, so it's computed **game-side** (only the
game knows the map) and shipped per-player through the existing state pipe.
The TS plugin turns it into DSP. This follows the mod's existing split:
game mod = facts, plugin = audio.

```
game mod (20 Hz, game thread)          TS plugin (per voice frame)
  line traces camera -> each head  ->    occlusion byte per speaker  ->
  wall count 0..N                        lowpass + extra attenuation,
                                         smoothed ~150 ms
```

## Game side: tracing

Confirmed available via UE4SS reflected calls (UHT dump):

```
UKismetSystemLibrary::LineTraceSingle(WorldContextObject, Start, End,
    TraceChannel, bTraceComplex, ActorsToIgnore, DrawDebugType,
    OutHit&, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime) -> bool
```

- Static BlueprintCallable: call `ProcessEvent` on the KismetSystemLibrary
  CDO. WorldContext = our live PlayerController.
- **Channel:** Visibility (`TraceTypeQuery1`). Open doorways pass, walls
  block. Glass blocks visibility traces — acceptable at first (glass muffles
  a bit in reality); revisit with a material check later.
- **ActorsToIgnore:** both pawns (listener + speaker), so bodies don't
  occlude their own voices. TArray param pointing at a local two-element
  array.
- **Wall count, not just binary:** iterative single traces — trace, on hit
  advance the start point past `OutHit.Location` + 10 cm, re-trace; cap at
  4 hits. Distinguishes "one plasterboard wall" from "across the building".
- **FHitResult:** mirror only the fields we read (`Location`, `Distance`)
  at offsets taken from the UHT dump; allocate the param struct oversized
  (2x dump size) so a layout drift degrades to garbage values, not stack
  corruption. v1 can even ignore OutHit contents (binary blocked/clear)
  while we validate the call.
- **Cost:** ≤7 speakers x ≤4 traces at 20 Hz on the game thread — negligible
  (AI does far more traces per frame).

## Transport

Add to `RtrPlayer`: `uint8_t occlusion` (0 = clear, each wall adds ~85,
saturating). Bump `RTR_VERSION` to 3 (both halves must upgrade together —
the version gate already enforces that cleanly).

## TS plugin: DSP

Per speaker, occlusion `o` in [0,1], slewed at ~150 ms to avoid pops when
someone strafes past a corner:

- **Lowpass:** one-pole LPF per speaker, cutoff mapped
  `clear -> bypass, o=1 -> ~500 Hz` (log interpolation). One-pole is enough —
  muffling is a broad effect, and it's cheap and click-free while cutoff
  moves.
- **Attenuation:** `gain *= 1 - 0.55*o` on top of distance falloff.
- **Radio path unaffected** — radio exists to beat walls. (Later, M4's
  dual-path mixing would apply occlusion only to the direct component.)
- Diagnostics: extend the throttled `audio '<name>': dist=... pan=...` log
  with `occl=`.

## Milestones

1. **Trace spike** (game mod only): log `occl(<name>)=N walls` per player;
   verify against known spots — same room 0, closed door 1, floor between 1+,
   across the map 4 (cap).
2. **Ship it:** occlusion byte in `RtrPlayer`, version bump, plugin logs it.
3. **DSP:** lowpass + attenuation with slew; A/B test through the metering
   logs and by ear (wall vs doorway vs open).
4. **Tune:** per-wall strength, whether open doors read as clear (Visibility
   channel should handle it), glass handling, maybe scale muffle slightly
   with distance-through-wall.

## Risks / fallbacks

- **FHitResult layout drift across patches** — the oversized-buffer +
  offset-mirror approach contains it; binary mode needs no OutHit reads at
  all and is the guaranteed-safe fallback.
- **LineTraceSingle via CDO misbehaving** — fallback to
  `AActor::ActorLineTraceSingle` on the listener pawn, or worst case a
  cheap heuristic (different Z floors => 1 wall) which still beats nothing.
- **Doors that are "open" but blocked by the door mesh swung into the
  doorway** — traces are per-tick so state follows the door; if Visibility
  behaves oddly for RoN's door meshes, try `TraceTypeQuery2` (Camera).
