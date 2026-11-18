# Splitsgate

Splitsgate is a native Windows race-splits overlay for Splitgate. It reads the game's race state directly and displays a compact in-game timer with per-ball splits, personal-best comparisons, and a hold-to-restart shortcut.

## Features

- Game-synchronized race timer and ball counter
- Expanding per-ball split list
- Personal-best storage and split deltas for every map and difficulty
- Automatic map and difficulty detection in the race lobby
- Clean lobby reset between runs
- Hold `R` to restart the active race
- Press `` ` `` to open settings and change the restart key
- Restart and pre-race countdown filtering so the timer begins at the game's real zero

## Compatibility

This project targets the current 64-bit Windows build using `PortalWars-Win64-Shipping.exe`. It relies on game-specific native offsets, so a future game update may require the constants in `overlay.cpp` to be updated.

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

The build produces:

- `build/Release/SplitgateSplitsOverlay.dll`
- `build/Release/SplitgateOverlayLoader.exe`
- `build/Release/assets/splitgate-new-s-mark-clean.png`

## Running

1. Keep the loader, DLL, and `assets` folder together.
2. Start Splitgate and enter the race lobby.
3. Run `SplitgateOverlayLoader.exe`.
4. Press `End` in game to unload the overlay.

Personal-best data is saved beside the DLL as `splitgate-personal-bests.tsv`. That file is ignored by Git.

## Third-party software

The repository vendors [Dear ImGui](https://github.com/ocornut/imgui) and [MinHook](https://github.com/TsudaKageyu/minhook). Their license files are retained in their respective `vendor` directories.

## Disclaimer

Splitsgate is an independent community project and is not affiliated with or endorsed by 1047 Games. Splitgate names and marks belong to their respective owners. Use injected game modifications at your own risk.
