# Builds the shippable installer.
#
# Checks the payload BEFORE calling ISCC. Inno happily builds an installer from
# whatever is present, so a missing toolchain or a stale exe produces a setup
# that installs cleanly and then fails on the user's machine -- which is the
# one place the failure is expensive.
#
# Order for a release:
#   cmake --build build
#   tools\bundle_toolchain.ps1
#   tools\verify_toolchain.ps1
#   tools\build_installer.ps1

param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
  [string]$Iscc     = ""
)

$ErrorActionPreference = "Stop"
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$root     = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

# ---- locate ISCC ------------------------------------------------------------
if (-not $Iscc) {
  # LocalAppData first: winget installs Inno per-user by default, so the
  # Program Files paths miss it entirely on a machine where it plainly is
  # installed.
  $Iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) {
  $c = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($c) { $Iscc = $c.Source }
}
if (-not $Iscc) {
  Write-Host "Inno Setup 6 not found." -ForegroundColor Red
  Write-Host "  winget install --id JRSoftware.InnoSetup -e"
  Write-Host "  (or https://jrsoftware.org/isdl.php), then re-run this script."
  exit 1
}

# ---- payload checks ---------------------------------------------------------
$fail = 0
function Need($rel, $why) {
  $p = Join-Path $BuildDir $rel
  if (Test-Path $p) { return }
  Write-Host "MISSING  $rel  -- $why" -ForegroundColor Red
  $script:fail++
}

Need "eclipse_console.exe"      "the UI binary"
Need "eclipse_cli.exe"          "the terminal binary"
Need "libwinpthread-1.dll"      "snippet DLLs import it; run tools\bundle_toolchain.ps1"
Need "libeclipse_console.dll.a" "snippets link back against the UI binary"
Need "libeclipse_cli.dll.a"     "snippets link back against the CLI binary"
Need "fx.h"                     "the snippet API header"
Need "web\index.html"           "the UI"
Need "conf\offsets_windows.conf" "seed offsets for the first run"
Need "toolchain\bin\g++.exe"    "the bundled compiler; run tools\bundle_toolchain.ps1"
Need "toolchain\libexec\gcc\x86_64-w64-mingw32" "the C++ front end"

if ($fail) { Write-Host "`n$fail item(s) missing - not building." -ForegroundColor Red; exit 1 }

# A toolchain that is present but incomplete is the failure this cannot see,
# so lean on the real check rather than re-implementing a weaker one.
Write-Host "running verify_toolchain..." -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "verify_toolchain.ps1") -BuildDir $BuildDir
if ($LASTEXITCODE -ne 0) { Write-Host "toolchain verification failed - not building." -ForegroundColor Red; exit 1 }

# Warn if the exe is older than the sources: shipping a stale binary is silent.
$newestSrc = Get-ChildItem (Join-Path $root "src"), (Join-Path $root "web") -Recurse -File |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
$exe = Get-Item (Join-Path $BuildDir "eclipse_console.exe")
if ($newestSrc -and $newestSrc.LastWriteTime -gt $exe.LastWriteTime) {
  Write-Host "WARNING: $($newestSrc.Name) is newer than eclipse_console.exe - run cmake --build build first." -ForegroundColor Yellow
}

$mb = (Get-ChildItem $BuildDir -Recurse -File |
       Where-Object { $_.FullName -notmatch '\\(CMakeFiles|_deps)\\' } |
       Measure-Object Length -Sum).Sum / 1MB
Write-Host ("payload: {0:N0} MB uncompressed" -f $mb)

New-Item -ItemType Directory -Force -Path (Join-Path $root "dist") | Out-Null
& $Iscc (Join-Path $PSScriptRoot "installer.iss")
if ($LASTEXITCODE -ne 0) { Write-Host "ISCC failed" -ForegroundColor Red; exit 1 }

Get-ChildItem (Join-Path $root "dist") -Filter "*.exe" |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
  ForEach-Object { Write-Host ("`nbuilt: {0}  ({1:N0} MB)" -f $_.FullName, ($_.Length/1MB)) -ForegroundColor Green }
