// proxy/automation_hook.h
#pragma once

#include <cstdint>

// Automation harness (0.2.8): lets a command be delivered to the game's own
// console dispatcher from outside the process, without simulating any input.
//
// Mechanism: commands are appended (one per line) to a drop-file next to
// XIII.exe; each game tick the proxy reads it, executes every queued line
// through the engine's console dispatch, then truncates it. Because it goes
// through the engine's own Exec path rather than synthesized keystrokes, it
// works regardless of window focus -- the same reason the focus hook exists.
//
// Dispatch is two-tier, matching how the real console resolves a command:
//   1. UObject::ScriptConsoleExec on the live APlayerController -- the
//      UnrealScript exec functions (god, fly, ghost, walk, killpawns, ...).
//   2. UGameEngine::Exec on GEngine -- engine-level commands (open, start).
// Both targets, plus the GEngine and GLog globals, are resolved by their
// exported decorated names; if any lookup fails the harness stays inert.
//
// Everything is gated behind [VR] Automation=1 in XIII.ini (default off).
void InstallAutomationHook();

// Called once per frame from the camera hook, which already runs on the game
// thread with a live APlayerController -- Exec must not be called from any
// other thread. CameraLocation is three floats, CameraRotation three int32
// (Unreal FRotator units); either may be null.
void AutomationTick(void* playerController, const float* cameraLocation,
                    const int32_t* cameraRotation);
