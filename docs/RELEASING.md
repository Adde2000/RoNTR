# Releasing

Releases are built by `.github/workflows/release.yml` when you push a version
tag:

```
git tag v0.3.1
git push origin v0.3.1
```

The workflow builds the Windows TS3 plugin, the Linux TS3 plugin, and the
game mod + UE4SS runtime, assembles the install pack
(`game/` + `teamspeak/` + install docs), and attaches
`RoN-TacticalRadio-vX.Y.Z.tar.gz` / `.zip` to a GitHub release.

## Required repo secret: `UEPSEUDO_TOKEN`

The game mod builds against RE-UE4SS, whose `Unreal` submodule
([UEPseudo](https://github.com/Re-UE4SS/UEPseudo)) is a **private** repo,
visible only to GitHub accounts linked to Epic Games (the same prerequisite
as building locally — see docs.ue4ss.com).

1. On an Epic-linked GitHub account, create a fine-grained personal access
   token with read access to repositories (it must be able to read
   `Re-UE4SS/UEPseudo`).
2. In this repo: Settings → Secrets and variables → Actions → New repository
   secret, name `UEPSEUDO_TOKEN`.

The workflows rewrite the submodule's `git@github.com:` URL to token-based
HTTPS. Without the secret, CI skips the game-mod job; a release tag fails
with a clear error.

Note: releases redistribute a compiled UE4SS build. RE-UE4SS is MIT-licensed
(include their license notice), and UEPseudo's sources are never included —
only compiled artifacts.

## Version bumps

Update both before tagging (keep them equal to the tag):

- `ts3-plugin/src/plugin.cpp` → `PLUGIN_VERSION`
- `game-mod/src/dllmain.cpp` → `ModVersion`

If the shared-memory/UDP structs in `shared/RadioLink.h` change, also bump
`RTR_VERSION` — it prevents mismatched halves from talking to each other.

## CI (`ci.yml`)

Every push/PR: host-side tests (DSP, spatial math, UDP transport) with
`-Werror`, plus Windows and Linux TS-plugin builds. The game-mod job runs
only when `UEPSEUDO_TOKEN` is configured.
