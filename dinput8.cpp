// Tekken 7 seamless online rematch (dinput8.dll proxy) + ImGui overlay (F7).
// Build: build.bat (release) / build.bat research (adds full game trace, match-loop and UE probes to the log).
// Pinned to TekkenGame-Win64-Shipping.exe TimeDateStamp 0x63eb1c19; every patch site is byte-verified first.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <share.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#ifndef RESEARCH
#define RESEARCH 0  // release: log only status/errors, fresh file per launch, no UE ProcessEvent probe
#endif

// ---------- proxy export ----------
typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
static DirectInput8Create_t g_pfnDirectInput8Create = NULL;

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, void *punkOuter)
{
    if (!g_pfnDirectInput8Create) {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat_s(sysPath, "\\dinput8.dll");
        HMODULE hReal = LoadLibraryA(sysPath);
        if (hReal) g_pfnDirectInput8Create = (DirectInput8Create_t)GetProcAddress(hReal, "DirectInput8Create");
    }
    return g_pfnDirectInput8Create ? g_pfnDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) : E_FAIL;
}

// ---------- log ----------
// Win32 WriteFile, no CRT stdio: msvcrt's text-mode _write reserves 15.5 KB of stack per flush
// (`mov eax,3CA0h; call __chkstk`), which overflowed game threads near their stack limit
// (user crash at msvcrt.dll+0x7ae67, reproduced). Keep every logging frame small.
static CRITICAL_SECTION g_logLock;
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void Log(const char* fmt, ...) {
    char buf[768];
    int h = _snprintf(buf, sizeof buf, "%lu [%lu:%lu] ", GetTickCount(), GetCurrentProcessId(), GetCurrentThreadId());
    if (h < 0 || h >= (int)sizeof buf) h = 0;
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf(buf + h, sizeof buf - h - 2, fmt, args);
    va_end(args);
    n = (n < 0 || n > (int)sizeof buf - h - 3) ? (int)sizeof buf - h - 3 : n;  // truncated: msvcrt returns -1
    n += h;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == 0)) --n;
    buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_logLock);
    if (g_log == INVALID_HANDLE_VALUE)  // shared: tail it live
        g_log = CreateFileA("rematch_mod.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            RESEARCH ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, n, &wr, NULL);
    LeaveCriticalSection(&g_logLock);
}

// ImGui asserts (imgui_user_config.h) log instead of abort()ing the game.
void ImAssertLog(const char* expr, const char* file, int line) {
    static volatile LONG n;
    if (InterlockedIncrement(&n) <= 20) Log("[-] ImGui assert: %s (%s:%d)", expr, file, line);
}

// Crash breadcrumbs: log fatal exceptions with module+offset so bug reports pinpoint the fault.
// Vectored handlers also see exceptions the game handles itself (first chance, any thread), so: capped, and skipped
// when the thread has under 64 KB of stack left. Stack overflows are not touched (nothing safe to do there).
static const char* volatile* g_crashStep;  // -> overlay step, set once the overlay exists
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    // x64 TEB+0x1478 = DeallocationStack (bottom of the reserved stack); avoids the Win8+ GetCurrentThreadStackLimits
    ULONG_PTR lo = *(ULONG_PTR*)((uint8_t*)NtCurrentTeb() + 0x1478);
    if (!lo || (ULONG_PTR)&code - lo < 64 * 1024) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG count;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    void* pc = e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
        GetModuleFileNameA(mod, name, sizeof name);
    const char* base = strrchr(name, '\\');
    Log("[!] exception 0x%08lx at %s+0x%llx (addr 0x%llx) thread %lu overlay step %s", code, base ? base + 1 : name,
        (unsigned long long)((uint8_t*)pc - (uint8_t*)mod), (unsigned long long)e->ExceptionRecord->ExceptionInformation[1],
        GetCurrentThreadId(), g_crashStep ? *g_crashStep : "-");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Set when the game logs a mutually accepted online rematch (playermatch_revenge, accept path), and when
// the session layer finishes its rematch handshake (battle connection re-established).
static volatile bool g_rematchAccepted, g_sessionIdle;

