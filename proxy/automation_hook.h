// proxy/automation_hook.h
#pragma once

#include <cstdint>

// Automation harness (0.2.9): lets a command be delivered to the game's own
// console dispatcher from outside the process, without simulating any input.
//
// Commands are appended (one per line) to a drop-file next to XIII.exe; the
// proxy reads it, executes every queued line through the engine's console
// dispatch, then truncates it. Because it goes through the engine's own Exec
// path rather than synthesized keystrokes, it works regardless of window
// focus -- the same reason the focus hook exists.
//
// WHERE the dispatch happens is the whole lesson of 0.2.8, which crashed with
// a General protection fault inside UGameEngine::Exec:
//
//     History: UGameEngine::Exec <- UGameEngine::Draw <- UWindowsViewport::
//              Repaint <- UWindowsClient::Tick <- ClientTick <-
//              UGameEngine::Tick <- UpdateWorld <- MainLoop
//
// 0.2.8 drained the queue from the camera hook (eventPlayerCalcView), which
// the stack above shows is reached from INSIDE UGameEngine::Draw. Running a
// console command re-entrantly mid-render faults. So dispatch now runs from
// APlayerController::Tick -- the game-logic phase, outside the render path --
// and the camera hook is left to do telemetry only.
//
// Dispatch is two-tier, matching how the real console resolves a command:
//   1. UObject::ScriptConsoleExec on the live APlayerController -- the
//      UnrealScript exec functions (god, fly, ghost, walk, killpawns, ...).
//   2. UGameEngine::Exec on GEngine -- engine-level commands (open, start).
// Tier 2 is the call that faulted in 0.2.8 and is now OPT-IN, behind
// [VR] AutomationEngineExec=1, so the default path only touches the player.
//
// Symbols are resolved by exported decorated name from the module that
// actually defines them (Engine.dll vs Core.dll -- see the .cpp); if any
// lookup or the Tick prologue check fails, the harness stays inert.
//
// Gated behind [VR] Automation=1 in XIII.ini (default off).
void InstallAutomationHook();

// Telemetry only -- called once per frame from the camera hook, which has the
// camera pose in hand. Deliberately does NOT run commands: it is inside the
// render path (see above). CameraLocation is three floats, CameraRotation
// three int32 (Unreal FRotator units); either may be null.
void AutomationTelemetryTick(const float* cameraLocation,
                             const int32_t* cameraRotation);
