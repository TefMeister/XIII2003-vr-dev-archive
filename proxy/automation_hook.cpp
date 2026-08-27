// proxy/automation_hook.cpp
//
// See automation_hook.h for the mechanism and for why dispatch moved off the
// render path in 0.2.9. Exported symbols this relies on, each resolved from
// the module that actually defines it -- looking in the wrong one returns
// null SILENTLY, which is how 0.2.8's first draft nearly shipped half-working:
//
//   Engine.dll  ?GEngine@@3PAVUEngine@@A                -- UEngine* GEngine
//   Engine.dll  ?Exec@UGameEngine@@UAEHPBDAAVFOutputDevice@@@Z
//   Engine.dll  ?Tick@APlayerController@@UAEHMW4ELevelTick@@@Z
//   Core.dll    ?ScriptConsoleExec@UObject@@UAEHPBDAAVFOutputDevice@@PAV1@@Z
//   Core.dll    ?GLog@@3PAVFOutputDevice@@A              -- FOutputDevice* GLog
//
// The mangled names decode to ANSI char* (PBD), so this build of the engine
// is not Unicode and commands pass as plain char*.

#include <windows.h>
#include <cstdio>
#include <string>
#include "automation_hook.h"
#include "automation_policy.h"

static void Log(const char* m) {
    OutputDebugStringA("[xiii-auto] "); OutputDebugStringA(m); OutputDebugStringA("\n");
}

// UGameEngine::Exec(const char*, FOutputDevice&) -- thiscall as fastcall.
typedef int(__fastcall* Exec_t)(void* self, void* edx, const char* cmd, void* ar);
// UObject::ScriptConsoleExec(const char*, FOutputDevice&, UObject* Executor).
typedef int(__fastcall* ScriptConsoleExec_t)(void* self, void* edx, const char* cmd,
                                             void* ar, void* executor);
// APlayerController::Tick(FLOAT DeltaSeconds, ELevelTick TickType) -> UBOOL.
typedef int(__fastcall* PCTick_t)(void* self, void* edx, float delta, int tickType);

// APlayerController::Tick prologue we relocate: `push ebp; mov ebp,esp;
// push -1`. Exactly 5 bytes with no relative operands, so it copies verbatim
// into the trampoline and a jmp rel32 fits with no NOP padding.
static const uint8_t kPCTickPrologue[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
static const size_t  kPCTickStealLen   = sizeof(kPCTickPrologue);  // 5

static bool                s_enabled       = false;
static Exec_t              s_engineExec    = nullptr;
static ScriptConsoleExec_t s_scriptExec    = nullptr;
static PCTick_t            s_pcTickTramp   = nullptr;
static void**              s_gEngine       = nullptr;  // &GEngine
static void**              s_gLog          = nullptr;  // &GLog
static bool                s_engineExecOk  = false;    // tier-2 opt-in
static DWORD               s_pollMs        = 200;
static DWORD               s_telemetryMs   = 1000;
static DWORD               s_lastPoll      = 0;
static DWORD               s_lastTelemetry = 0;
static uint64_t            s_tick          = 0;
static LONG                s_inCommand     = 0;  // re-entrancy guard
static char                s_cmdPath[MAX_PATH] = {0};
static char                s_logPath[MAX_PATH] = {0};

static void PathNextToExe(const char* leaf, char* out) {
    if (!GetModuleFileNameA(nullptr, out, MAX_PATH)) { out[0] = 0; return; }
    char* slash = strrchr(out, '\\');
    if (!slash) { out[0] = 0; return; }
    lstrcpyA(slash + 1, leaf);
}

static UINT ReadVrInt(const char* key, UINT def) {
    char ini[MAX_PATH];
    PathNextToExe("XIII.ini", ini);
    if (!ini[0]) return def;
    return (UINT)GetPrivateProfileIntA("VR", key, (INT)def, ini);
}

// Opened and closed per write so the file is always flush-safe -- the 0.2.8
// crash was diagnosed entirely from what had and had not reached this file,
// which only works because every line is on disk before the next call runs.
static void AppendLog(const char* line) {
    if (!s_logPath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, s_logPath, "a") != 0 || !f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

// Guards against handing the engine a pointer it will dereference blindly.
// GLog is assigned during engine init; if it were ever null or stale, the
// engine's own "unrecognized command" print would fault inside Exec with no
// output to show for it -- indistinguishable from the 0.2.8 crash.
static bool IsReadablePtr(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) return false;
    const BYTE* regionEnd = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
    return ((const BYTE*)p + n) <= regionEnd;
}

// A live FOutputDevice: the object itself readable, and its vtable pointer
// pointing at readable memory.
static void* ValidOutputDevice() {
    if (!s_gLog) return nullptr;
    void* ar = *s_gLog;
    if (!IsReadablePtr(ar, sizeof(void*))) return nullptr;
    void* vtbl = *(void**)ar;
    if (!IsReadablePtr(vtbl, sizeof(void*) * 4)) return nullptr;
    return ar;
}

static std::string ReadAndClearCommandFile() {
    FILE* f = nullptr;
    if (fopen_s(&f, s_cmdPath, "r") != 0 || !f) return std::string();
    std::string contents;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) contents.append(buf, n);
    fclose(f);
    if (!contents.empty()) {
        // Truncate so each command runs exactly once. Deliberately done
        // BEFORE execution: a line still in the file proves it never ran.
        if (fopen_s(&f, s_cmdPath, "w") == 0 && f) fclose(f);
    }
    return contents;
}

