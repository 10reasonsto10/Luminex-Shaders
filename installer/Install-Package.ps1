param([string]$DestinationRoot = (Join-Path $env:LOCALAPPDATA 'LuminexShaders'))

$ErrorActionPreference = 'Stop'
$packageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$payload = Join-Path $packageRoot 'Payload'
$destination = [IO.Path]::GetFullPath($DestinationRoot)
$launcher = Join-Path $destination 'Launcher'
$runtime = Join-Path $destination 'Runtime'

if (-not [Environment]::Is64BitOperatingSystem -or
    ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -and $env:PROCESSOR_ARCHITEW6432 -ne 'AMD64')) {
    throw 'This release requires Windows on an x64 (Intel/AMD) computer.'
}
if ((Split-Path $destination -Leaf) -ne 'LuminexShaders') {
    throw 'Choose an installation folder named LuminexShaders so it can also be safely uninstalled.'
}
# Validate the complete extracted package before changing an existing installation.
# This detects damaged/incomplete downloads; it is not a publisher signature.
$releaseManifest = Join-Path $PSScriptRoot 'release-manifest.json'
if (-not (Test-Path -LiteralPath $releaseManifest -PathType Leaf)) {
    throw 'Release manifest is missing. Download LuminexSetup.exe again.'
}
$release = Get-Content -LiteralPath $releaseManifest -Raw | ConvertFrom-Json
if (-not $release.version -or -not @($release.files).Count) { throw 'Invalid release manifest.' }
$listed = @{}
foreach ($entry in $release.files) {
    $relative = [string]$entry.path
    if (-not $relative -or [IO.Path]::IsPathRooted($relative) -or
        $relative -match '(^|[\\/])\.\.([\\/]|$)|:' -or $listed.ContainsKey($relative)) {
        throw "Invalid release manifest path: $relative"
    }
    $path = [IO.Path]::GetFullPath((Join-Path $packageRoot $relative))
    if (-not $path.StartsWith($packageRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Package integrity check failed: $relative. Download LuminexSetup.exe again."
    }
    $listed[$relative] = $true
}

foreach ($relative in @('Launcher\LuminexLoader.exe',
    'Runtime\LuminexRuntime.dll', 'Runtime\LuminexReShadeRuntime.dll',
    'Runtime\ReShade.ini', 'Runtime\ReShadePreset.ini', 'Runtime\Config\Luminex.ini')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $relative) -PathType Leaf)) {
        throw "Incomplete release package. Missing Payload\$relative"
    }
    if (-not $listed.ContainsKey(('Payload/' + $relative.Replace('\', '/')))) {
        throw "Payload file is missing from the release manifest: $relative"
    }
}
# Catch in-use files before replacing any program files or notices.
foreach ($relative in @('Launcher\LuminexLoader.exe',
    'Runtime\LuminexRuntime.dll', 'Runtime\LuminexReShadeRuntime.dll')) {
    $target = Join-Path $destination $relative
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        try { $stream = [IO.File]::Open($target, 'Open', 'ReadWrite', 'None'); $stream.Dispose() }
        catch { throw "Close the client or program using $target, then run Setup again. No installation files changed." }
    }
}
New-Item -ItemType Directory -Path $launcher, $runtime, (Join-Path $runtime 'ReShade\Shaders'), (Join-Path $runtime 'ReShade\Textures') -Force | Out-Null
# Preserve existing user settings and presets. Release files replace only program-owned content.
foreach ($relative in @('Launcher\LuminexLoader.exe',
    'Runtime\LuminexRuntime.dll', 'Runtime\LuminexReShadeRuntime.dll')) {
    $source = Join-Path $payload $relative
    $target = Join-Path $destination $relative
    if ((Test-Path -LiteralPath $target -PathType Leaf) -and
        (Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $target).Hash) {
        continue
    }
    $previous = $null
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $backupDirectory = Join-Path $destination 'PreviousVersions'
        New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
        $previous = Join-Path $backupDirectory ((Split-Path $relative -Leaf) + '.' + [Guid]::NewGuid().ToString('N') + '.bak')
        Move-Item -LiteralPath $target -Destination $previous
    }
    try {
        Copy-Item -LiteralPath $source -Destination $target
    } catch {
        if ($previous -and -not (Test-Path -LiteralPath $target)) {
            Move-Item -LiteralPath $previous -Destination $target
        }
        throw
    }
}
foreach ($relative in @('Runtime\ReShade.ini', 'Runtime\ReShadePreset.ini', 'Runtime\Config\Luminex.ini')) {
    $target = Join-Path $destination $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    if (-not (Test-Path -LiteralPath $target)) {
        Copy-Item -LiteralPath (Join-Path $payload $relative) -Destination $target
    }
}
foreach ($relative in @('Runtime\ReShade\Shaders', 'Runtime\ReShade\Textures',
    'Runtime\ReShade\Licenses')) {
    $source = Join-Path $payload $relative
    if (Test-Path -LiteralPath $source -PathType Container) {
        $target = Join-Path $destination $relative
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
    }
}
foreach ($relative in @('Runtime\ReShade\packages.json')) {
    $source = Join-Path $payload $relative
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        $target = Join-Path $destination $relative
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
}
New-Item -ItemType Directory -Path (Join-Path $runtime 'Logs') -Force | Out-Null
$manifest = [ordered]@{
    product = 'Luminex Shaders'
    version = $release.version
    shaderDelivery = 'direct-author-download'
    installedUtc = [DateTime]::UtcNow.ToString('o')
    runtimeSha256 = (Get-FileHash -LiteralPath (Join-Path $runtime 'LuminexRuntime.dll') -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'install-manifest.json') -Encoding UTF8
Write-Output "Installed Luminex Shaders to $destination. Already running clients keep their currently loaded runtime."
