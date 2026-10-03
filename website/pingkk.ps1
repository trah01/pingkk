# Windows PowerShell 5.1+: irm https://he.sb/pingkk.ps1 | iex
# Install for the current user; administrator privileges are not required.
$ErrorActionPreference = 'Stop'
$workDir = $null
$originalTls = [Net.ServicePointManager]::SecurityProtocol
try {
    if ($env:OS -ne 'Windows_NT') { throw 'This installer is for Windows only.' }
    if ($PSVersionTable.PSVersion.Major -lt 5) { throw 'PowerShell 5.1 or later is required.' }
    [Net.ServicePointManager]::SecurityProtocol = $originalTls -bor [Net.SecurityProtocolType]::Tls12

    $architecture = $env:PROCESSOR_ARCHITEW6432
    if (-not $architecture) { $architecture = $env:PROCESSOR_ARCHITECTURE }
    switch ($architecture.ToUpperInvariant()) {
        'ARM64' { $platform = 'windows-arm64' }
        'AMD64' {
            $osVersion = [Environment]::OSVersion.Version
            if ($osVersion -lt [Version]'10.0.17763') {
                $platform = 'windows-x64-legacy'
            } else {
                $platform = 'windows-x64'
            }
        }
        default { throw "Unsupported Windows architecture: $architecture" }
    }

    $asset = "pingkk-$platform-cli.zip"
    $workDir = Join-Path ([IO.Path]::GetTempPath()) ('pingkk-install-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $workDir | Out-Null
    Write-Host "Downloading $asset..."
    $downloadUrl = 'https://he.sb/pingkk'
    $response = Invoke-WebRequest -UseBasicParsing -Uri "$downloadUrl/SHA256SUMS.txt"
    $expected = $null
    foreach ($line in ($response.Content -split "`n")) {
        if ($line.Trim() -match ('^([a-fA-F0-9]{64})\s+\*?' + [Regex]::Escape($asset) + '$')) {
            $expected = $Matches[1]
            break
        }
    }
    if (-not $expected) { throw "No SHA256 checksum was found for $asset." }
    $archive = Join-Path $workDir $asset
    Invoke-WebRequest -UseBasicParsing -Uri "$downloadUrl/$asset" -OutFile $archive
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expected) {
        throw 'SHA256 verification failed. Download and install again.'
    }
    $unpacked = Join-Path $workDir 'unpacked'
    Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
    $binary = Join-Path $unpacked 'bin/pingkk.exe'
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw 'The archive does not contain pingkk.exe.' }
    & $binary --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'The CLI cannot run on this system. Check the compatibility requirements in README.' }

    $installDir = Join-Path $env:LOCALAPPDATA 'Programs/pingkk'
    $binDir = Join-Path $installDir 'bin'
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $unpacked 'licenses') -Destination $installDir -Recurse -Force
    Copy-Item -LiteralPath $binary -Destination (Join-Path $binDir 'pingkk.exe') -Force

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($userPath -split ';' | Where-Object { $_ })
    $alreadyAdded = @($entries | Where-Object { [Environment]::ExpandEnvironmentVariables($_).TrimEnd('\') -ieq $binDir.TrimEnd('\') }).Count -gt 0
    if (-not $alreadyAdded) {
        [Environment]::SetEnvironmentVariable('Path', (($entries + $binDir) -join ';'), 'User')
    }
    if (-not (@($env:Path -split ';') -contains $binDir)) { $env:Path = "$binDir;$env:Path" }
    Write-Host "Installed: $binDir\pingkk.exe"
    Write-Host 'Run pingkk --help. New terminals will also find the command.'
} catch {
    throw "pingkk installation failed: $($_.Exception.Message)"
} finally {
    [Net.ServicePointManager]::SecurityProtocol = $originalTls
    if ($workDir -and (Test-Path -LiteralPath $workDir)) {
        Remove-Item -LiteralPath $workDir -Recurse -Force
    }
}
