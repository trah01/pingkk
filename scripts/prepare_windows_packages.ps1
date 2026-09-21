param(
    [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture,
    [Parameter(Mandatory)][ValidateSet(5, 6)][int]$QtMajor
)
$ErrorActionPreference = 'Stop'
./scripts/stage_windows_runtime.ps1 -Architecture $Architecture -QtMajor $QtMajor

New-Item -ItemType Directory -Force dist/licenses | Out-Null
Copy-Item licenses/* dist/licenses/
@"
图形界面：运行 bin/pingkk-gui.exe
图形版已内置命令行程序，供图形界面测试和“安装命令行工具”功能使用。
"@ | Set-Content -Encoding utf8 dist/使用说明.txt

New-Item -ItemType Directory -Force dist-cli/bin, dist-cli/licenses | Out-Null
Copy-Item dist/bin/pingkk.exe dist-cli/bin/
Copy-Item licenses/* dist-cli/licenses/
@"
命令行：在终端执行 bin/pingkk.exe --help
"@ | Set-Content -Encoding utf8 dist-cli/使用说明.txt

New-Item -ItemType Directory -Force dist-portable/licenses | Out-Null
Copy-Item dist/bin/* dist-portable/ -Recurse
Copy-Item licenses/* dist-portable/licenses/
@"
双击 pingkk-gui.exe 即可运行，无需安装。
命令行：在当前目录打开终端并执行 pingkk.exe --help
"@ | Set-Content -Encoding utf8 dist-portable/使用说明.txt
