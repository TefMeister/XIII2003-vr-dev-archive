// proxy/automation_hook.cpp
//
// See automation_hook.h for the mechanism. Exported symbols this relies on,
// all confirmed present in the shipped Engine.dll/Core.dll:
//   ?GEngine@@3PAVUEngine@@A                 -- UEngine* GEngine
//   ?GLog@@3PAVFOutputDevice@@A              -- FOutputDevice* GLog
//   ?Exec@UGameEngine@@UAEHPBDAAVFOutputDevice@@@Z
//   ?ScriptConsoleExec@UObject@@UAEHPBDAAVFOutputDevice@@PAV1@@Z
// The mangled names decode to ANSI char* (PBD), so this build of the engine
// is not Unicode and commands are passed as plain char*.
//
// GLog is used as the FOutputDevice both calls require, so anything a command
// prints lands in the game's own log rather than needing a fabricated vtable.

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

static bool                s_enabled       = false;
static Exec_t              s_engineExec    = nullptr;
static ScriptConsoleExec_t s_scriptExec    = nullptr;
static void**              s_gEngine       = nullptr;  // &GEngine
static void**              s_gLog          = nullptr;  // &GLog
static DWORD               s_pollMs        = 200;
static DWORD               s_telemetryMs   = 1000;
static DWORD               s_lastPoll      = 0;
static DWORD               s_lastTelemetry = 0;
static uint64_t            s_tick          = 0;
static char                s_cmdPath[MAX_PATH]  = {0};
static char                s_logPath[MAX_PATH]  = {0};

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

// Opened and closed per write so the file is always flush-safe mid-session --
// same convention as the 0.2.7 perf log.
static void AppendLog(const char* line) {
    if (!s_logPath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, s_logPath, "a") != 0 || !f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
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
        // Truncate so each command runs exactly once.
        if (fopen_s(&f, s_cmdPath, "w") == 0 && f) fclose(f);
    }
    return contents;
}

// Two-tier dispatch, mirroring how the console resolves a command: the
// player's own exec functions first, then the engine's.
static void RunCommand(void* playerController, const std::string& cmd) {
    void* ar = s_gLog ? *s_gLog : nullptr;
    if (!ar) { Log("GLog is null; skipping command"); return; }

    int handled = 0;
    const char* by = "unhandled";
    if (playerController && s_scriptExec) {
        handled = s_scriptExec(playerController, nullptr, cmd.c_str(), ar, playerController);
        if (handled) by = "playercontroller";
    }
    if (!handled && s_engineExec && s_gEngine && *s_gEngine) {
        handled = s_engineExec(*s_gEngine, nullptr, cmd.c_str(), ar);
        if (handled) by = "engine";
    }

    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "exec \"%s\" -> %s", cmd.c_str(), by);
    Log(line);
    AppendLog(line);
}

void AutomationTick(void* playerController, const float* cameraLocation,
                    const int32_t* cameraRotation) {
    if (!s_enabled) return;
    ++s_tick;
    const DWORD now = GetTickCount();

    // GetTickCount wraps every ~49 days; the unsigned subtraction stays
    // correct across the wrap, so no special-casing is needed.
    if (now - s_lastPoll >= s_pollMs) {
        s_lastPoll = now;
        std::string contents = ReadAndClearCommandFile();
        if (!contents.empty())
            for (const std::string& cmd : xiii::ParseCommandFile(contents))
                RunCommand(playerController, cmd);
    }

    if (s_telemetryMs && now - s_lastTelemetry >= s_telemetryMs) {
        s_lastTelemetry = now;
        xiii::TelemetrySample s{};
        s.tick     = s_tick;
        s.uptimeMs = now;
        if (cameraLocation) { s.x = cameraLocation[0]; s.y = cameraLocation[1]; s.z = cameraLocation[2]; }
        if (cameraRotation) { s.pitch = cameraRotation[0]; s.yaw = cameraRotation[1]; s.roll = cameraRotation[2]; }
        AppendLog(xiii::FormatTelemetryLine(s).c_str());
    }
}

void InstallAutomationHook() {
    if (ReadVrInt("Automation", 0) == 0) return;

    s_pollMs      = ReadVrInt("AutomationPollMs", 200);
    s_telemetryMs = ReadVrInt("AutomationTelemetryMs", 1000);

    // Module split verified against the shipped binaries' export tables:
    // GEngine and UGameEngine::Exec are Engine.dll; ScriptConsoleExec (a
    // UObject method) and GLog are Core.dll. Looking in the wrong module
    // silently yields null, so each is resolved from its own.
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    HMODULE hCore   = GetModuleHandleA("Core.dll");
    if (!hEngine || !hCore) { Log("Engine.dll/Core.dll not loaded; automation inert"); return; }

    s_gEngine    = (void**)GetProcAddress(hEngine, "?GEngine@@3PAVUEngine@@A");
    s_engineExec = (Exec_t)GetProcAddress(
        hEngine, "?Exec@UGameEngine@@UAEHPBDAAVFOutputDevice@@@Z");
    s_scriptExec = (ScriptConsoleExec_t)GetProcAddress(
        hCore, "?ScriptConsoleExec@UObject@@UAEHPBDAAVFOutputDevice@@PAV1@@Z");
    s_gLog = (void**)GetProcAddress(hCore, "?GLog@@3PAVFOutputDevice@@A");

    if (!s_gEngine || !s_gLog || (!s_engineExec && !s_scriptExec)) {
        char miss[256];
        _snprintf_s(miss, sizeof(miss), _TRUNCATE,
                    "required exports not found; automation inert "
                    "(GEngine=%d GLog=%d UGameEngine::Exec=%d ScriptConsoleExec=%d)",
                    s_gEngine != nullptr, s_gLog != nullptr,
                    s_engineExec != nullptr, s_scriptExec != nullptr);
        Log(miss);
        return;
    }

    PathNextToExe("xiii_automation_cmds.txt", s_cmdPath);
    lstrcpyA(s_logPath, "");
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
                "automation ON (poll=%lums telemetry=%lums) cmds=%s log=%s",
                s_pollMs, s_telemetryMs, s_cmdPath, s_logPath);
    Log(line);
    AppendLog("=== automation session start ===");
    AppendLog(line);
}
