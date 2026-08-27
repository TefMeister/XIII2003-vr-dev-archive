#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "automation_policy.h"

using xiii::ParseCommandFile;
using xiii::FormatTelemetryLine;

TEST_CASE("a single command line is parsed") {
    auto cmds = ParseCommandFile("god");
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0] == "god");
}

TEST_CASE("multiple lines each become a command, in order") {
    auto cmds = ParseCommandFile("god\nfly\nslomo 0.5");
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0] == "god");
    CHECK(cmds[1] == "fly");
    CHECK(cmds[2] == "slomo 0.5");
}

TEST_CASE("CRLF line endings are handled") {
    auto cmds = ParseCommandFile("god\r\nfly\r\n");
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0] == "god");
    CHECK(cmds[1] == "fly");
}

TEST_CASE("surrounding whitespace is trimmed") {
    auto cmds = ParseCommandFile("   slomo 0.5   \n\t god \t");
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0] == "slomo 0.5");
    CHECK(cmds[1] == "god");
}

TEST_CASE("blank and whitespace-only lines are skipped") {
    auto cmds = ParseCommandFile("god\n\n   \n\t\nfly");
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0] == "god");
    CHECK(cmds[1] == "fly");
}

TEST_CASE("comment lines are skipped") {
    auto cmds = ParseCommandFile("# a note\ngod\n// another note\nfly");
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0] == "god");
    CHECK(cmds[1] == "fly");
}

TEST_CASE("a comment marker mid-line is not treated as a comment") {
    // Only a leading marker starts a comment; UE console args may contain #.
    auto cmds = ParseCommandFile("say hello # world");
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0] == "say hello # world");
}

TEST_CASE("empty input yields no commands") {
    CHECK(ParseCommandFile("").empty());
    CHECK(ParseCommandFile("\n\n  \n").empty());
}

TEST_CASE("an over-long command is rejected rather than truncated") {
    // Exec takes a raw char*; a pathological line is dropped, not half-sent.
    std::string huge(xiii::kMaxCommandLength + 1, 'x');
    CHECK(ParseCommandFile(huge).empty());
}

TEST_CASE("a command exactly at the length limit is kept") {
    std::string atLimit(xiii::kMaxCommandLength, 'x');
    auto cmds = ParseCommandFile(atLimit);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].size() == xiii::kMaxCommandLength);
}

TEST_CASE("telemetry line carries tick, time and full camera pose") {
    xiii::TelemetrySample s{};
    s.tick = 42;
    s.uptimeMs = 1500;
    s.x = 1.5f; s.y = -2.25f; s.z = 300.0f;
    s.pitch = 100; s.yaw = 32768; s.roll = 0;

    std::string line = FormatTelemetryLine(s);

    CHECK(line.find("tick=42") != std::string::npos);
    CHECK(line.find("ms=1500") != std::string::npos);
    CHECK(line.find("pos=") != std::string::npos);
    CHECK(line.find("rot=") != std::string::npos);
    CHECK(line.find("32768") != std::string::npos);
    // One record per line: the writer appends the newline itself.
    CHECK(line.find('\n') == std::string::npos);
}
