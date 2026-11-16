# Tekken 7 Seamless Online Rematch

A `dinput8.dll` proxy mod for **TEKKEN 7** (Steam, PC) that restarts an online player match **in place** after both players accept a rematch, the same way offline VS does. No stage or character unload/reload, no trip back through the lobby.

Includes a small in-game overlay (Dear ImGui) with an on/off switch, toggled with **F7**.

> **Both players need the mod, with the toggle ON.** If your opponent does not have it, turn it OFF in the overlay (F7), or the two clients desync on rematch.

## Compatibility

| | |
|---|---|
| Game | `TekkenGame-Win64-Shipping.exe`, Unreal Engine 4.13.2, Steam x64 |
| Exe build | PE TimeDateStamp `0x63eb1c19` only |
| Other builds | The mod checks every patch site byte-for-byte and disables itself (stock behaviour) on any mismatch |

## Install

1. Download `dinput8.dll` from the [latest release](../../releases/latest).
2. Copy it to `TekkenGame\Binaries\Win64\` next to `TekkenGame-Win64-Shipping.exe`.
3. Launch the game. Press **F7** in game to open the menu.

Uninstall: delete `dinput8.dll`. The game itself is never modified on disk.

Files the mod creates next to the exe:

| File | Purpose |
|---|---|
| `rematch_inplace.on` | Saved toggle state (present = ON) |
| `rematch_mod.log` | Status and error log, recreated each launch |
| `rematch_overlay.off` | You create it to disable the overlay entirely (rematch follows `rematch_inplace.on`) |

No runtime dependencies beyond Windows itself (statically linked; needs `d3d11.dll` and `D3DCOMPILER_47.dll`, which ship with Windows 10/11).

## How it works (short)

On mutual accept, the game's online post-match handler (`playermatch_revenge`) normally returns 0 and tears the battle down. The mod:

1. Waits for the session handshake to reach `BATTLE_STAT_IDLE`.
2. Calls the game's own `network_setup`.
3. Jumps into the offline "retry" reset block (stage reload included, so broken floors/walls are restored).
4. Dismisses the leftover "Connecting..." message box.

The in-game overlay draws from a 6-byte patch at the engine's `FD3D11Viewport::Present` call site. It does not touch DXGI vtables, so it coexists with the Steam overlay.

Full reverse-engineering notes, addresses and the crash post-mortems are in [REMATCH_RESEARCH.md](REMATCH_RESEARCH.md).

## Known limitations

- The rematch still waits about 15 s: the handshake goes through Steam lobby room data. Skipping it would need direct peer signalling (not implemented).
- Only one exe build is supported (see above).
- The latest overlay hook (engine call-site patch) was verified with synthetic tests against the Steam overlay DLL, not yet across many real online sessions. Please report problems with `rematch_mod.log` attached.

## Project structure

```
tekken7-seamless-rematch/
├── dinput8.cpp             # the whole mod: DirectInput8 proxy, patcher, rematch thunk, overlay
├── imgui_user_config.h     # routes ImGui asserts to the log instead of abort()
├── build.bat               # MinGW-w64 build (release / research)
├── REMATCH_RESEARCH.md     # reverse-engineering notes, addresses, crash post-mortems
├── LICENSE                 # MIT (this project)
├── README.md
└── imgui/                  # vendored Dear ImGui v1.92.9b + DX11/Win32 backends (MIT, own LICENSE.txt)
```

`dinput8.dll` is built into the repo root (git-ignored) and published via Releases.

Installed layout (game folder):

```
TekkenGame/Binaries/Win64/
├── TekkenGame-Win64-Shipping.exe
├── dinput8.dll             # this mod
├── rematch_inplace.on      # created by the mod: toggle ON
├── rematch_mod.log         # created by the mod
└── rematch_overlay.off     # optional, you create it to disable the overlay
```

## Build

Requires MSYS2 MinGW-w64 `g++` (`C:\msys64\mingw64\bin`).

```
build.bat            :: release build -> dinput8.dll
build.bat research   :: adds full game-trace logging and UE ProcessEvent probe
```

## Credits

- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut and contributors, v1.92.9b, MIT license (`imgui/LICENSE.txt`).
- **TEKKEN 7** is a trademark and product of Bandai Namco Entertainment. This project is unofficial, unaffiliated, and contains no game code or assets.
- Mod and reverse engineering: RAZORRZR0.

## Disclaimer

Use at your own risk, in unranked/private matches with a friend who also runs the mod. Modifying game memory can be against the game's terms of service and may be flagged by anti-cheat; the author is not responsible for bans or any other consequences.

## License

MIT for this project's own code (see `LICENSE`). Dear ImGui is MIT (see `imgui/LICENSE.txt`).
