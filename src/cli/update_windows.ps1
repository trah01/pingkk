param(
    [Parameter(Mandatory=$true)][string]$Target,
    [Parameter(Mandatory=$true)][string]$CurrentVersion,
    [Parameter(Mandatory=$true)][int]$ParentPid,
    [switch]$English
)
$ErrorActionPreference = 'Stop'
$workDir = $null
$staged = $null
$backup = $null
$originalTls = [Net.ServicePointManager]::SecurityProtocol
function Say([string]$Chinese, [string]$EnglishText) {
    if ($English) { Write-Host $EnglishText } else { Write-Host $Chinese }
}
function No-UpdateNeeded([string]$SourceVersion) {
    if ([Version]$SourceVersion -eq [Version]$CurrentVersion) {
        Say "当前版本 $CurrentVersion 已是最新。" "Current version $CurrentVersion is up to date."
    } else {
        Say "下载源版本 $SourceVersion 较旧，保留当前版本 $CurrentVersion。" "The source version $SourceVersion is older; keeping current version $CurrentVersion."
    }
}
# Use HttpWebRequest so the entire GitHub probe has a millisecond deadline,
# including redirects. Windows PowerShell 5.1 has no equivalent IWR deadline.
function Get-GitHubRelease {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $url = 'https://github.com/trah01/pingkk/releases/latest'
    for ($redirect = 0; $redirect -lt 8; $redirect++) {
        $remaining = 1000 - [int]$watch.ElapsedMilliseconds
        if ($remaining -le 0) { return $null }
        $request = [Net.HttpWebRequest]::Create($url)
        $request.Method = 'HEAD'
        $request.AllowAutoRedirect = $false
        $request.Timeout = $remaining
        $request.ReadWriteTimeout = $remaining
        $request.UserAgent = 'pingkk-updater'
        $response = $null
        $pending = $null
        try {
            $pending = $request.BeginGetResponse($null, $null)
            $remaining = 1000 - [int]$watch.ElapsedMilliseconds
            if ($remaining -le 0 -or -not $pending.AsyncWaitHandle.WaitOne($remaining)) {
                $request.Abort()
                return $null
            }
            $response = $request.EndGetResponse($pending)
            if ($watch.ElapsedMilliseconds -gt 1000) { return $null }
            $status = [int]$response.StatusCode
            if ($status -ge 300 -and $status -lt 400) {
                $next = [Uri]::new([Uri]$url, $response.Headers['Location'])
                if ($next.Scheme -ne 'https' -or $next.Host -ne 'github.com') { return $null }
                $url = $next.AbsoluteUri
                continue
            }
            if ($status -ne 200 -or $url -notmatch '^https://github\.com/trah01/pingkk/releases/tag/(v?\d+\.\d+\.\d+)$') { return $null }
            return $Matches[1]
        } catch {
            return $null
        } finally {
            if ($response) { $response.Close() }
            if ($pending -and $pending.IsCompleted) { $pending.AsyncWaitHandle.Close() }
        }
    }
    return $null
}
function Download([string]$Url, [string]$Destination, [int]$ConnectSeconds) {
    # Windows 7 may not have curl. HttpWebRequest works with TLS 1.2 in PS 5.1.
    $request = [Net.HttpWebRequest]::Create($Url)
    $request.Timeout = $ConnectSeconds * 1000
    $request.ReadWriteTimeout = 30000
    $request.UserAgent = 'pingkk-updater'
    $response = $null
    $stream = $null
    $file = $null
    $pending = $null
    try {
        $pending = $request.BeginGetResponse($null, $null)
        if (-not $pending.AsyncWaitHandle.WaitOne($ConnectSeconds * 1000)) {
            $request.Abort()
            throw 'The download connection timed out.'
        }
        $response = $request.EndGetResponse($pending)
        if ($response.ResponseUri.Scheme -ne 'https') { throw 'An insecure download redirect was rejected.' }
        $stream = $response.GetResponseStream()
        $file = [IO.File]::Create($Destination)
        $stream.CopyTo($file)
    } finally {
        if ($file) { $file.Dispose() }
        if ($stream) { $stream.Dispose() }
        if ($response) { $response.Close() }
        if ($pending -and $pending.IsCompleted) { $pending.AsyncWaitHandle.Close() }
    }
}
try {
    if ($env:OS -ne 'Windows_NT') { throw 'This updater is for Windows only.' }
    if ($PSVersionTable.PSVersion -lt [Version]'5.1') { throw 'PowerShell 5.1 or later is required.' }
    [Net.ServicePointManager]::SecurityProtocol = $originalTls -bor [Net.SecurityProtocolType]::Tls12
    if (-not [IO.Path]::IsPathRooted($Target) -or -not (Test-Path -LiteralPath $Target -PathType Leaf)) {
        throw 'The current executable path is invalid.'
    }
    $architecture = $env:PROCESSOR_ARCHITEW6432
    if (-not $architecture) { $architecture = $env:PROCESSOR_ARCHITECTURE }
    switch ($architecture.ToUpperInvariant()) {
        'ARM64' { $platform = 'windows-arm64' }
        'AMD64' {
            if ([Environment]::OSVersion.Version -lt [Version]'10.0.17763') { $platform = 'windows-x64-legacy' }
            else { $platform = 'windows-x64' }
        }
        default { throw "Unsupported Windows architecture: $architecture" }
    }
    $asset = "pingkk-$platform-cli.zip"
    $workDir = Join-Path ([IO.Path]::GetTempPath()) ('pingkk-update-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $workDir | Out-Null
    Say '正在检查更新，GitHub 连接超时为 1000ms……' 'Checking for updates; GitHub connection timeout is 1000ms...'
    $tag = Get-GitHubRelease
    if ($tag -and [Version]$tag.TrimStart('v') -le [Version]$CurrentVersion) {
        No-UpdateNeeded $tag.TrimStart('v')
        return
    }
    $source = 'mirror'
    $base = 'https://he.sb/pingkk/latest'
    if ($tag) {
        $source = 'github'
        $base = "https://github.com/trah01/pingkk/releases/download/$tag"
        Say '正在从 GitHub Releases 下载……' 'Downloading from GitHub Releases...'
        try {
            Download "$base/SHA256SUMS.txt" (Join-Path $workDir 'SHA256SUMS.txt') 1
            Download "$base/$asset" (Join-Path $workDir $asset) 1
        } catch {
            $source = 'mirror'
        }
    }
    if ($source -eq 'mirror') {
        $base = 'https://he.sb/pingkk/latest'
        Say 'GitHub 不可用，改从 https://he.sb/pingkk/latest/ 更新。' 'GitHub is unavailable; using https://he.sb/pingkk/latest/.'
        Download "$base/SHA256SUMS.txt" (Join-Path $workDir 'SHA256SUMS.txt') 10
        Download "$base/$asset" (Join-Path $workDir $asset) 10
    }
    $expected = $null
    foreach ($line in (Get-Content -LiteralPath (Join-Path $workDir 'SHA256SUMS.txt'))) {
        if ($line.Trim() -match ('^([a-fA-F0-9]{64})\s+\*?' + [Regex]::Escape($asset) + '$')) {
            $expected = $Matches[1]
            break
        }
    }
    if (-not $expected) { throw "No valid SHA256 checksum was found for $asset." }
    $archive = Join-Path $workDir $asset
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expected) {
        throw 'SHA256 verification failed. The current program was not changed.'
    }
    $unpacked = Join-Path $workDir 'unpacked'
    Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
    $binary = Join-Path $unpacked 'bin/pingkk.exe'
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw 'The release package does not contain the CLI executable.' }
    $banner = @(& $binary --help)
    if ($LASTEXITCODE -ne 0) { throw 'The new version cannot run on this system. The current program was not changed.' }
    if ($banner.Count -eq 0 -or $banner[0] -notmatch 'v(\d+\.\d+\.\d+)') { throw 'Unable to determine the new program version.' }
    $newVersion = $Matches[1]
    if ($source -eq 'github' -and $tag.TrimStart('v') -ne $newVersion) {
        throw 'The executable version does not match the GitHub release. The current program was not changed.'
    }
    if ([Version]$newVersion -le [Version]$CurrentVersion) {
        No-UpdateNeeded $newVersion
        return
    }
    $suffix = [Guid]::NewGuid().ToString('N')
    $staged = "$Target.update-$suffix.exe"
    $backup = "$Target.previous-$suffix.exe"
    Copy-Item -LiteralPath $binary -Destination $staged
    Say "正在更新 $CurrentVersion → ${newVersion}……" "Updating $CurrentVersion to $newVersion..."
    # A running Windows executable can be renamed, but cannot be overwritten.
    # Keep it until the new executable has taken its place, and roll back on error.
    Move-Item -LiteralPath $Target -Destination $backup
    try {
        Move-Item -LiteralPath $staged -Destination $Target
        $staged = $null
    } catch {
        Move-Item -LiteralPath $backup -Destination $Target
        $backup = $null
        throw
    }
    try {
        Remove-Item -LiteralPath $backup -Force
        $backup = $null
    } catch {
        # The old image is still mapped by the parent CLI. Clean up after it exits.
        $literal = "'" + $backup.Replace("'", "''") + "'"
        $cleanup = "Wait-Process -Id $ParentPid -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $literal -Force -ErrorAction SilentlyContinue"
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cleanup))
        try {
            Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -WindowStyle Hidden -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded)
        } catch {
            Say "旧版本暂存文件可在退出后删除：$backup" "The previous executable can be removed after exit: $backup"
        }
        $backup = $null
    }
    Say "更新完成：$newVersion" "Updated to $newVersion."
} catch {
    Say "更新失败：$($_.Exception.Message)" "Update failed: $($_.Exception.Message)"
    exit 1
} finally {
    [Net.ServicePointManager]::SecurityProtocol = $originalTls
    if ($staged -and (Test-Path -LiteralPath $staged)) { Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue }
    if ($workDir -and (Test-Path -LiteralPath $workDir)) { Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue }
}
