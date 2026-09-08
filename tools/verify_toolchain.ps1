# Proves a bundled toolchain can build a snippet on a machine with no compiler.
#
# The check that matters is the PATH: it deliberately excludes the dev install,
# so anything the bundle is missing fails here instead of on your friend's
# machine. A bundle that is only complete because C:\mingw64 happens to be on
# PATH looks identical to a working one until it ships.
#
# Also checks the C runtime match. The UI captures a snippet's printf by
# rebinding fd 1, which only works while the snippet DLL and the host share a
# CRT -- a UCRT host and an MSVCRT snippet each get their own stdio state and
# the output vanishes with no error at all.

param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
  # objdump is not part of the shipped bundle -- nothing at runtime needs it --
  # so the CRT check borrows one. Dev install first, then PATH.
  [string]$Objdump = "C:\mingw64\bin\objdump.exe"
)

$ErrorActionPreference = "Stop"
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$tc = "$BuildDir\toolchain"
$fail = 0

function Bad($m) { Write-Host "FAIL  $m" -ForegroundColor Red; $script:fail++ }
function Good($m) { Write-Host "ok    $m" -ForegroundColor Green }

if (-not (Test-Path "$tc\bin\g++.exe")) { Bad "no g++ at $tc\bin"; exit 1 }
foreach ($f in @("fx.h","libeclipse_console.dll.a","eclipse_console.exe")) {
  if (Test-Path "$BuildDir\$f") { Good "present: $f" } else { Bad "missing: $f" }
}
if (Test-Path "$BuildDir\libwinpthread-1.dll") { Good "present: libwinpthread-1.dll (snippet runtime dep)" }
else { Bad "missing: libwinpthread-1.dll beside the exe - snippets will fail to LOAD, not to compile" }

$probe = "$BuildDir\_verify_probe.cpp"
@'
#include <string>
#include <vector>
#include <windows.h>
extern "C" void fx_run() {
    std::vector<std::string> v{"ok"};
    std::string s; for (auto& p : v) s += p;
    fx_log(s.c_str());
}
'@ | Out-File -FilePath $probe -Encoding utf8

$saved = $env:PATH
try {
  # The whole point: the dev toolchain is NOT reachable from here.
  $env:PATH = "$tc\bin;C:\Windows\system32;C:\Windows"
  $out = & "$tc\bin\g++.exe" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ `
    -I"$BuildDir" -include "$BuildDir\fx.h" $probe `
    -o "$BuildDir\_verify_probe.dll" -L"$BuildDir" -leclipse_console 2>&1
  if ($LASTEXITCODE -eq 0 -and (Test-Path "$BuildDir\_verify_probe.dll")) {
    Good "compiled a snippet with the dev toolchain off PATH"
  } else {
    Bad "compile failed with the dev toolchain off PATH:"
    $out | ForEach-Object { Write-Host "        $_" }
  }
} finally { $env:PATH = $saved }

# CRT match. Compare the api-ms-win-crt-* imports of both binaries: the host is
# the reference, and the snippet must not pull in msvcrt.dll instead.
if (Test-Path "$BuildDir\_verify_probe.dll") {
  $od = $Objdump
  if (-not (Test-Path $od)) {
    $cmd = Get-Command objdump.exe -ErrorAction SilentlyContinue
    if ($cmd) { $od = $cmd.Source }
  }
  if ($od -and (Test-Path $od)) {
    $snip = (& $od -p "$BuildDir\_verify_probe.dll" 2>$null | Select-String "DLL Name:") -join " "
    $host_ = (& $od -p "$BuildDir\eclipse_console.exe" 2>$null | Select-String "DLL Name:") -join " "
    $snipUcrt = $snip -match "api-ms-win-crt"
    $hostUcrt = $host_ -match "api-ms-win-crt"
    if ($snip -match "msvcrt\.dll") { Bad "snippet links msvcrt.dll - printf capture will silently break" }
    elseif ($snipUcrt -and $hostUcrt) { Good "snippet and host agree on the UCRT" }
    else { Bad "could not confirm a CRT match (snippet ucrt=$snipUcrt host ucrt=$hostUcrt)" }
  } else {
    Write-Host "skip  CRT check (no objdump available)"
  }
}

Remove-Item $probe, "$BuildDir\_verify_probe.dll" -Force -ErrorAction SilentlyContinue

if ($fail) { Write-Host "`n$fail check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall checks passed" -ForegroundColor Green
