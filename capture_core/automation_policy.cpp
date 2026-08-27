#include "automation_policy.h"

#include <cstdio>

namespace xiii {
namespace {

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool IsComment(const std::string& line) {
    return line[0] == '#' || (line.size() >= 2 && line[0] == '/' && line[1] == '/');
}

}  // namespace

std::vector<std::string> ParseCommandFile(const std::string& contents) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= contents.size()) {
        size_t nl = contents.find('\n', start);
        size_t end = (nl == std::string::npos) ? contents.size() : nl;
        std::string line = Trim(contents.substr(start, end - start));
        if (!line.empty() && !IsComment(line) && line.size() <= kMaxCommandLength)
            out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string FormatTelemetryLine(const TelemetrySample& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "tick=%llu ms=%llu pos=%.2f,%.2f,%.2f rot=%d,%d,%d",
                  (unsigned long long)s.tick, (unsigned long long)s.uptimeMs,
                  s.x, s.y, s.z, s.pitch, s.yaw, s.roll);
    return std::string(buf);
}

}  // namespace xiii
