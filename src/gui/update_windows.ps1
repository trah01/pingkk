param(
    [switch]$Prepare,
    [int]$ParentPid = 0,
    [int]$LauncherPid = 0,
    [string]$Kind,
    [string]$Target,
    [Parameter(Mandatory)][string]$Package,
    [string]$Extracted,
    [Parameter(Mandatory)][string]$ExpectedVersion,
    [string]$Language = 'zh'
)
$ErrorActionPreference = 'Stop'
if ($Prepare) {
    $version = (Get-Item -LiteralPath $Package).VersionInfo.ProductVersion
    if ($version -notmatch '^([0-9]+\.[0-9]+\.[0-9]+)(?:\.0)?$' -or $Matches[1] -ne $ExpectedVersion) {
        throw 'Installer version does not match the update version.'
    }
    exit 0
}

$work = $PSScriptRoot
$resultFile = Join-Path $work 'result.txt'
$logFile = Join-Path $work 'apply.log'
$token = [Guid]::NewGuid().ToString('N')
$backup = "$Target.pingkk-old-$token"
$incoming = "$Target.pingkk-new-$token"
$movedOld = $false
$installed = $false

function Write-Result([string]$Text) {
    try {
        [IO.File]::WriteAllText($resultFile, $Text, (New-Object Text.UTF8Encoding $false))
    } catch {
        # 即使磁盘空间不足以写日志，也继续尝试重新打开应用。
    }
}

foreach ($processId in @($ParentPid, $LauncherPid)) {
    if ($processId -gt 0) {
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($process -and -not $process.WaitForExit(30000)) {
            Write-Result "error`nThe application did not exit; no files were changed.`n$Language"
            exit 1
        }
    }
}

try {
    if ($Kind -eq 'windows-installer') {
        $installer = Start-Process -FilePath $Package -Verb RunAs -Wait -PassThru -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/RESTARTEXITCODE=3010', '/NOCLOSEAPPLICATIONS',
            "/DIR=`"$Target`"", "/LOG=`"$logFile`""
        )
        if ($installer.ExitCode -ne 0) { throw "Installer returned $($installer.ExitCode)." }
    } else {
        if ($Kind -eq 'windows-file') {
            Copy-Item -LiteralPath $Package -Destination $incoming
        } elseif ($Kind -eq 'windows-directory') {
            Copy-Item -LiteralPath $Extracted -Destination $incoming -Recurse
        } else { throw 'Unsupported installation type.' }
        Move-Item -LiteralPath $Target -Destination $backup
        $movedOld = $true
        Move-Item -LiteralPath $incoming -Destination $Target
    }
    $installed = $true
    Write-Result "ok`n$ExpectedVersion`n$Language"
} catch {
    $_ | Out-String | Add-Content -Encoding utf8 $logFile -ErrorAction SilentlyContinue
    if ($movedOld -and (Test-Path -LiteralPath $backup)) {
        # 若新文件已放入目标，先移开，再恢复旧文件；不覆盖用户的其他路径。
        if (Test-Path -LiteralPath $Target) {
            Move-Item -LiteralPath $Target -Destination "$incoming-failed" -ErrorAction SilentlyContinue
        }
        Move-Item -LiteralPath $backup -Destination $Target -ErrorAction SilentlyContinue
    }
    $message = if ($Language -eq 'en') {
        'Update failed or permission was denied. Please check the update log.'
    } else { '更新失败或未获得安装权限，请查看更新日志。' }
    Write-Result "error`n$message`n$Language"
}

try {
    $executable = if ($Kind -eq 'windows-file') { $Target } else { Join-Path $Target 'pingkk-gui.exe' }
    Start-Process -FilePath $executable -ArgumentList @('--update-result', "`"$resultFile`"")
    if ($installed) {
        Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $Package -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $Extracted -Recurse -Force -ErrorAction SilentlyContinue
    }
} catch {
    $_ | Out-String | Add-Content -Encoding utf8 $logFile -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show("无法重新打开应用，请手动启动。`n$logFile", 'pingkk') | Out-Null
    exit 1
}
