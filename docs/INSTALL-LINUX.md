# Installing RoN Tactical Radio on Linux

For playing Ready or Not through **Steam Proton** with the **native Linux
TeamSpeak 3 client**. The mod detects this setup automatically: the game mod
broadcasts state over local UDP (port 39440), which crosses the Proton/native
boundary (Windows shared memory does not).

You need the `RoN-TacticalRadio-friend-pack.tar.gz` from whoever built the mod
(`tar xzf RoN-TacticalRadio-friend-pack.tar.gz` to extract).

## 1. Game side (into the Proton game folder)

1. Find the game folder, typically:
   `~/.steam/steam/steamapps/common/Ready Or Not/ReadyOrNot/Binaries/Win64/`
2. Copy the contents of the archive's `game/` folder into it. You should end up with:
   - `Win64/dwmapi.dll` (the UE4SS loader)
   - `Win64/ue4ss/UE4SS.dll`, `UE4SS-settings.ini`, `Mods/` (contains `RoNTacticalRadio`)
3. In Steam: Ready or Not → Properties → Launch Options:

   ```
   WINEDLLOVERRIDES="dwmapi=n,b" %command%
   ```

   (Tells Proton to load the mod's `dwmapi.dll` instead of Wine's built-in.)

## 2. TeamSpeak side (native Linux client)

1. Copy `teamspeak/linux/ron_tactical_radio_linux_amd64.so` to
   `~/.ts3client/plugins/` (create the folder if missing).
2. Start TS3 → Tools → Options → Addons → enable **RoN Tactical Radio**.
3. Options → Capture → set **Voice Activity Detection**. Local voice is
   always on: anyone near you in-game hears you positionally when you speak.

If the plugin fails to load (very old distro / glibc), build it locally —
one command, only needs g++ and git:

```
./scripts/build-linux-plugin.sh     # from the repo, or see the zip's src/
```

## 3. Verify

- Launch the game: `Win64/ue4ss/UE4SS.log` should show
  `[RoNTacticalRadio] shared memory ready, udp ready` and 5-second heartbeat
  lines (`inGame=... players=...`).
- TS3 → Tools → Client Log: `plugin loaded`, then `game link active: ...`
  once you're in a lobby.
- In a match with the mod's author: each of you should see
  `mapped TS client '<name>' -> game player '<name>'`, voice should fade with
  distance and pan left/right as you turn, and holding **Caps Lock** (radio
  PTT) should be heard by the other with a radio effect at any distance.

## Troubleshooting

- **No UE4SS.log at all** → launch option not applied or files in the wrong
  folder; double-check step 1.
- **Log shows `inGame=0` forever** → you're in the menus; join a lobby.
- **`game link lost` in TS while playing** → game and TS must run on the same
  machine; check nothing else occupies UDP port 39440.
- **Voices don't fade** → both players need the mod installed; unmodded
  players are passed through at normal volume by design.
- Keys are currently hardcoded (Caps Lock / V / ]) — Wine forwards them fine,
  but tell the author if you want them remapped.
