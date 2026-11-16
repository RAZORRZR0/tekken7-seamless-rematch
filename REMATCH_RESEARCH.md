# TEKKEN 7: online rematch research

Goal: make an online player-match rematch restart in place, like offline VS, instead of tearing the battle down and reloading.

Binary: `TekkenGame-Win64-Shipping.exe` (PE32+, image base `0x140000000`, TimeDateStamp `0x63eb1c19`). All addresses below are for this build.

## Tooling

`dinput8.dll` (source `dinput8.cpp`) is a proxy DLL. The game imports only `DirectInput8Create` from `DINPUT8.dll`. The proxy redirects the game's stripped debug printf (`0x140556170`, body is `save args; ret`, 852 callers) into `rematch_mod.log` next to the exe, with a timestamp and thread ID on each line. The stub's bytes occur twice in `.text`, so the DLL finds it through the unique call site at `0x140391b29` (the `is_btl_loop` log).

The probe also logs two more sources. Each patch site is checked byte-for-byte first, and the probe stays off on any other exe build:

- `NATIVE>`/`NATIVE<`: enter and return of the match loop (`0x3f4870`, non-zero = replay in place), `play_match` (`0x3f4c40`, returns end type 1–8) and the eight post-match handlers (`end7_playermatch_revenge` = online). The `call rel32` sites are redirected through a jump page allocated within ±2 GB of the exe.
- `UE>`: `UObject::ProcessEvent` (`0x936b80`, found as the only function in 1,111 vtable slots that tests `FUNC_Native` at `UFunction+0x88`). All vtable slots and the single direct call (`0x12be28f`) are patched. It logs only the load/reinit/battle functions listed in the Unreal section, as `Outer.Func self=<object> parms=<first 8 bytes>`. Names are read via `GNames` (128 chunks × 16384 `FNameEntry*`, name at `+0x10`, bit 0 of `Index` = wide string). This is checked at runtime by requiring `GObjects[0]` to be named `CoreUObject`. Limitation: `ProcessEvent` only sees C++→Blueprint calls, not Blueprint→native calls.

## Evidence: logs

### Offline VS (`rematch_mod_offline.log`)

| t (ms) | event |
|---|---|
| 19078453 | `CBattleManagerConsole::load_start()`: match 1 loads |
| 19099812 | `Called Ready-Fight.` |
| 19137843 | `STAR MAX=2`, `SwfHUD`: **rematch starts, no `term()`, no `load_start()`** |
| 19145890 | `Called Ready-Fight.` (about 8 s after the HUD, intros skipped) |
| 19187890 | `request_term()`, then `term()`, only when leaving to the menu |

### Online player match, guest side, rematch accepted (`rematch_mod.log`)

| t (ms) | Δ from accept | event |
|---|---|---|
| 964968 | 0 | `IsPlayerMatchRematchAccepted : revenge [Accept][Accept]`, `playermatch_revenge : rematch [Rematch]` |
| 964968–965125 | 0.2 s | **`CBattleManagerConsole::term()` begin/end: the battle is torn down** |
| 965000 | | `NetSeqSession: IsPlayerMatchRematch. is_btl_loop[true]` (unmodified game) |
| 965578 | | `SUBSTAT_SESSION_REMATCH_ROOM_BATTLE_WAIT → SUBSTAT_SESSION_REMATCH_CHECK_BATTLE_START`, `m_bPlayerMatchRematch → TRUE` |
| 972578 | 7.6 s | `BATTLE_STAT_NONE → GUEST` (7 s idle before the handshake) |
| 972703–974937 | 10.0 s | `MatchedAsClient`, `GUESTSYNC → WATCH_GUEST → BATTLE_START_SYNC → IDLE` (peer handshake) |
| 975062 | 10.1 s | `CBattle::setup_makuai` (VS splash) |
| 984218 | 19.3 s | **`load_start()`: full reload** |
| 987390–994250 | 29.3 s | `network_setup : Load wait start … Load finished` (waits for both peers) |
| 1007484 | 42.5 s | `doPostedPlayWith`, `STAR MAX=2` |
| 1025203 | **60.2 s** | `Called Ready-Fight.` (full intros) |

