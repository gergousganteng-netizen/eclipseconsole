# Assembles the g++ that ships with the app.
#
# Deliberately NOT part of `cmake --build`: the result is ~320 MB and changes
# only when the compiler does, so staging it on every build would cost minutes
# a day to produce a byte-identical tree.
#
# Why we ship the DEV machine's toolchain rather than downloading one:
# the snippet DLL must use the same C runtime as the host exe, because the UI
# captures a snippet's printf by rebinding fd 1. A UCRT host loading a snippet
# built by an MSVCRT g++ gets two independent stdio states and the output
# silently disappears -- no error, just nothing. Copying the compiler that
# built the host is the only way to guarantee they agree.
#
# Verify with tools/verify_toolchain.ps1 after running this.

param(
  [string]$Source = "C:\mingw64",
  [string]$Dest   = (Join-Path $PSScriptRoot "..\build\toolchain")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path "$Source\bin\g++.exe")) { throw "no g++ at $Source\bin" }

# Read the version and triple off the tree rather than hardcoding them, so a
# compiler upgrade does not silently produce an empty bundle.
$trip = "x86_64-w64-mingw32"
$verDir = Get-ChildItem "$Source\libexec\gcc\$trip" -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $verDir) { throw "no libexec\gcc\$trip\<version> under $Source" }
$ver = $verDir.Name
Write-Host "toolchain: gcc $ver ($trip)"

$Dest = [System.IO.Path]::GetFullPath($Dest)
New-Item -ItemType Directory -Force -Path "$Dest\bin" | Out-Null

# ---- bin: the driver, the assembler, the linker, and their DLL closure -------
# The closure is resolved with objdump rather than a hand-written list, because
# a missing DLL shows up as a process that will not start with no message at
# all. bin\ in a full install is 250 MB; almost all of it is Fortran, Ada, GDB,
# cmake and doxygen.
$seeds = @(
  "$Source\bin\g++.exe", "$Source\bin\gcc.exe", "$Source\bin\as.exe",
  "$Source\bin\ld.exe",  "$Source\bin\ar.exe",
  "$Source\libexec\gcc\$trip\$ver\cc1plus.exe",
  "$Source\libexec\gcc\$trip\$ver\collect2.exe"
) | Where-Object { Test-Path $_ }

$need  = New-Object System.Collections.Generic.HashSet[string]
$queue = New-Object System.Collections.Generic.Queue[string]
$seeds | ForEach-Object { $queue.Enqueue($_) }
while ($queue.Count -gt 0) {
  $f = $queue.Dequeue()
  foreach ($line in (& "$Source\bin\objdump.exe" -p $f 2>$null)) {
    if ($line -match '^\s*DLL Name:\s*(\S+)') {
      $dll = $Matches[1]
      $p = Join-Path "$Source\bin" $dll
      if ((Test-Path $p) -and (-not $need.Contains($dll))) { [void]$need.Add($dll); $queue.Enqueue($p) }
    }
  }
}
Write-Host "  bin: $($seeds.Count) exes + $($need.Count) dlls"
$need  | ForEach-Object { Copy-Item (Join-Path "$Source\bin" $_) "$Dest\bin\" -Force }
$seeds | Where-Object { $_ -like "$Source\bin\*" } | Copy-Item -Destination "$Dest\bin\" -Force

# ---- libexec: the C++ front end and the link driver --------------------------
# liblto_plugin.dll is NOT optional despite us never passing -flto: the driver
# hands -fuse-linker-plugin to the linker by default and aborts without it.
$le = "$Dest\libexec\gcc\$trip\$ver"
New-Item -ItemType Directory -Force -Path $le | Out-Null
foreach ($f in @("cc1plus.exe","collect2.exe","liblto_plugin.dll")) {
  Copy-Item "$Source\libexec\gcc\$trip\$ver\$f" $le -Force
}

# ---- lib/gcc: crt objects, libgcc, GCC's internal headers --------------------
# `plugin` is 11 MB of headers for building GCC plugins; nothing reads it here.
$lg = "$Dest\lib\gcc\$trip\$ver"
New-Item -ItemType Directory -Force -Path $lg | Out-Null
Get-ChildItem "$Source\lib\gcc\$trip\$ver" -File | Copy-Item -Destination $lg -Force
foreach ($d in @("include","include-fixed")) {
  if (Test-Path "$Source\lib\gcc\$trip\$ver\$d") {
    Copy-Item "$Source\lib\gcc\$trip\$ver\$d" $lg -Recurse -Force
  }
}

# ---- lib: libstdc++ and friends, not python/gfortran/bfd --------------------
# libgcc* is here rather than only under lib\gcc: libgcc_s.a lives in this
# directory, and without it a link that does NOT pass -static-libgcc fails with
# "cannot find -lgcc_s". The app always passes it, so this went unnoticed until
# something compiled with default flags.
New-Item -ItemType Directory -Force -Path "$Dest\lib" | Out-Null
Get-ChildItem "$Source\lib" -File |
  Where-Object { $_.Name -like "libstdc++*" -or $_.Name -like "libsupc++*" -or
                 $_.Name -like "libssp*"    -or $_.Name -like "libgcc*" } |
  Copy-Item -Destination "$Dest\lib" -Force

# ---- headers and import libraries -------------------------------------------
# Copied whole. These are the Windows API surface a snippet may reach for, and
# trimming them by guesswork trades 90 MB for a failure that only appears when
# someone finally uses the header we dropped.
Copy-Item "$Source\include"      "$Dest\include"      -Recurse -Force
New-Item -ItemType Directory -Force -Path "$Dest\$trip" | Out-Null
Copy-Item "$Source\$trip\include" "$Dest\$trip\include" -Recurse -Force
Copy-Item "$Source\$trip\lib"     "$Dest\$trip\lib"     -Recurse -Force

# ---- runtime DLL for the snippets themselves --------------------------------
# Snippet DLLs import libwinpthread-1.dll: libstdc++ is threaded, and
# -static-libstdc++ does not pull winpthread in with it. It has to sit beside
# the EXE, not in toolchain\bin -- the host is what LoadLibrary's the snippet,
# so it is the host's search path that has to resolve it.
$exeDir = Split-Path $Dest -Parent
Copy-Item "$Source\bin\libwinpthread-1.dll" $exeDir -Force
Write-Host "  libwinpthread-1.dll -> $exeDir"

$mb = (Get-ChildItem $Dest -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("done: {0:N1} MB at {1}" -f $mb, $Dest)