// Overlay state: what the rematch path is doing, and Accept -> Ready-Fight time of the last rematch.
static const char* volatile g_status = "Idle";
static volatile DWORD g_acceptTick, g_lastRematchMs;

// Replacement for the game's stripped debug printf (shipping build: save args + ret). 852 call sites on many
// threads with format/argument pairs that never ran in retail; release builds format only the three we need.
static void GameTrace(const char* fmt, ...) {
    bool revenge = !strncmp(fmt, "CBattleManagerConsole::playermatch_revenge : rematch [", 54);
    bool battleStat = !strncmp(fmt, "NetInterCS::ChangenBattleStat : ", 32);
    bool readyFight = !strncmp(fmt, "Called Ready-Fight.", 19);
    if (!RESEARCH && !revenge && !battleStat && !readyFight) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf(buf, sizeof buf - 1, fmt, args);
    va_end(args);
    buf[(n < 0 || n >= (int)sizeof buf - 1) ? sizeof buf - 1 : n] = 0;
    if (revenge && !strncmp(buf, "CBattleManagerConsole::playermatch_revenge : rematch [Rematch]", 62)) {
        g_rematchAccepted = true;
        g_acceptTick = GetTickCount();
    }
    if (battleStat && strstr(buf, "[BATTLE_STAT_BATTLE_START_SYNC]->[BATTLE_STAT_IDLE]")) g_sessionIdle = true;
    if (readyFight && g_acceptTick) {
        g_lastRematchMs = GetTickCount() - g_acceptTick;
        g_acceptTick = 0;
        g_status = "Idle";
    }
    if (RESEARCH) Log("%s", buf);
}

// ---------- patch helpers ----------
static uint8_t* g_base;

