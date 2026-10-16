@echo off
setlocal
echo Installing pingkk for the current user...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12; & ([ScriptBlock]::Create((Invoke-WebRequest -UseBasicParsing -TimeoutSec 30 -Uri 'https://he.sb/pingkk.ps1').Content)); exit 0 } catch { Write-Host ('Installation failed: ' + $_.Exception.Message); exit 1 }"
set "pingkk_exit_code=%errorlevel%"
if not "%pingkk_exit_code%"=="0" echo Installation failed. See the message above.
echo.
pause
exit /b %pingkk_exit_code%
