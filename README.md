# OpenBox Achievements for Forza Horizon 2 Extra Options

Forza Horizon 2 achievements on PC, shown by the
[OpenBox Achievements](https://github.com/byzyn4ik/OpenBoxAchievements) app.
This is a plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions):
it watches the game's own achievement telemetry, evaluates the FH2 achievement
rules in game and sends the unlocks and progress to OpenBox Achievements.

It needs no fork of Extra Options: it is one DLL in a mod folder, loaded by a
stock Extra Options 0.3.2.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.2.
- Forza Horizon 2 Extra Options 0.3.2 or later (its mod loader).
- The OpenBox Achievements app, installed with its setup (MSI). Without it the
  plugin does nothing and leaves the game as it is.

## Install

1. Close the game.
2. Copy the `mods\OpenBox Achievements` folder (from a release ZIP, or
   `out\mods` after building) into the `mods` folder beside `Azimuth.exe`
   and Extra Options' `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       OpenBox Achievements\
         mod.json
         plugins\
           OpenBoxAchievements.dll
           OpenBoxAchievements.ini
   ```

3. Start the game. Extra Options adds the folder to `mods\loadorder.txt` and
   loads the plugin; `FH2_ExtraOptions.log` shows lines starting with
   `OPENBOX ACHIEVEMENTS:`.

To turn it off, set `Enabled=0` in `OpenBoxAchievements.ini`, or comment the
folder out in `mods\loadorder.txt` (`# OpenBox Achievements`).

## Settings