The first online match took 31 s from VS splash to fight. The rematch took 50 s.

## Root cause (static analysis)

`CBattleManagerConsole` runs each match in a loop at `0x1403f4870`:

```text
0x1403f49a0  loop:
  call 0x1403f5870              ; per-match setup
  call 0x1403f4c40              ; play match -> ebp = end type 1..8
  switch (ebp) -> post-match handler, sil = handler's return value
     0x1403f8510, 0x1403f3210, 0x1403fb4b0(0), 0x1403fb4b0(1),
     0x1403fa310, 0x1403fb1e0, 0x1403f9d80 (playermatch_revenge), 0x1403fac40
  if (!m_is_term(+0x97) && !m_is_request_term(+0x9a) && sil) goto loop   ; replay in place
  return 0
```

Its caller (`0x1403f7be0`, battle manager exec) at `0x1403f8353` runs the match loop again if that call returns non-zero. Otherwise it calls `term()` (`0x1403fc610`).

- Offline handlers return `1` on rematch, so the loop replays without `term`/`load_start`. That is the instant path.
- `playermatch_revenge` (`0x1403f9d80`) always ends with `xor al, al` at `0x1403fa061`, so it always returns `0`. Online rematch therefore always falls out of the loop, runs `term()`, and goes through the network session's rematch flow (`NetSeqSession` with `is_btl_loop`, `SUBSTAT_SESSION_REMATCH_*`). That flow re-syncs both peers and then does a full `load_start`.

`is_btl_loop` (`[*0x1434d0b80 + 0x910]`) is already `true` in the unmodified game. The earlier "patches 1–3" premise (that the flag is being killed) is wrong, and those patches were removed.

## Open problem for an in-place online rematch

Making `playermatch_revenge` return `1` would keep the battle loaded. But then:

1. `NetSeqSession` waits for battle termination (loop at `0x1403914dc–0x140391507` on `0x1403f0e40`, which reads `m_is_term`) and would stall.
2. The peer handshake (`BATTLE_STAT_GUEST/HOST → … → BATTLE_START_SYNC`, `MatchedAsClient`) would not run. That handshake is where both sides agree on fighters and stage and reset network battle state. Skipping it risks desync.

A working mod must therefore replay in place **and** run (or replicate) the battle-start sync without `term()`/`load_start()`. Next step: trace what the offline handler that returns `1` resets (`0x1403fa310` is the likely retry menu), and what `BATTLE_START_SYNC` resets.

## Cheaper partial wins seen in the timeline

- About 7 s idle between `CHECK_BATTLE_START` and `BATTLE_STAT_GUEST`.
- About 9 s VS splash (`setup_makuai` → `load_start`).
- Full character intros on the rematch (offline skips them).

## Unreal side (UnrealModLoader dump: ObjectDump.txt, EngineInfo.txt, RootLevel_Dump.txt)