static void WriteCode(void* at, const void* bytes, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

static uint8_t* FindPattern(uint8_t* base, size_t size, const uint8_t* pattern, const char* mask) {
    size_t len = strlen(mask);
    for (size_t i = 0; i + len <= size; ++i) {
        size_t j = 0;
        while (j < len && (mask[j] == '?' || base[i + j] == pattern[j])) ++j;
        if (j == len) return base + i;
    }
    return NULL;
}

// rel32 calls need a target within +-2GB, so stubs live in an executable page next to the exe.
static uint8_t* g_slots;
static size_t g_slotUsed;

static uint8_t* NearAlloc(size_t n) {
    if (!g_slots) {
        uintptr_t lo = (uintptr_t)g_base > 0x7FF00000 ? (uintptr_t)g_base - 0x7FF00000 : 0x10000;
        for (uintptr_t a = (uintptr_t)g_base - 0x10000; a > lo; a -= 0x10000)
            if ((g_slots = (uint8_t*)VirtualAlloc((void*)a, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) break;
        if (!g_slots) return NULL;
    }
    if (g_slotUsed + n > 0x1000) return NULL;
    uint8_t* s = g_slots + g_slotUsed;
    g_slotUsed += (n + 15) & ~(size_t)15;
    return s;
}

static uint8_t* NearJump(void* target) {
    uint8_t* s = NearAlloc(14);
    if (!s) return NULL;
    s[0] = 0xFF; s[1] = 0x25; memset(s + 2, 0, 4);  // jmp [rip+0]
    memcpy(s + 6, &target, 8);
    return s;
}

// Redirect `call rel32` at rva to hook; returns the original callee or NULL if the site doesn't match.
static void* PatchCall(uint32_t rva, uint32_t expectRva, void* hook) {
    uint8_t* site = g_base + rva;
    if (site[0] != 0xE8 || site + 5 + *(int32_t*)(site + 1) != g_base + expectRva) return NULL;
    uint8_t* slot = NearJump(hook);
    if (!slot) return NULL;
    int32_t rel = (int32_t)(slot - (site + 5));
    WriteCode(site + 1, &rel, 4);
    return g_base + expectRva;
}

// ---------- native probe: CBattleManagerConsole match loop ----------
// match loop 0x3f4870 (non-zero = replay in place, zero = term()); play_match returns end type 1..8;
// post-match handlers return non-zero to replay in place. end type 7 = playermatch_revenge (online).
// Everything but play_match returns bool in al; the upper bits of rax are garbage.
typedef uint64_t (*Fn4)(uint64_t, uint64_t, uint64_t, uint64_t);
struct Site { uint32_t rva, target; const char* name; Fn4 orig; };
static Site g_sites[] = {
    {0x3f8353, 0x3f4870, "match_loop", NULL},
    {0x3f49e1, 0x3f4c40, "play_match(end_type)", NULL},
    {0x3f4ae1, 0x3f8510, "end1_handler", NULL},
    {0x3f4af1, 0x3f3210, "end2_handler", NULL},
    {0x3f4b04, 0x3fb4b0, "end3_handler", NULL},
    {0x3f4b17, 0x3fb4b0, "end4_handler", NULL},
    {0x3f4b27, 0x3fa310, "end5_handler", NULL},
    {0x3f4b37, 0x3fb1e0, "end6_handler", NULL},
    {0x3f4b47, 0x3f9d80, "end7_playermatch_revenge", NULL},
    {0x3f4b57, 0x3fac40, "end8_handler", NULL},
};
static const int SITE_PLAY_MATCH = 1, SITE_REVENGE = 8;

// Experiment v3 (both players need it): on a mutually accepted online rematch, replay in place in the
// game's own per-match order instead of term() + reload. Off unless rematch_inplace.on sits next to the exe.
//   1. wait (game frame-wait on the battle coroutine) for the session's rematch handshake ->BATTLE_STAT_IDLE;
//      timeout -> leave the original 0 (normal teardown path)
//   2. network_setup(this, ctx) 0x3f8c60: per-match peer sync + online battle setup (match_loop prologue)
//   3. offline retry handler's "rematch, same characters" reset (0x3fa627..0x3fac1e, ends `mov al,1`):
//      reinit characters + battle start
// The step-3 thunk rebuilds 0x3fa310's frame (same pushes, sub rsp 0x30, rbx at [rsp+0x78]) and sets the
// only registers that path reads: r13 = [0x34ce030], r14 = this, r15 = ctx; +0xac = 0 keeps it off the
// character-select branch (the only rsi reader).
static const uint32_t RVA_RETRY_HANDLER = 0x3fa310, RVA_RETRY_RESET = 0x3fa627, RVA_RETRY_GLOBAL = 0x34ce030;
static const uint32_t RVA_NETWORK_SETUP = 0x3f8c60, RVA_WAIT_FRAMES = 0x5463d0;
// Message box (one dialog slot at +0x58). NetSeqSession closes it at 0x391b98 right before starting the next
// battle; the in-place replay never reaches that, so "Connecting..." stays up. We do the same close.
static const uint32_t RVA_MSGBOX = 0x34d5218, RVA_MSGBOX_CLOSE = 0x38cfa0, RVA_NETSEQ_CLOSE_SITE = 0x391b98;
static const int SESSION_WAIT_FRAMES = 60 * 30;  // ponytail: fixed 30 s timeout, tune if handshakes run longer
static const char* const INPLACE_FILE = "rematch_inplace.on";  // persisted toggle (overlay creates/deletes it)
static bool g_inplaceReady;          // all anchors verified, thunk built
static volatile bool g_inplace;      // user toggle (overlay / file)
static Fn4 g_retryReset, g_networkSetup, g_waitFrames, g_msgboxClose;

static bool BuildRetryThunk() {
    const uint8_t prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30,
                                0x4C, 0x8B, 0x2D};  // + mov r13, [rip+x] (r13 load at 0x3fa324)
    const uint8_t entry[] = {0x49, 0x8B, 0xCD, 0xE8};  // mov rcx, r13; call 0x2616f0
    const uint8_t tail[] = {0xB0, 0x01, 0xEB, 0x02, 0x32, 0xC0, 0x48, 0x8B, 0x5C, 0x24, 0x78, 0x48, 0x83, 0xC4, 0x30,
                            0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0xC3};
    const uint8_t nsHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                              0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x01, 0, 0};
    const uint8_t waitHead[] = {0x48, 0x8B, 0x49, 0x28, 0x89, 0x51, 0x18, 0x48, 0x83, 0xC1, 0x08, 0xE9};  // ctx->[+28h].count = n; yield
    uint8_t* h = g_base + RVA_RETRY_HANDLER;
    if (memcmp(h, prologue, sizeof prologue) || h + 0x1b + *(int32_t*)(h + 0x17) != g_base + RVA_RETRY_GLOBAL ||
        memcmp(g_base + RVA_RETRY_RESET, entry, sizeof entry) || memcmp(g_base + 0x3fac1e, tail, sizeof tail) ||
        memcmp(g_base + RVA_NETWORK_SETUP, nsHead, sizeof nsHead) || memcmp(g_base + RVA_WAIT_FRAMES, waitHead, sizeof waitHead))
        return false;
    // anchor: NetSeqSession's own close = `mov rcx, [rip -> msgbox]; call close`
    uint8_t* cs = g_base + RVA_NETSEQ_CLOSE_SITE;
    if (cs[0] != 0x48 || cs[1] != 0x8B || cs[2] != 0x0D || cs + 7 + *(int32_t*)(cs + 3) != g_base + RVA_MSGBOX ||
        cs[7] != 0xE8 || cs + 12 + *(int32_t*)(cs + 8) != g_base + RVA_MSGBOX_CLOSE)
        return false;
    g_networkSetup = (Fn4)(g_base + RVA_NETWORK_SETUP);
    g_waitFrames = (Fn4)(g_base + RVA_WAIT_FRAMES);
    g_msgboxClose = (Fn4)(g_base + RVA_MSGBOX_CLOSE);

    uint8_t code[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10,                                // mov [rsp+10h], rbx
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push rbp,rsi,rdi,r12..r15
        0x48, 0x83, 0xEC, 0x30,                                      // sub rsp, 30h
        0x49, 0x89, 0xCE,                                            // mov r14, rcx
        0x49, 0x89, 0xD7,                                            // mov r15, rdx
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                          // mov rax, &global
        0x4C, 0x8B, 0x28,                                            // mov r13, [rax]
        0x41, 0xC6, 0x86, 0xAC, 0, 0, 0, 0,                          // mov byte [r14+0ACh], 0
        0x41, 0xC6, 0x86, 0xA2, 0, 0, 0, 1,                          // mov byte [r14+0A2h], 1: reload stage (0x3f8600) like offline VS
        0x31, 0xF6,                                                  // xor esi, esi
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                          // mov rax, reset entry
        0xFF, 0xE0,                                                  // jmp rax (returns to our caller, al = 1)
    };
    uint8_t* glob = g_base + RVA_RETRY_GLOBAL;
    uint8_t* reset = g_base + RVA_RETRY_RESET;
    memcpy(code + 28, &glob, 8);
    memcpy(code + 59, &reset, 8);
    uint8_t* t = NearAlloc(sizeof code);
    if (!t) return false;
    memcpy(t, code, sizeof code);
    FlushInstructionCache(GetCurrentProcess(), t, sizeof code);
    g_retryReset = (Fn4)t;
    return true;
}

template <int N>
static uint64_t Wrap(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (RESEARCH) Log("NATIVE> %s enter", g_sites[N].name);
    if (N == SITE_REVENGE) g_rematchAccepted = g_sessionIdle = false;
    uint64_t r = g_sites[N].orig(a, b, c, d);
    if (RESEARCH) Log("NATIVE< %s -> %u", g_sites[N].name, N == SITE_PLAY_MATCH ? (uint32_t)r : (uint32_t)(uint8_t)r);
    if (N == SITE_REVENGE && g_inplaceReady && g_inplace && g_rematchAccepted && !(uint8_t)r) {
        Log("EXPERIMENT: rematch accepted -> waiting for session handshake");
        g_status = "Waiting for session handshake";
        int f = 0;
        while (!g_sessionIdle && f < SESSION_WAIT_FRAMES) { g_waitFrames(b, 1, 0, 0); ++f; }
        if (!g_sessionIdle) {
            Log("EXPERIMENT: no session handshake after %d frames -> normal teardown", f);
            g_status = "Handshake timeout: normal rematch";
            return r;
        }
        Log("EXPERIMENT: session idle after %d frames -> network_setup", f);
        g_status = "Syncing with opponent";
        g_networkSetup(a, b, 0, 0);
        Log("EXPERIMENT: network_setup done -> offline retry reset");
        g_status = "Restarting in place";
        r = g_retryReset(a, b, c, d);
        Log("EXPERIMENT: retry reset -> %u (match loop replays in place)", (uint32_t)(uint8_t)r);
        uint8_t* mb = *(uint8_t**)(g_base + RVA_MSGBOX);
        bool open = mb && *(void**)(mb + 0x58);
        if (open) g_msgboxClose((uint64_t)mb, 0, 0, 0);
        Log("EXPERIMENT: message box %s", open ? "closed (was open)" : "already empty");
    }
    return r;
}
static Fn4 g_wraps[] = {Wrap<0>, Wrap<1>, Wrap<2>, Wrap<3>, Wrap<4>, Wrap<5>, Wrap<6>, Wrap<7>, Wrap<8>, Wrap<9>};
static_assert(sizeof g_wraps / sizeof *g_wraps == sizeof g_sites / sizeof *g_sites, "one wrapper per site");

// ---------- UE probe: UObject::ProcessEvent ----------
// Layout from UnrealModLoader EngineInfo + disassembly (UE 4.13 fork):
//   UObject: Name(FName) +0x18, Outer +0x20. GNames global -> {FNameEntry** chunks[128]; int32 num @+0x400},
//   16384 entries per chunk; FNameEntry: Index @0 (bit0 = wide), name @+0x10.
// ponytail: ProcessEvent only sees C++->Blueprint calls (Receive*/On* events), not Blueprint->native calls.
static const uint32_t RVA_PROCESS_EVENT = 0x936b80, RVA_PE_DIRECT_CALL = 0x12be28f;
static const uint32_t RVA_GNAMES = 0x368AF60, RVA_GOBJECTS = 0x3693550;

typedef void (*ProcessEvent_t)(void*, void*, void*);
static ProcessEvent_t g_origPE;

static bool SafeRead(const void* addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

static std::string NameOf(int32_t idx) {
    uint8_t* names;
    if (idx < 0 || !SafeRead(g_base + RVA_GNAMES, &names, 8) || !names) return "?";
    uint8_t* chunk; uint8_t* entry; int32_t flags; char buf[128] = {0};
    if (!SafeRead(names + (idx / 16384) * 8, &chunk, 8) || !chunk) return "?";
    if (!SafeRead(chunk + (idx % 16384) * 8, &entry, 8) || !entry) return "?";
    if (!SafeRead(entry, &flags, 4)) return "?";
    if (flags & 1) {  // wide name
        wchar_t w[128] = {0};
        SafeRead(entry + 0x10, w, sizeof w - 2);
        for (int i = 0; i < 127 && w[i]; ++i) buf[i] = (char)w[i];
    } else {
        SafeRead(entry + 0x10, buf, sizeof buf - 1);
    }
    return buf;
}

static std::string ObjName(void* obj) {
    int32_t idx;
    return obj && SafeRead((uint8_t*)obj + 0x18, &idx, 4) ? NameOf(idx) : "null";
}

static std::string OuterName(void* obj) {
    void* outer;
    return obj && SafeRead((uint8_t*)obj + 0x20, &outer, 8) ? ObjName(outer) : "null";
}

static const std::unordered_set<std::string> g_watch = {
    // level streaming
    "LoadStreamLevel", "UnloadStreamLevel", "ReloadStreamLevel", "LoadRequestedLevel",
    "RequestSwitchLevels", "SwitchLevels", "PostSwitchLevels", "OnReplayReloadedStage",
    "RequestTransition", "OnTransitionRequested", "OnTransitionCompleted",
    // characters / battle actors
    "ReceiveCharactersBeginLoad", "ReceiveCharactersBeginUnload", "ReceiveCharactersBeginReinitialize",
    "AsyncLoadCharacters", "AsyncLoadCharacter", "LoadCharacter", "ReinitializeCharacter",
    "SpawnBattleActors", "AsyncSpawnBattleActors", "DestroyBattleActors", "CleanUpItemsInLevel",
    // game mode flags
    "SetLevelLoadCompletedFlag", "SetLevelReLoadCompletedFlag", "SetReinitCharCompletedFlag",
    "SetSecretlySwitchingStageFlag", "SetLevelVisiblingFlag", "SetLevelLoadCancelFlag",
    // battle lifecycle events
    "OnPreInitBattle", "OnInitBattle", "OnUnloadLevel", "OnBattleEnd", "OnRoundSettled",
};

static SRWLOCK g_cacheLock = SRWLOCK_INIT;
static std::unordered_map<void*, std::string*> g_cache;  // UFunction* -> "Outer.Func" or NULL
static bool g_namesChecked, g_namesOk;

static std::string* Watched(void* fn) {
    AcquireSRWLockShared(&g_cacheLock);
    auto it = g_cache.find(fn);
    bool hit = it != g_cache.end();
    std::string* v = hit ? it->second : NULL;
    ReleaseSRWLockShared(&g_cacheLock);
    if (hit) return v;

    std::string name = ObjName(fn);
    v = g_watch.count(name) ? new std::string(OuterName(fn) + "." + name) : NULL;
    AcquireSRWLockExclusive(&g_cacheLock);
    g_cache.emplace(fn, v);
    ReleaseSRWLockExclusive(&g_cacheLock);
    return v;
}

static void HookPE(void* self, void* fn, void* parms) {
    if (!g_namesChecked) {  // first call: GObjects[0] must be the CoreUObject package ("/Script/CoreUObject")
        uint8_t* objs; void* obj0 = NULL;
        std::string n0 = SafeRead(g_base + RVA_GOBJECTS + 0x10, &objs, 8) && SafeRead(objs, &obj0, 8) ? ObjName(obj0) : "?";
        g_namesOk = n0.size() >= 11 && n0.compare(n0.size() - 11, 11, "CoreUObject") == 0;
        g_namesChecked = true;
        Log(g_namesOk ? "[+] UE name table verified (GObjects[0]=%s)" : "[-] UE name table check failed (got '%s'); UE probe off", n0.c_str());
    }
    if (g_namesOk) {
        std::string* w = Watched(fn);
        if (w) {
            uint8_t p[8] = {0};
            if (parms) SafeRead(parms, p, 8);
            Log("UE> %s self=%s parms=%02x%02x%02x%02x%02x%02x%02x%02x", w->c_str(), ObjName(self).c_str(), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }
    g_origPE(self, fn, parms);
}

static void InstallUEProbe(PIMAGE_SECTION_HEADER rdata) {
    const uint8_t peHead[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0, 0, 0};
    if (memcmp(g_base + RVA_PROCESS_EVENT, peHead, sizeof peHead) != 0) { Log("[-] ProcessEvent bytes mismatch; UE probe off"); return; }
    g_origPE = (ProcessEvent_t)(g_base + RVA_PROCESS_EVENT);

    void* hook = (void*)&HookPE;
    uint64_t* p = (uint64_t*)(g_base + rdata->VirtualAddress);
    size_t n = rdata->Misc.VirtualSize / 8, patched = 0;
    for (size_t i = 0; i < n; ++i)
        if (p[i] == (uint64_t)g_origPE) { WriteCode(&p[i], &hook, 8); ++patched; }
    bool direct = PatchCall(RVA_PE_DIRECT_CALL, RVA_PROCESS_EVENT, hook) != NULL;
    Log("[+] ProcessEvent hooked: %zu vtable slots, direct call %s", patched, direct ? "ok" : "MISSING");
}

// ---------- ImGui overlay (F7) ----------
// Hooks IDXGISwapChain::Present/ResizeBuffers through the vtable shared by every swap chain dxgi creates
// (read from a throwaway device), so no detour library. Toggle persists as rematch_inplace.on.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain*, UINT, UINT);
static IDXGISwapChain* g_sc;           // the one swap chain the overlay draws on (first game window presented)
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static WNDPROC g_origWndProc;
static volatile bool g_menu;
static DWORD g_hintUntil;
static volatile bool g_overlayReady;  // set only after ImGui is fully initialized
static bool g_overlayFailed;
// UE4 can call Present from more than one thread (RHI thread, loading-screen thread); ImGui is single-threaded.
static SRWLOCK g_overlayLock = SRWLOCK_INIT;
static const char* volatile g_overlayStep = "none";  // breadcrumb for the crash log

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_F7 && !(lp & (1 << 30))) g_menu = !g_menu;
    if (g_menu && ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    return CallWindowProcW(g_origWndProc, h, msg, wp, lp);
}

static void SetInplace(bool on) {
    g_inplace = on;
    if (on) { FILE* f = fopen(INPLACE_FILE, "w"); if (f) fclose(f); }
    else DeleteFileA(INPLACE_FILE);
    Log("[*] Seamless rematch %s (overlay)", on ? "ON" : "off");
}

static void DrawOverlay() {
    if (g_menu) {
        ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        bool open = true;
        ImGui::Begin("Tekken 7 Seamless Rematch  [F7]", &open, ImGuiWindowFlags_AlwaysAutoResize);
        if (!open) g_menu = false;
        if (!g_inplaceReady) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Unavailable: unsupported game build or patch check failed.");
            ImGui::TextUnformatted("See rematch_mod.log next to the game exe.");
        } else {
            bool on = g_inplace;
            if (ImGui::Checkbox("Seamless online rematch", &on)) SetInplace(on);
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Both players need this mod with this ON.");
            ImGui::TextUnformatted("Opponent without the mod: turn it OFF, or the rematch desyncs.");
            ImGui::Separator();
            ImGui::Text("Status: %s", g_status);
            if (g_lastRematchMs) ImGui::Text("Last rematch: %.1f s from accept to fight", g_lastRematchMs / 1000.0f);
        }
        ImGui::End();
    } else if (GetTickCount() < g_hintUntil) {
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        ImGui::Text("Seamless Rematch: %s   (F7 for menu)", !g_inplaceReady ? "unavailable" : g_inplace ? "ON" : "off");
        ImGui::End();
    }
}

static void CreateRtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static bool InitOverlay(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &pid);
    if (pid != GetCurrentProcessId()) return false;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return false;
    g_overlayStep = "init";
    g_dev = dev;
    g_dev->GetImmediateContext(&g_ctx);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    if (!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(g_dev, g_ctx)) {
        Log("[-] Overlay: ImGui backend init failed; overlay off");
        g_overlayFailed = true;
        return false;
    }
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(desc.OutputWindow, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
    g_sc = sc;
    g_hintUntil = GetTickCount() + 10000;
    g_overlayReady = true;
    Log("[+] Overlay ready (F7)");
    return true;
}

static void DrawFrame(IDXGISwapChain* sc) {
    if (!g_overlayReady && !g_overlayFailed && !InitOverlay(sc)) return;
    if (!g_overlayReady || sc != g_sc) return;  // other swap chains / devices: never touch
    if (!g_rtv) { g_overlayStep = "rtv"; CreateRtv(sc); }
    g_overlayStep = "newframe";
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_menu;
    g_overlayStep = "draw";
    DrawOverlay();
    ImGui::Render();
    if (g_rtv) {
        g_overlayStep = "render";
        ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
         for (auto* v : oldRtv) if (v) v->Release();
         if (oldDsv) oldDsv->Release();
        g_rtv->Release();
        g_rtv = nullptr;
     }
    g_overlayStep = "present";
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST) && TryAcquireSRWLockExclusive(&g_overlayLock)) {
        DrawFrame(sc);
        ReleaseSRWLockExclusive(&g_overlayLock);
    }
    void** vt = *(void***)sc;
    Present_t realPresent = (Present_t)vt[8];
    return realPresent(sc, sync, flags);
}