`mods\OpenBox Achievements\plugins\OpenBoxAchievements.ini`, read when the game
starts (Extra Options' own `FH2_ExtraOptions.ini` is not used or changed).
Empty values mean the default.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Achievements]` | `Enabled` | `1` | evaluate the rules and send the results to OpenBox |
| | `Game` | `fh2` | the OpenBox game id |
| | `PauseButton` | `1` | the pause menu's Achievements button (Progress tab) opens OpenBox Achievements |
| | `Saves` | see below | the saves folder for the save stats (full, or relative to the launcher's folder) |
| | `Rules` | built in | another rules file (full, or relative to the plugin's folder) |
| `[OpenBox]` | `AppFolder` | from the registry | the app's folder (full, or relative to the launcher's folder) |
| | `StartWithGame` | `1` | start OpenBox Achievements in the tray with the game when it is not running |
| | `Data` | `%LOCALAPPDATA%\OpenBoxAchievements` | OpenBox's data folder |
| | `Pipe` | `OpenBoxAchievements.Events.v1` | OpenBox's pipe name |
| `[Logging]` | `Enabled` | `1` | `OpenBoxAchievements.log` beside the plugin |
| | `Telemetry` | `0` | also `OpenBoxAchievements.telemetry.jsonl`: every event the rules saw, for replays |

The saves are looked for in `<launcher>\data\titles\1A96545B\saves` and in
`Documents\My Games\Forza Horizon 2\Saves\1A96545B`; the newest save wins.

## How it works

- **Finding OpenBox.** The app's setup records its folder in
  `HKCU\Software\OpenBox Achievements\InstallFolder` (the value the Forza
  Horizon 2 Builder reads too). `[OpenBox] AppFolder` overrides it. When
  `OpenBox Achievements.exe` is not there, the plugin installs nothing.
- **Telemetry.** FH2 reports gameplay as Xbox Live ETX events through
  `EtwPlus.dll!EtxEventWrite`; on the console the achievements were unlocked
  from them on the server. From `FH2PluginInitialize` the plugin points the
  game's import slot for that function (RVA `0x1dd0060`) at a wrapper that
  turns each event into a JSON record for the rules and then calls the
  original function unchanged. If the host has not resolved the import yet,
  a short-lived thread waits for it; the per-car-update callback
  (`FH2PluginVehicleUpdate`) rewraps it if the host refills the slot.
- **Save stats.** When the player first drives, the newest save's
  `ProfileData` is read once (on a worker thread), so progress made before
  the plugin was installed counts too.
- **Rules.** The FH2 rules (`rules\fh2_achievement_rules.json`) are built into
  the DLL. They live here, in the game-side mod: OpenBox Achievements ships no
  rules, only the achievement IDs. Counters and unlocks are kept in
  `%LOCALAPPDATA%\OpenBoxAchievements\games\fh2\ingame_state.json` (on the
  first run it is made from OpenBox's `state.json`, so earlier progress
  carries over).
- **OpenBox.** The results go over OpenBox's local pipe
  `\\.\pipe\OpenBoxAchievements.Events.v1` (`hello`, `unlocked`, `progress`,
  `end`; OpenBox answers `ok`; see OpenBox's `docs/EVENTS.md`). Unlocks are
  resent until OpenBox has them, so none is lost while the app is closed.
  `hello` carries the game's process id (`pid=<n>`), and while there is
  nothing to send the plugin says `alive fh2 pid=<n>` every 5 seconds: OpenBox
  counts FH2 as played (Game Bar widget, time played, the page over the game)
  while the plugin is on the pipe, not by the game's EXE name, and stops when
  the game's process exits. Older OpenBox versions ignore `alive`.
- **Pause menu.** The game's own Achievements button (Progress tab) asks the
  console's system UI for the title's achievements through
  `PopupShowAchievements`' vtable slot `0x2e360c8` (method `0xe38770`). The
  plugin points that slot at a handler that sends what is pending and brings
  OpenBox Achievements forward on FH2's page, then runs the game's method as
  before. No menu layout (XAML) is changed and no new tile is added.

### Guards and coexistence

Each hook is checked byte for byte against the executable Extra Options 0.3.2
supports (`forza_durango_release_final.exe`, SHA-256 `3ec04c61...0a3`), and on
any mismatch it is not installed and the game is left alone (the reason is
logged):

| Hook | Site | Guard |
|---|---|---|
| ETX telemetry | import slot `0x1dd0060` | image size `0x3a18000`; the first event name `AnnaRecommendation` at `0x1e64338` and the event table at `0x2cb9280` pointing at it; the image's import table names `EtwPlus.dll!EtxEventWrite` at exactly this slot; the slot holds executable code outside the game image |
| Achievements button | vtable slot `0x2e360c8` | the slot holds `image + 0xe38770`, and the method's first 20 bytes (its prologue) match |

Extra Options 0.3.2 patches neither site (checked against its sources and
`patches.json`), and the plugin does not touch its XAML resource hook or the
pause page init that Extra Options already hooks. Both slots are swapped
atomically, and the original is always called. If another module wraps the
ETX import after this plugin, it is left alone (no double wrapping).

The plugin exports `FH2PluginInitialize` and `FH2PluginVehicleUpdate` only.
It does not use `FH2PluginDatabaseUpdate`: the merged game database holds the
game's content (cars, events), not the player's progress, which comes from the
telemetry and the save.

## Building

Python 3.10+ and [Zig 0.15.2](https://ziglang.org/download/) (C++17, every
file built with `-Wall -Wextra -Werror`; no Visual Studio or Xbox SDK needed):

```text
py build.py --zig <zig-x86_64-windows-0.15.2>\zig.exe --test
```

Output in `out\`: `mods\OpenBox Achievements\` (the mod to install),
`tools\ach-replay.exe` and `tests\plugin_test.exe`. `--test` runs the offline
tests. CI (GitHub Actions) builds and tests every push and pull request and
keeps the mod folder as an artifact; a `v*` tag publishes it as a release ZIP.

## Testing

`py tests\run_tests.py` (after a build; `build.py --test` runs it) checks,
without the game or a running OpenBox:

1. **Package**: `mod.json` as Extra Options validates it, the folder layout,
   the DLL (x64, the loader's exports, only Windows imports) and the INI.
2. **Rules**: the built-in rules' IDs match OpenBox's FH2 manifest (when the
   OpenBox app repo is beside this one), and that manifest has no rules.
3. **Replay**: `tests\data\*.jsonl` (fabricated events) through
   `ach-replay.exe`, compared with `*.expected.json` (unlocks, progress).
4. **Plugin**: `plugin_test.exe` loads the built DLL as the Extra Options
   loader does, with a fake FH2 image (the import table, event table and
   Achievements vtable at the inspected RVAs), a fake OpenBox pipe server and
   a fake OpenBox app: events through the hooked import reach the original and
   unlock achievements, the save stats count, OpenBox gets `unlocked` and
   `progress` lines (`hello` with the pid, then `alive` heartbeats while idle),
   the button brings up the app and still runs the game's
   method; a late-resolved import, a mismatched image, a mismatched button and
   a missing app are each handled without touching the game.

The OpenBox app's own tests (`run-tests.ps1` in its repo) also run this
DLL, through `plugin_test.exe bus`, against the real app on a private pipe.

Replaying a recorded session (`[Logging] Telemetry=1`):

```text
out\tools\ach-replay.exe replay --manifest rules\fh2_achievement_rules.json --out state.json OpenBoxAchievements.telemetry.jsonl
out\tools\ach-replay.exe compare expected-state.json state.json
```

## License

GPL-3.0 (see `LICENSE`). The rules engine and the telemetry and save readers
come from the OpenBox fork of Forza Horizon 2 Extra Options, and `src\fh2_plugin.h`
from Extra Options itself, both GPL-3.0.