static void RunCommand(void* playerController, const std::string& cmd) {
    void* ar = ValidOutputDevice();
    if (!ar) {
        AppendLog("SKIP: GLog is not a valid FOutputDevice");
        Log("GLog is not a valid FOutputDevice; skipping command");
        return;
    }

    char line[640];
    // Logged BEFORE the call, flushed, so a fault names the command and the
    // tier that was in flight. 0.2.8 logged only on completion, so its crash
    // left no record of which call died and it had to be inferred.
    _snprintf_s(line, sizeof(line), _TRUNCATE, "exec BEGIN player \"%s\"", cmd.c_str());
    AppendLog(line);

    int handled = 0;
    const char* by = "unhandled";
    if (playerController && s_scriptExec) {
        handled = s_scriptExec(playerController, nullptr, cmd.c_str(), ar, playerController);
        if (handled) by = "playercontroller";
    }

    if (!handled && s_engineExecOk && s_engineExec && s_gEngine &&
        IsReadablePtr(s_gEngine, sizeof(void*)) && *s_gEngine) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "exec BEGIN engine \"%s\"", cmd.c_str());
        AppendLog(line);
        handled = s_engineExec(*s_gEngine, nullptr, cmd.c_str(), ar);
        if (handled) by = "engine";
    }

    _snprintf_s(line, sizeof(line), _TRUNCATE, "exec END   \"%s\" -> %s", cmd.c_str(), by);
    AppendLog(line);
    Log(line);
}

// Drain the queue. Runs from APlayerController::Tick -- game-logic phase,
// NOT the render path.
static void DrainCommands(void* playerController) {
    if (!s_enabled || !s_cmdPath[0]) return;
    const DWORD now = GetTickCount();
    // GetTickCount wraps every ~49 days; unsigned subtraction stays correct.
    if (now - s_lastPoll < s_pollMs) return;
    s_lastPoll = now;

    // A command that itself ticks the player must not re-enter the drain.
    if (InterlockedCompareExchange(&s_inCommand, 1, 0) != 0) return;
    std::string contents = ReadAndClearCommandFile();
    if (!contents.empty())
        for (const std::string& cmd : xiii::ParseCommandFile(contents))
            RunCommand(playerController, cmd);
    InterlockedExchange(&s_inCommand, 0);
}

static int __fastcall Hook_PCTick(void* self, void* edx, float delta, int tickType) {
    const int r = s_pcTickTramp(self, edx, delta, tickType);
    DrainCommands(self);
    return r;
}

void AutomationTelemetryTick(const float* cameraLocation,
                             const int32_t* cameraRotation) {
    if (!s_enabled || !s_telemetryMs) return;
    ++s_tick;
    const DWORD now = GetTickCount();
    if (now - s_lastTelemetry < s_telemetryMs) return;
    s_lastTelemetry = now;

    xiii::TelemetrySample s{};
    s.tick     = s_tick;
    s.uptimeMs = now;
    if (cameraLocation) { s.x = cameraLocation[0]; s.y = cameraLocation[1]; s.z = cameraLocation[2]; }
    if (cameraRotation) { s.pitch = cameraRotation[0]; s.yaw = cameraRotation[1]; s.roll = cameraRotation[2]; }
    AppendLog(xiii::FormatTelemetryLine(s).c_str());
}

