# tools/xiii-auto.ps1
#
# Drives the 0.2.8 automation harness from outside the game: appends console
# commands to the drop-file the proxy polls, and tails the telemetry log.
# Requires [VR] Automation=1 in the game's XIII.ini.
#
#   .\xiii-auto.ps1 -Send 'god','fly'
#   .\xiii-auto.ps1 -Tail 40
#   .\xiii-auto.ps1 -Send 'walk' -Tail 20
#
# The game must be in actual gameplay (not a menu): the harness ticks from the
# camera hook, which only runs while a PlayerController is rendering a view.

param(
    [string]$System = "D:\Program Files (x86)\Steam\steamapps\common\XIII - Classic\system",
    [string[]]$Send,
    [int]$Tail = 0,
    [switch]$Clear
)

$cmdFile = Join-Path $System "xiii_automation_cmds.txt"
$logFile = Join-Path $env:TEMP "xiii_capture\xiii_automation.log"

if ($Clear) {
    if (Test-Path $logFile) { Clear-Content $logFile; "cleared $logFile" }
}

if ($Send) {
    # ASCII: the engine's Exec takes a plain char* in this build.
    Add-Content -Path $cmdFile -Value $Send -Encoding ASCII
    "sent $($Send.Count) command(s) -> $cmdFile"
    foreach ($c in $Send) { "  $c" }
}

if ($Tail -gt 0) {
    if (Test-Path $logFile) {
        "--- last $Tail line(s) of $logFile ---"
        Get-Content $logFile -Tail $Tail
    } else {
        "no telemetry log yet at $logFile (has the game reached gameplay?)"
    }
}
