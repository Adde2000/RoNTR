# RoN Tactical Radio

TFAR-style proximity voice + radio for **Ready or Not**, via a TeamSpeak 3
plugin fed by a UE4SS game mod. See `docs/DESIGN.md` for the full architecture.

```
shared/RadioLink.h      IPC contract: shared memory + UDP structs (both sides include this)
game-mod/               UE4SS C++ mod (runs inside RoN, publishes game state)
ts3-plugin/             TeamSpeak 3 client plugin (positional audio, radio DSP, tests)
scripts/                setup / build / deploy / package automation (see below)
docs/                   DESIGN.md, INSTALL-LINUX.md, RELEASING.md
.github/workflows/      CI (tests + plugin builds) and tag-driven releases
```

Releases: push a `vX.Y.Z` tag and CI attaches a ready-to-install pack to a
GitHub release — see `docs/RELEASING.md` (one repo secret required).

## Status

Milestone M1 code-complete. The full pipeline is plumbed:
game mod → shared memory → TS plugin → per-client proximity/radio DSP.
Game-side reads are implemented with class/function names confirmed against
the RoN UHT dump (`AReadyOrNotPlayerController`, `APlayerCameraManager::GetCameraLocation/Rotation`,
`AReadyOrNotPlayerState::GetPlayerName/GetPawn`, `AActor::K2_GetActorLocation`,
`AReadyOrNotCharacter::IsDeadOrUnconscious`). Needs an in-game smoke test.

Note: vanilla RoN has a built-in proximity VOIP (`UProximityVoiceComponent`,
`EVoiceType{VT_Local,VT_Team}`) — disable in-game voice while using this mod
to avoid doubled audio.

## Build & deploy (automated)

Prerequisites (one-time installs): Git, CMake 3.22+, Visual Studio 2022 with
the Desktop C++ workload, Rust via [rustup.rs](https://rustup.rs) (RE-UE4SS
dependency), and a GitHub account linked to Epic Games (RE-UE4SS's Unreal
submodule — see [docs.ue4ss.com](https://docs.ue4ss.com/guides/creating-a-c++-mod.html)).

Then, in PowerShell:

```powershell
cd <this folder>
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\all.ps1 -InstallUE4SS   # setup + build + deploy (omit flag if UE4SS already installed)
```

Or step by step: `scripts\setup.ps1` (clone RE-UE4SS + TS3 SDK),
`scripts\build.ps1` (both DLLs; first RE-UE4SS build is slow),
`scripts\deploy.ps1` (game mod → `ue4ss\Mods\RoNTacticalRadio\dlls\main.dll`
+ mods.txt entry; TS plugin → `%APPDATA%\TS3Client\plugins`).

After deploying: enable the plugin in TS3 Options → Addons. Set capture mode
to **Voice Activity Detection** — local voice is always on (heard positionally
by nearby players); the radio PTT key transmits on the radio net.

### Host-side tests (any OS)

```
cd ts3-plugin
cmake -B build -DRTR_BUILD_TESTS=ON && cmake --build build && ./build/rtr_tests
```

## Next steps

Milestones can be found here: [Milestones](https://github.com/Adde2000/RoNTR/milestones)

## Keys (defaults, in `dllmain.cpp`)

- Caps Lock — radio PTT (local voice is always on, no key needed)
- ] — cycle radio channel



_Test_