// Minimal 5-byte-jmp inline hook with a hand-built trampoline, same shape as
// camera_hook.cpp. The prologue is verified before patching, so a version
// mismatch fails safe instead of crashing.
static bool InstallPCTickHook(HMODULE hEngine) {
    BYTE* target = (BYTE*)GetProcAddress(
        hEngine, "?Tick@APlayerController@@UAEHMW4ELevelTick@@@Z");
    if (!target) { Log("APlayerController::Tick export not found"); return false; }

    for (size_t i = 0; i < kPCTickStealLen; ++i)
        if (target[i] != kPCTickPrologue[i]) {
            Log("APlayerController::Tick prologue mismatch; not hooking");
            return false;
        }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, kPCTickStealLen + 5,
                                      MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!tramp) { Log("trampoline alloc failed"); return false; }
    memcpy(tramp, target, kPCTickStealLen);
    tramp[kPCTickStealLen] = 0xE9;  // jmp rel32
    *(int32_t*)(tramp + kPCTickStealLen + 1) =
        (int32_t)((target + kPCTickStealLen) - (tramp + kPCTickStealLen + 5));
    s_pcTickTramp = (PCTick_t)tramp;

    DWORD oldProt = 0;
    if (!VirtualProtect(target, kPCTickStealLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        Log("VirtualProtect failed"); return false;
    }
    target[0] = 0xE9;
    *(int32_t*)(target + 1) = (int32_t)((BYTE*)&Hook_PCTick - (target + 5));
    DWORD tmp = 0;
    VirtualProtect(target, kPCTickStealLen, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), target, kPCTickStealLen);
    return true;
}

void InstallAutomationHook() {
    if (ReadVrInt("Automation", 0) == 0) return;

    s_pollMs       = ReadVrInt("AutomationPollMs", 200);
    s_telemetryMs  = ReadVrInt("AutomationTelemetryMs", 1000);
    // Tier 2 is the call that faulted in 0.2.8. Off unless asked for.
    s_engineExecOk = ReadVrInt("AutomationEngineExec", 0) != 0;

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    HMODULE hCore   = GetModuleHandleA("Core.dll");
    if (!hEngine || !hCore) {
        Log("Engine.dll/Core.dll not loaded; automation inert"); return;
    }

    s_gEngine    = (void**)GetProcAddress(hEngine, "?GEngine@@3PAVUEngine@@A");
    s_engineExec = (Exec_t)GetProcAddress(
        hEngine, "?Exec@UGameEngine@@UAEHPBDAAVFOutputDevice@@@Z");
    s_scriptExec = (ScriptConsoleExec_t)GetProcAddress(
        hCore, "?ScriptConsoleExec@UObject@@UAEHPBDAAVFOutputDevice@@PAV1@@Z");
    s_gLog = (void**)GetProcAddress(hCore, "?GLog@@3PAVFOutputDevice@@A");

    if (!s_gLog || !s_scriptExec) {
        char miss[256];
        _snprintf_s(miss, sizeof(miss), _TRUNCATE,
                    "required exports not found; automation inert "
                    "(GLog=%d ScriptConsoleExec=%d GEngine=%d UGameEngine::Exec=%d)",
                    s_gLog != nullptr, s_scriptExec != nullptr,
                    s_gEngine != nullptr, s_engineExec != nullptr);
        Log(miss);
        return;
    }

    if (!InstallPCTickHook(hEngine)) {
        Log("dispatch site unavailable; automation inert");
        return;
    }

    PathNextToExe("xiii_automation_cmds.txt", s_cmdPath);
    char temp[MAX_PATH];
    if (GetTempPathA(MAX_PATH, temp)) {
        _snprintf_s(s_logPath, sizeof(s_logPath), _TRUNCATE, "%sxiii_capture", temp);
        CreateDirectoryA(s_logPath, nullptr);
        _snprintf_s(s_logPath, sizeof(s_logPath), _TRUNCATE,
                    "%sxiii_capture\\xiii_automation.log", temp);
    }

    // Start from a clean slate so a line left over from a previous run does
    // not fire on load.
    FILE* f = nullptr;
    if (s_cmdPath[0] && fopen_s(&f, s_cmdPath, "w") == 0 && f) fclose(f);

    s_enabled = true;
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "automation ON (poll=%lums telemetry=%lums engineExec=%d) "
                "dispatch=APlayerController::Tick cmds=%s",
                s_pollMs, s_telemetryMs, s_engineExecOk ? 1 : 0, s_cmdPath);
    Log(line);
    AppendLog("=== automation session start ===");
    AppendLog(line);
}
