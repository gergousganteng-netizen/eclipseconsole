# THROWAWAY -- build/coldloop.ps1. Cold-open sampler: restart the console, one
# open + close under the DXGI probe, keep the files. Delete when done.
# usage: coldloop.ps1 <tag> <count> <variant: game|third>
param([string]$tag, [int]$count, [string]$variant)
$b = 'C:\Users\user\downloads\eclipseconsole\build'
Set-Location $b
for ($i = 1; $i -le $count; $i++) {
  Stop-Process -Name eclipse_console -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 1200
  $env:ECLIPSE_ZLOG = '1'
  Start-Process -FilePath "$b\eclipse_console.exe" -WorkingDirectory $b
  # attach + HUD_READY: poll the trace for the badge rects, up to 12s
  $ok = $false
  for ($w = 0; $w -lt 60; $w++) {
    Start-Sleep -Milliseconds 200
    if ((Test-Path "$env:TEMP\eclipse_z.log") -and (Select-String -Path "$env:TEMP\eclipse_z.log" -Pattern 'RECTS -> 1 solid' -Quiet)) { $ok = $true; break }
  }
  Start-Sleep -Milliseconds 1500
  if ($variant -eq 'game') { $script = 'third,w300,gamefg,w600,ins,w2500,ins,w1500' }
  else                     { $script = 'third,w600,ins,w2500,ins,w1500' }
  & "$b\zdup.exe" $script | Out-Null
  Copy-Item zdup_px.txt "zdup_cold_${tag}_${variant}_$i.px"
  Copy-Item zdup_z.txt  "zdup_cold_${tag}_${variant}_$i.z"
  Copy-Item zdup_g.txt  "zdup_cold_${tag}_${variant}_$i.g"
  Copy-Item "$env:TEMP\eclipse_z.log" "zdup_cold_${tag}_${variant}_$i.zlog"
  "cycle $i ready=$ok done"
}