Engine: custom UE 4.13.2 fork ([ue4-tekkengame](https://github.com/Modding-Zaibatsu/ue4-tekkengame)). The exe has no version string. Dumper-7 crashes on it; RussellJ's UnrealModLoader (Tekken 7 build, tekkenmods.com/mod/2579) works. EngineInfo gives these RVAs: `GNames 0x368AF60`, `GObjects 0x3693550`, `GWorld 0x379C7A0`.

**The battle "reload" is not an Unreal map change.** One persistent `RootLevel` stays loaded (17 `WorldSettings` = persistent level plus streamed sublevels). Stages and characters are streamed in and out under it:

| Class | Relevant functions |
|---|---|
| `TekkenGame.LevelLoader` / `BP_LevelLoader_C` | `LoadStreamLevel`, `UnloadStreamLevel`, **`ReloadStreamLevel`**, `LoadRequestedLevel`, `AreAllSubLevelsVisible` |
| `TekkenGame.CharacterManager` / `BP_CharacterManager_C` | `ReceiveCharactersBeginLoad`, `ReceiveCharactersBeginUnload`, **`ReceiveCharactersBeginReinitialize`**, `AsyncLoadCharacters` |
| `BPI_TekkenPlayer_Modular_C` | `LoadCharacter`, **`ReinitializeCharacter`** |
| `TekkenGame.Resident` / `BP_Resident_C` | `SpawnBattleActors`, `AsyncSpawnBattleActors`, `DestroyBattleActors` |
| `TekkenGame.TekkenGameMode` | `Set/GetLevelLoadCompletedFlag`, **`SetLevelReLoadCompletedFlag`**, **`SetReinitCharCompletedFlag`**, `Set/GetSecretlySwitchingStageFlag`, `SetLevelLoadCancel*` |
| `TekkenGame.LevelTransition` | `RequestTransition`, `IsTransferring`, `OnTransitionRequested/Completed` |
| `BPI_EventListener_C` (implemented by ~40 stage/effect BPs) | `OnPreInitBattle`, `OnInitBattle`, `OnUnloadLevel`, `OnBattleEnd`, `OnRoundSettled` |

The engine has a separate reinitialize path (`ReceiveCharactersBeginReinitialize`, `ReinitializeCharacter`, `ReloadStreamLevel`, `SetReinitCharCompletedFlag`, `SetLevelReLoadCompletedFlag`) alongside the full load/unload path. [INFERENCE] Offline rematch uses reinitialize and online rematch uses unload/load. Not yet observed.

Next probe: log calls to the functions in the table above (UObject `ProcessEvent` hook, or UnrealModLoader's function hooking) during one offline and one online rematch. The difference tells which path to force online.

Triage: 429 game-side classes (engine/plugin packages pre-filtered: 1330 classes removed), judged by role. 209 flagged (battle_flow 74, online 59, loading 24, plus any rated as a possible hook point); all read.

## Probe run 2 (offline VS, then online player match as host)

Native probe confirmed the loop model (the Unreal probe was off in this run because the name check expected `CoreUObject`, but the game names it `/Script/CoreUObject`; fixed):

- **Offline:** `play_match → 5`, `end5_handler` (`0x3fa310`, retry menu) `→ 1`. The second match's HUD appears in the same millisecond, with no `term()`, `load_start` or `network_setup`: an in-place replay.
- **Online:** `play_match → 7`, `end7_playermatch_revenge → 0` right after `revenge [Accept][Accept]`, so `match_loop → 0`, then `term()`.
- `network_setup` (the per-battle peer sync) runs once per match-loop entry, not per replay.

Host timeline, Accept → Ready-Fight, **45 s**: `term()` 0 s; 8 s waiting for the guest's room data; session handshake to `IDLE` at 13 s; VS splash to `load_start` at 22.5 s; `network_setup` load **1.0 s** (assets already resident); intros until 39 s; fight at 45 s.

**The rematch handshake re-sends byte-identical match parameters.** Host: seed 43575, stage 40, fighters 9/8, 3 rounds, 60 s, both times. Guest session: seed 65183, stage 54, both times. A rematch needs no new negotiation.

## Experiment: in-place online rematch

`rematch_inplace.on` next to the exe (**both players**). The trigger is the game logging `playermatch_revenge : rematch [Rematch]` (mutual accept). Declines are untouched.

### v1 (only force `end7_playermatch_revenge` to return 1): failed, rematch prompt loops

After the forced 1, the match loop went straight to `play_match`, which returned 7 again ~1 s later with no battle in between. Each pass re-opened the rematch prompt. Returning 1 only means "stay in the loop": the **handler itself must reset the battle**, and the offline retry handler does that before it returns 1.

Normal online rematch, Unreal side (probe run 3, without the experiment): `OnBattleEnd`, Accept, `ReceiveCharactersBeginUnload`, `DestroyBattleActors`, `AsyncSpawnBattleActors`, `ReloadStreamLevel` (same stage), `ReceiveCharactersBeginLoad`, `OnPreInitBattle`, `OnInitBattle`.

### v2 (run the offline retry handler's own reset): battle restarts in 0.8 s, but desyncs

The offline retry handler `0x3fa310` runs its "Rematch, same characters" reset at `0x3fa627`–`0x3fac1e`, ending in `mov al, 1`. It calls `0x2616f0`, `0x2aad40(1)`, `0x4cd560`, `0x445f90`, `0x4c0ff0`, `0x4498d0`, `0x449370`, `0x260e70`, the `0x25fxxx` wait/reinit chain, `0x3fc000`, `0x4c5a70`, `0x4cd4e0` and `0x2ab150(1)`, with frame waits in between.

After `playermatch_revenge` returns on a mutual accept, the wrapper calls a thunk that runs this original code instead of reimplementing it:

1. Rebuild `0x3fa310`'s frame: same pushes, `sub rsp,0x30`, `rbx` at `[rsp+0x78]`.
2. Set the only registers that path reads: `r13 = [0x34ce030]`, `r14 = this`, `r15 = ctx`.
3. Clear `+0xac`, so the code skips the character-select branch (the only `rsi` reader).
4. Jump to `0x3fa627`. The game's own epilogue returns `al=1` to the match loop.

All anchor bytes are checked first; on a mismatch the experiment is disabled.

**Result (probe run 4, both players modded):**

- **Timing:** accept → `ReceiveCharactersBeginReinitialize` → `OnPreInitBattle`/`OnInitBattle`, HUD back in **0.8 s**, Ready-Fight at +9 s (normal: +45 s).
- **Symptoms:** host desynced; on the guest, P2 controlled both characters; both screens showed "Connecting".
- **Cause 1:** the session layer ran its own rematch flow in parallel: `CHECK_BATTLE_START` → `BATTLE_STAT_HOST` (+9 s) → `C2S_REQUEST_CONNECT` / `MatchedAsHost` → `BATTLE_START_SYNC` → `IDLE` (+14 s). That re-established the battle connection *after* the replayed battle had already started.
- **Cause 2:** the replay skipped `network_setup` (`0x3f8c60`). That is the per-match peer sync (`Load wait` / `Recv TargetBattleReady` / `Sync` / `RTT`, error exits to menu) plus mode-specific online battle setup. It is only called from the `match_loop` prologue (`0x3f48c1`), followed by battle start `0x3f4100`. The offline reset ends with the same battle-start chain (`0x261160`, `0x25fc90`, `0x25fa30/50`, `0x25ff20`, `0x2dd200`), so it supplied only the second half.
- The rematch handshake parameters matched the first match of the same session (seed 59768, stage 40, 2 rounds, 40 s): identical per session.
- The load flag is `NetManager+0x908`: set by `0x3fbac0` (exec loop, before load), cleared in `network_setup` after the peer's `TargetBattleReady`.

### v3 (current): game order, session handshake → `network_setup` → reset → close stale dialog

On a mutual accept, the wrapper does the following:

1. Yields the battle coroutine with the game's frame wait (`0x5463d0(ctx, 1)`, the same call `playermatch_revenge` uses) until the game logs `BATTLE_START_SYNC]->[BATTLE_STAT_IDLE]`. After 30 s it returns the original 0 (normal teardown).
2. Calls `network_setup(this, ctx)`.
3. Runs the v2 reset thunk and returns 1.

Planned budget: handshake (~9–14 s) + sync (~2 s) + reset (0.8 s), instead of 45 s.

**Result (probe run 5, host): works.** The replay ran fully online (live Ping/RTT HUD, `Adjust game RTT`, `SendBattleStartSyncResponsePacket`, all `network_setup` stages clean, no `Error:true`).

| Δ from Accept | Event |
|---|---|
| +14.3 s | `IDLE` |
| +21.0 s | `network_setup` done (RTT measure takes ~5.6 s of it) |
| +21.8 s | reset |
| **+27.2 s** | Ready-Fight (normal: +45 s) |

One leftover: the **"Connecting…" dialog stayed on screen** over the fight. The game has one message box (global `0x34d5218`, slot `+0x58`): `show(id)` = `0x38dde0` (no-op while a dialog is up), `close` = `0x38cfa0`, `done?` = `0x38d350`. On the normal path, `NetSeqSession` closes it at `0x391b98`, right before the next battle (after the old battle terminated and `is_btl_loop`). The in-place replay never reaches that.

**Fix:** after the reset, the wrapper does the same close if the slot is occupied, and logs `message box closed (was open)` / `already empty`. Anchored on `NetSeqSession`'s own `mov rcx,[rip→0x34d5218]; call 0x38cfa0` bytes.

**Probe run 6 (host, several matches, one decline): stable.**

- Two consecutive in-place rematches, both with `message box closed (was open)`.
- Accept → Ready-Fight took **24.3 s** and **26.3 s**.
- The decline logged `rematch [Cancel][Timeout(0)]`. The experiment stayed out of it, and the normal teardown / return to lobby ran.
- No extra battle was started after the in-place ones.

## Release build: ImGui overlay

`dinput8.dll` now has a Dear ImGui (v1.92.9b) overlay. Press **F7** to open it; a 10 s hint appears at startup.

- **`build.bat`** builds the release version: it logs only status and errors to `rematch_mod.log`, starts a fresh file each launch, and has no `ProcessEvent` probe.
- **`build.bat research`** also writes the full game trace, the `NATIVE>/<` match-loop calls and the `UE>` events to the log (appended), as in probe runs 1–6.

- **Toggle:** the "Seamless online rematch" checkbox is saved as `rematch_inplace.on` next to the exe. Both players need the mod with it ON; turn it OFF against an unmodded opponent.
- **Status:** shows the current rematch phase and the last Accept→Ready-Fight time.
- **Unavailable:** the feature shows "Unavailable" (and stays off) when the exe build or any anchor check fails.
- **Hook:** `IDXGISwapChain::Present`/`ResizeBuffers` are patched in the dxgi vtable that every swap chain shares, read from a throwaway WARP device. No detour library is used. Input reaches ImGui through a `WndProc` subclass.
- **Build:** `build.bat` (MinGW, static, ImGui sources in `imgui/`). The DLL imports only stock Windows DLLs: d3d11, D3DCompiler_47, dwmapi, gdi32, kernel32, msvcrt, shell32, user32.

### Startup crash in the first overlay build (user report)

**Report:** `exe stamp 0x63eb1c19`, every check passed, `Overlay ready (F7)` logged, then the game closed a few seconds into startup. Ryzen 5 3600 / GTX 1650, game on E:. Addresses are RVAs on top of the runtime module base, so the drive and ASLR don't matter.

**Cause:** UE4 calls `Present` from more than one thread at startup (RHI thread and loading/movie thread). `HookPresent` had two bugs:

- It set `g_dev` before ImGui was initialized, so a second thread went into `ImGui_ImplDX11_NewFrame` with no backend.
- Nothing stopped two ImGui frames from running at once.

MinGW keeps `assert()` active (no `NDEBUG`), so `IM_ASSERT` called `abort()` and the game was gone.

**Reproduced:** two threads presenting two swap chains (two devices). The old release DLL aborted 3/3 runs (`bd != nullptr … ImGui_ImplDX11_Init` / `Forgot to call Render()`).

**Fix (3/3 survive, overlay on exactly one chain):**

- The overlay runs under a try-lock: a busy thread skips the overlay for that frame and still presents.
- `g_overlayReady` is set only after init.
- It draws only on the adopted swap chain (game-process window), never on other chains or devices.
- `DXGI_PRESENT_TEST` frames are skipped.
- The caller's render targets are restored after drawing.

**Diagnostics:**

- `IM_ASSERT` is routed to the log (`imgui_user_config.h`) instead of `abort()`.
- A vectored exception logger writes `[!] exception … at module+offset … overlay step` (capped at 8).
- `rematch_overlay.off` skips the overlay entirely.

### Second user crash: `msvcrt.dll+0x7ae67`, `0xc0000005`

**Report:** Windows Event Viewer showed the crash in `msvcrt.dll` 7.0.26100.9444 (the game itself doesn't use msvcrt; the mod does).

**Cause:** `msvcrt+0x7ae67` is the stack probe in `__chkstk` (`test [r11], r11b` walking down the stack), so a thread ran out of stack.

- The deep caller is msvcrt's `_write`, which reserves **0x3ca0 bytes (15.5 KB)** of stack to convert line endings for text-mode files.
- Our log file was text mode (`_fsopen`), so every `Log()` → `fflush` paid that cost on whichever game thread was logging.
- The vectored exception logger made it worse: it runs on any thread that hits even a game-handled exception, often deep in that thread's stack.

**Reproduced:** under a minimal Python debugger, a 64 KB-stack thread raises an AV that the DLL's handler logs. The old DLL faulted at **`msvcrt.dll+0x7ae67`**, the same offset as the user's report.

**Fix (the small-stack case now survives 3/3; crash lines are still written on a 1 MB stack):**

- `Log()` uses `CreateFile`/`WriteFile` with a 768-byte buffer and no CRT stdio. The log is now CRLF.
- The crash logger skips threads with under 64 KB of stack left. It reads the stack bottom from TEB+0x1478 (`GetCurrentThreadStackLimits` is Windows 8+), and it no longer touches stack-overflow exceptions.
- In release builds, `GameTrace` only formats the three format strings it needs (prefix match on the format pointer). The other ~850 trace sites return immediately.
- The overlay race test from the first crash still passes 3/3.

### Third user crash: `EXCEPTION_STACK_OVERFLOW` in `gameoverlayrenderer64.dll`

**Report:** `Unhandled Exception: EXCEPTION_STACK_OVERFLOW` in `nvwgf2umx.dll` / `d3d11.dll` / `DINPUT8.dll` with repeating frames of `gameoverlayrenderer64.dll` (Steam overlay).

**Cause:** Steam's `gameoverlayrenderer64.dll` intercepts `IDXGIFactory::CreateSwapChain` and installs an inline detour (`E9 ...`) on the function pointer found at `vt[8]` of the swapchain.

1. In `InstallOverlay()`, a dummy swapchain was created at DLL load time (`DllMain` / early `PatchThread`).
2. Steam's `CreateSwapChain` hook saw the dummy swapchain and installed an inline detour on `dxgi.dll!Present`, saving the trampoline to `GameOverlayRenderer64.dll + 0x167340`.
3. `InstallOverlay()` then overwrote `vt[8]` in DXGI's shared static vtable with `&HookPresent`.
4. Seconds later, Tekken 7 initialized its D3D11 viewport and called `CreateSwapChain` for the real game window.
5. Steam's `CreateSwapChain` hook inspected `vt[8]`. Because `vt[8]` was now `HookPresent` (not `dxgi.dll!Present`), Steam's hook detector (`0x8e980`) saw it as a new unhooked function.
6. Steam installed an inline detour on `HookPresent` and overwrote `0x167340` with a trampoline to `HookPresent`.
7. When `Present()` was called:
   - `sc->Present` entered `vt[8]` (`HookPresent`).
   - Steam's detour at `HookPresent` jumped to Steam.
   - Steam called its trampoline `0x167340` (jumping into `HookPresent`).
   - `HookPresent` executed and called `g_origPresent` (`dxgi.dll!Present`).
   - `dxgi.dll!Present` jumped to Steam (detour from step 2).
   - Steam called its trampoline `0x167340` (jumping back into `HookPresent`).
   - Infinite mutual recursion (25,854 calls logged in ~2s) until stack exhaustion.

**Reproduced:** In `smoke_steam_test.exe` loading `GameOverlayRenderer64.dll`, reproducing the exact 25,854 recursive call loop and `STATUS_STACK_OVERFLOW` (exit code 253).

**Permanent Fix (Engine Call-Site Hook):**
- Discarded all DXGI global vtable patching (`vt[8]` / `vt[13]`), dummy swapchains, and timing sleeps. Modifying DXGI's shared vtable races with UE4's render thread (`FRenderingThread`) creating the D3D11 swapchain.
- Patched the engine's exact `Present` call site directly inside `TekkenGame-Win64-Shipping.exe` at RVA `0xb35bdc` (`FD3D11Viewport::Present`):
  - Original: `48 8B 01 FF 50 40` (`mov rax, [rcx]; call qword ptr [rax+0x40]`, 6 bytes).
  - Patched:  `E8 <rel32> 90` (`call HookPresent; nop`, 6 bytes).
- Arguments match x64 fastcall: `rcx = sc`, `edx = SyncInterval`, `r8d = Flags`.
- `HookPresent` draws ImGui, then calls `(*(Present_t*)(*(void***)sc)[8])(sc, sync, flags)` to invoke the unmodified swapchain vtable (which cleanly chains through Steam overlay and graphics driver).
- Released backbuffer RTV at the end of every frame in `DrawFrame`, eliminating the need to hook `ResizeBuffers`.
- Because DXGI's shared vtable is never modified, Steam's overlay is completely unaware of the hook. Zero recursion, zero race conditions, zero stack overflows.
## Fix: broken floors/walls carried over into the rematch

**Symptom:** after an in-place rematch, floors/walls broken in the previous match stayed broken. Offline VS rematch restores them.

**Cause:** the offline retry handler reloads the stage only when `CBattleManagerConsole+0xa2` is set (`0x3fa9ff`: `call 0x25fa30`, then `call 0x3f8600`). Every post-match handler sets or clears `+0xa2` itself right before use (`0x3fa5a9`, `0x3fae1e`, `0x3fbab0`, `0x3fa472`, …). The v3 thunk jumps past the place where offline sets it, so it was never set.

**`0x3f8600` is "reload current stage":**

1. It passes the current stage name to the stage-manager singleton `0x34d5c58` (`0x422060`). That is the same global its constructor `0x4216c0` stores at `0x42171d`.
2. That singleton's `_reload` task (`0x421830`) is the only native caller of `LevelLoader.ReloadStreamLevel` (thunk `0x572f30`, via `0x260fd0`).
3. Breakable floors/walls are Blueprint actors in the streamed sublevels (`BP_FloorBreak_C`, `TekkenFloorActor`, `TekkenWallActor`). Re-streaming the sublevel restores them.
4. Their `OnResetFloor`/`OnResetWall` events have no native caller (their `FName` globals `0x34d74a0`/`0x34d74b0` are only registered), so there is no lighter native "reset breaks" call to use instead.

**Fix:** the thunk sets `+0xa2 = 1` before entering `0x3fa627`, so the reset runs the same stage reload as offline VS. Smoke-tested against real exe bytes: the reset entry sees `+0xa2=1`, `+0xac=0`, `r13/r14/r15` correct.

**Triage:** 6,412 reflected functions. 5,624 were dropped by name prefilter (floor/wall/break/stage/level/reset/reload/…) and 87 had engine owners, leaving 701 units in 315 distinct names (8 owner lists truncated, 0 errors). The judge sorted them by role: resets_break 41, stage_reload 34, break_event 26, battle_init 35, other 179. 145 were flagged; the top flags were read and traced natively as above.

## "Connecting…" duration (v3, host log)

| Δ from Accept | Event |
|---|---|
| +0 | `REMATCH_CHECK_BATTLE_START` |
| +8.9 s | `BATTLE_STAT_HOST`: both room members published ready (state 7) through **Steam lobby room data** |
| +12.1 s | guest `C2S_REQUEST_CONNECT` |
| +14.3 s | `BATTLE_START_SYNC → IDLE` |
| +15.3 s | `network_setup` load/sync |
| +21.0 s | **RTT measure done (5.6 s)** |
| +21.8 s | reset |

Most of this is the game's own lobby handshake and RTT measurement, not the mod.

- The 180-frame (3 s) branch in `0x373430` (`cmp [this+0x1c], 0B4h`) never applies on a rematch: `[this+0x1c]` is a per-battle frame counter, reset only at `→ IDLE` (`0x371f79`), so it is far past 180 by match end. Patching it would change nothing, so it is not patched.
- The RTT measure (`0x3f9ba0`, ≤600 frames, waits on `0x3a1ac0`) feeds input-delay frames (`Adjust game RTT`). It is not skipped.
- Real cut (future): skip the session rematch handshake when both peers run the mod. This needs a mod-to-mod signal plus suppressing the session's rematch flow, which collided with the in-place battle in v2.

Remaining risks:

- `playermatch_revenge` already ran its accept-path calls (`0x14038cfa0`, `0x14038dde0(…, 0xe)`, `0x1402aad80(1)`) assuming teardown follows.
- After `IDLE`, `NetSeqSession` waits for battle termination before setting up "the next" battle. It may start an extra battle when the in-place one ends.
- Room battle state stays 14 (rematch selecting).
- Mode-specific state set before the reset entry in offline (`mode == 6` → `+0x9b`) is still skipped. `+0xa2` (stage reload) is now set by the thunk.
