param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateSet(5, 6)][int]$QtMajor = 6
)
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
function Invoke-SmokeProcess([string]$File, [string]$Arguments) {
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    $process.Handle | Out-Null
    if (-not $process.WaitForExit(60000)) {
        & taskkill.exe /PID $process.Id /T /F
        throw "Smoke test timed out: $File"
    }
    if ($process.ExitCode -ne 0) { throw "Smoke test failed: $File ($($process.ExitCode))" }
}
$before = @(Get-ChildItem $env:TEMP -Directory -Filter 'pingkk-*' | Select-Object -ExpandProperty FullName)
Invoke-SmokeProcess $executablePath '--smoke-test'
$remaining = @(Get-ChildItem $env:TEMP -Directory -Filter 'pingkk-*' |
    Where-Object { $_.FullName -notin $before })
if ($remaining.Count) { throw 'Portable launcher left temporary files behind' }
# Check Unicode/space paths and the extraction mode used for replacing Qt DLLs.
$destination = Join-Path $env:RUNNER_TEMP ('pingkk smoke 中文 ' + [guid]::NewGuid())
Invoke-SmokeProcess $executablePath "--extract `"$destination`""
foreach ($file in @('pingkk-gui.exe', 'pingkk.exe', "Qt${QtMajor}Core.dll", 'platforms/qwindows.dll', 'licenses/LGPL-3.0.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $destination $file))) { throw "Missing $file" }
}
Invoke-SmokeProcess (Join-Path $destination 'pingkk-gui.exe') '--smoke-test'
Invoke-SmokeProcess (Join-Path $destination 'pingkk.exe') '--help'
