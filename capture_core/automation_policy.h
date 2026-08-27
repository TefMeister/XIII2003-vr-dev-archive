// capture_core/automation_policy.h
//
// Pure logic for the automation harness (0.2.8): turning the contents of the
// drop-file into a list of console commands, and formatting one telemetry
// record. The engine-side plumbing (resolving UGameEngine::Exec, reading and
// truncating the file, writing the log) lives in proxy/automation_hook.cpp;
// everything here is Windows-free so it can be unit-tested.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xiii {

// Longest command accepted. UGameEngine::Exec takes a raw char*, so a
// pathological line is dropped whole rather than sent half-formed.
const size_t kMaxCommandLength = 512;

// Splits the drop-file contents into commands, one per line, in file order.
// Blank lines, whitespace-only lines, and lines whose first non-space
// character starts a comment ('#' or "//") are skipped; surrounding
// whitespace is trimmed. Lines longer than kMaxCommandLength are dropped.
std::vector<std::string> ParseCommandFile(const std::string& contents);

// One telemetry observation: where the render camera was on a given tick.
// Rotations are raw Unreal FRotator units (65536 per revolution).
struct TelemetrySample {
    uint64_t tick;
    uint64_t uptimeMs;
    float    x, y, z;
    int32_t  pitch, yaw, roll;
};

// Formats one record as a single line (no trailing newline -- the writer
// appends it).
std::string FormatTelemetryLine(const TelemetrySample& s);

}  // namespace xiii