static void InstallOverlay() {
    uint8_t* site = g_base + 0xb35bdc;
    const uint8_t expect[] = {0x48, 0x8B, 0x01, 0xFF, 0x50, 0x40};
    if (memcmp(site, expect, sizeof expect)) {
        Log("[-] Present call site mismatch (0xb35bdc); overlay off");
        return;
    }
    uint8_t* slot = NearJump((void*)&HookPresent);
    if (!slot) {
        Log("[-] NearJump failed for HookPresent; overlay off");
        return;
    }
    int32_t rel = (int32_t)(slot - (site + 5));
    uint8_t patch[6] = {0xE8, 0, 0, 0, 0, 0x90};
    memcpy(patch + 1, &rel, 4);
    WriteCode(site, patch, sizeof patch);
    Log("[+] Overlay hooked via engine Present call site (0xb35bdc)");
}

// ---------- startup ----------
static DWORD WINAPI PatchThread(LPVOID) {
    Sleep(100);
    g_base = (uint8_t*)GetModuleHandleA(NULL);
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(g_base + ((PIMAGE_DOS_HEADER)g_base)->e_lfanew);
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt), text = NULL, rdata = NULL;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!memcmp(sec[i].Name, ".text", 6)) text = &sec[i];
        if (!memcmp(sec[i].Name, ".rdata", 7)) rdata = &sec[i];
    }
    Log("[*] Tekken 7 seamless rematch loaded (%s build, exe stamp 0x%08lx)", RESEARCH ? "research" : "release", nt->FileHeader.TimeDateStamp);
    if (!text || !rdata) { Log("[-] sections not found"); return 0; }
    if (nt->FileHeader.TimeDateStamp != 0x63eb1c19) { Log("[-] unknown exe build; native/UE probes off"); }

    // Game trace: stub bytes occur twice in .text, so resolve it through the unique is_btl_loop call site.
    uint8_t* textBase = g_base + text->VirtualAddress;
    const uint8_t patCall[] = {0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x84, 0xDB, 0x74, 0x78};
    const uint8_t stubBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0xC3};
    uint8_t* cs = FindPattern(textBase, text->Misc.VirtualSize, patCall, "xxx????x????xxxx");
    uint8_t* stub = cs ? cs + 12 + *(int32_t*)(cs + 8) : NULL;
    if (stub && !memcmp(stub, stubBytes, sizeof stubBytes)) {
        uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};
        void* target = (void*)&GameTrace;
        memcpy(jmp + 6, &target, 8);
        WriteCode(stub, jmp, sizeof jmp);
        Log("[+] Game trace hooked");
    } else {
        Log("[-] Game trace stub not found");
    }

    if (nt->FileHeader.TimeDateStamp == 0x63eb1c19) {
        for (size_t i = 0; i < sizeof g_sites / sizeof *g_sites; ++i) {
            g_sites[i].orig = (Fn4)PatchCall(g_sites[i].rva, g_sites[i].target, (void*)g_wraps[i]);
            if (!g_sites[i].orig) Log("[-] native site %s mismatch, skipped", g_sites[i].name);
        }
        g_inplaceReady = g_sites[SITE_REVENGE].orig && BuildRetryThunk();
        g_inplace = g_inplaceReady && GetFileAttributesA(INPLACE_FILE) != INVALID_FILE_ATTRIBUTES;
        if (!g_inplaceReady) Log("[-] seamless rematch anchors mismatch; feature unavailable");
        Log("[+] Seamless rematch %s", g_inplace ? "ON" : "off");
        if (RESEARCH) InstallUEProbe(rdata);
        g_crashStep = &g_overlayStep;
        if (GetFileAttributesA("rematch_overlay.off") == INVALID_FILE_ATTRIBUTES) InstallOverlay();
        else Log("[*] Overlay disabled (rematch_overlay.off); seamless rematch follows rematch_inplace.on");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_logLock);
        AddVectoredExceptionHandler(0, CrashLogger);
        DisableThreadLibraryCalls(hinstDLL);
        CreateThread(NULL, 0, PatchThread, NULL, 0, NULL);
    }
    return TRUE;
}
