# Splitsgate

**[Download Splitsgate](https://github.com/cody-raves/Splitsgate/releases/latest/download/Splitsgate.zip)** ·
**[Beginner setup guide](https://cody-raves.github.io/Splitsgate/)**

Splitsgate is a native Windows race-splits overlay for **Splitgate 1 and
Splitgate 2**. It reads the game's race state directly and displays a compact
in-game timer with per-ball splits and personal-best comparisons.

## Features

- Game-synchronized race timer and ball counter
- Expanding per-ball split list
- Personal-best storage and split deltas for every map and difficulty
- Your saved PB time shown directly in the race HUD in both games
- Automatic map and difficulty detection in the race lobby
- Clean lobby reset between runs
- Hold `R` to restart the active race
- Double-tap `R` to open settings, or press `` ` `` as a fallback
- Move, resize, and recolor the HUD
- Automatic update notifications on launcher startup
- Restart and pre-race countdown filtering so the timer begins at the game's real zero

## Compatibility

This project targets the current 64-bit Windows builds of both games. It relies
on game-specific native state, so a future game update may require an overlay
update.

## Building

Requirements:

- Windows 10 or 11
- Visual Studio 2022 Build Tools with the C++ desktop workload
- CMake 3.20 or newer

From a Developer PowerShell:

```powershell
git clone --recursive https://github.com/cody-raves/Splitsgate.git
cd Splitsgate
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build creates a self-contained launcher. The overlay DLL and logo are
embedded in the EXE and prepared automatically when it runs.

## Running

1. Download and extract `Splitsgate.zip` from the latest release.
2. Start Splitgate 1 or Splitgate 2.
3. Run `Splitsgate.exe` (the recommended dual-game launcher).
4. Press `End` in game to unload the overlay.

The zip also contains `Splitsgate-Splitgate1.exe` and
`Splitsgate-Splitgate2.exe` for users who want a game-specific launcher. All
three EXEs contain the same overlay and share the same settings and PB files.

Personal-best data and settings are kept in `%LOCALAPPDATA%\Splitsgate\Runtime`.

## Crash diagnostics

Anonymous crash reporting is optional and disabled by default. Enable **Send
anonymous crash reports** in the overlay settings to let the launcher report an
abnormal game-process exit. Reports contain the Splitsgate version, game name,
Windows exit code, and HUD layout values. They do not include usernames,
personal-best files, screenshots, or memory dumps.

Reports that cannot be sent are retained in
`%LOCALAPPDATA%\Splitsgate\Runtime\CrashReports`. The report service URL can be
provided through `%LOCALAPPDATA%\Splitsgate\Runtime\crash-report-endpoint.txt`
or the `SPLITSGATE_CRASH_REPORT_URL` environment variable.

On startup, the launcher checks the latest GitHub release. If a newer version
is available, it downloads, replaces, and relaunches the single EXE
before loading the overlay. Release builds should attach `Splitsgate.exe` as a
GitHub release asset; zip assets remain supported for older releases.

## Third-party software

The repository vendors [Dear ImGui](https://github.com/ocornut/imgui) and [MinHook](https://github.com/TsudaKageyu/minhook). Their license files are retained in their respective `vendor` directories.

## Disclaimer

Splitsgate is an independent community project and is not affiliated with or endorsed by 1047 Games. Splitgate names and marks belong to their respective owners. Use injected game modifications at your own risk.
