param(
    [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture,
    [Parameter(Mandatory)][ValidateSet(5, 6)][int]$QtMajor
)
$ErrorActionPreference = 'Stop'
$qt = $env:QT_ROOT_DIR
New-Item -ItemType Directory -Force dist/bin/platforms, dist/bin/styles | Out-Null
Copy-Item "$qt/plugins/platforms/qwindows.dll" dist/bin/platforms/
$style = if ($QtMajor -eq 5) { 'qwindowsvistastyle.dll' } else { 'qmodernwindowsstyle.dll' }
Copy-Item "$qt/plugins/styles/$style" dist/bin/styles/
# Qt's shared libraries still need the app-local MSVC runtime. No installer/UAC.
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -property installationPath
$runtimeVersion = if ($Architecture -eq 'x64') { '14.29.*' } else { '*' }
$crt = Get-ChildItem "$vs/VC/Redist/MSVC/$runtimeVersion/$Architecture/Microsoft.VC*.CRT" -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "MSVC runtime for $Architecture is missing" }
Copy-Item "$($crt.FullName)/*.dll" dist/bin/
@('[Paths]', 'Plugins=.') | Set-Content -Encoding ascii dist/bin/qt.conf
