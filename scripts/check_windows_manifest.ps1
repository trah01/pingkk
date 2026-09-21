param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$mt = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/mt.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $mt) { throw 'Manifest tool not found' }
$manifest = Join-Path $env:RUNNER_TEMP 'pingkk-gui.manifest'
& $mt.FullName "-inputresource:$Executable;#1" "-out:$manifest"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read embedded manifest' }
$content = Get-Content -Raw $manifest
foreach ($declaration in @('true/pm', 'PerMonitorV2', 'PerMonitor')) {
    if ($content -notmatch [regex]::Escape($declaration)) {
        throw "DPI declaration is missing: $declaration"
    }
}
Write-Host 'Embedded manifest contains legacy and per-monitor-v2 DPI declarations.'
