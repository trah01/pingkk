param(
    [Parameter(Mandatory)][string]$SourceDir,
    [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceDir).Path
$payload = Join-Path $PWD "build-portable-payload-$Architecture"
New-Item -ItemType Directory -Force $payload | Out-Null
$files = @(Get-ChildItem -LiteralPath $source -Recurse -File | Sort-Object FullName)
if (-not (Test-Path -LiteralPath (Join-Path $source 'pingkk-gui.exe'))) { throw 'Missing GUI' }
$ddf = @(
    '.OPTION EXPLICIT', '.Set Cabinet=on', '.Set Compress=on',
    '.Set CompressionType=LZX', '.Set CompressionMemory=21',
    '.Set MaxDiskSize=0', '.Set MaxCabinetSize=0', '.Set FolderSizeThreshold=0',
    '.Set CabinetNameTemplate=payload.cab', ".Set DiskDirectoryTemplate=$payload",
    ".Set InfFileName=$payload\payload.inf", ".Set RptFileName=$payload\payload.rpt"
)
$header = @('struct PayloadFile { const wchar_t* name; const wchar_t* path; };',
            'static const PayloadFile payloadFiles[] = {')
$index = 0
foreach ($file in $files) {
    $name = 'f{0:D4}' -f $index++
    # ASCII cabinet member names also handle Chinese license filenames reliably.
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $payload $name)
    $ddf += ('"{0}" {1}' -f (Join-Path $payload $name), $name)
    $relative = $file.FullName.Substring($source.Length + 1).Replace('\', '\\').Replace('"', '\"')
    $header += ('    {{L"{0}", L"{1}"}},' -f $name, $relative)
}
$header += '};'
$ddf | Set-Content -Encoding ascii (Join-Path $payload 'payload.ddf')
$header | Set-Content -Encoding utf8 (Join-Path $payload 'payload.h')
& makecab.exe /F (Join-Path $payload 'payload.ddf')
if ($LASTEXITCODE -ne 0) { throw 'Cabinet compression failed' }
$cabPath = (Join-Path $payload 'payload.cab').Replace('\', '/')
$iconPath = (Join-Path $PWD 'assets/pingkk-icon.ico').Replace('\', '/')
@("101 RCDATA `"$cabPath`"", "1 ICON `"$iconPath`"") |
    Set-Content -Encoding utf8 (Join-Path $payload 'payload.rc')
$cmakeArch = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x64' }
$cmakePayload = $payload.Replace('\', '/')
$toolset = if ($Architecture -eq 'x64') { 'v142' } else { 'v143' }
cmake -S packaging/windows/portable -B "build-portable-$Architecture" -A $cmakeArch -T $toolset "-DPAYLOAD_DIR:PATH=$cmakePayload"
if ($LASTEXITCODE -ne 0) { throw 'Launcher configuration failed' }
cmake --build "build-portable-$Architecture" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed' }
Copy-Item "build-portable-$Architecture/Release/pingkk_portable.exe" "pingkk-windows-$Architecture-portable.exe"
