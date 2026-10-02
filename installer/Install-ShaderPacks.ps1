param(
    [string]$DestinationRoot = (Join-Path $env:LOCALAPPDATA 'LuminexShaders'),
    [string[]]$PackageIds,
    [switch]$All,
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ShaderPackages.json') -Raw | ConvertFrom-Json
$packages = @($catalog.packages)
if ($ListOnly) { $packages | Select-Object id, name, repositoryUrl; return }

function Get-SafeChild([string]$parent, [string]$relative) {
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)|:') {
        throw "Unsafe package path: $relative"
    }
    $base = [IO.Path]::GetFullPath($parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $path = [IO.Path]::GetFullPath((Join-Path $base $relative))
    if (-not $path.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes destination: $relative" }
    return $path
}

if ($All) { $PackageIds = @($packages | ForEach-Object id) }
$PackageIds = @($PackageIds | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | Select-Object -Unique)
if ($PackageIds.Count -eq 0) { Write-Output 'Shader downloads skipped. Open Shader packs in the manager whenever you want to add effects.'; return }
foreach ($id in $PackageIds) { if ($id -notin @($packages.id)) { throw "Unknown shader package ID: $id" } }
# These are shared headers, not an unrelated optional effect dependency.
$PackageIds = @('00') + @($PackageIds | Where-Object { $_ -ne '00' })
$runtime = Join-Path ([IO.Path]::GetFullPath($DestinationRoot)) 'Runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
$failures = @()
$installed = @()
$index = 0

foreach ($id in $PackageIds) {
    $pack = $packages | Where-Object id -eq $id | Select-Object -First 1
    $index++
    Write-Output "[$index/$($PackageIds.Count)] Downloading $($pack.name) from $($pack.downloadUrl)"
    $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $work = Join-Path $tempBase ('LuminexShaderDownload-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    $zip = $null
    try {
        $uri = [Uri]$pack.downloadUrl
        if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'github.com' -or $uri.AbsolutePath -notmatch '/archive/.+\.zip$') {
            throw "Unapproved download origin for $($pack.name)"
        }
        $archivePath = Join-Path $work 'package.zip'
        $ProgressPreference = 'SilentlyContinue'
        $response = Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $archivePath -PassThru -TimeoutSec 180
        $finalUri = $response.BaseResponse.ResponseUri
        if ($finalUri.Scheme -ne 'https' -or $finalUri.Host -notin @('github.com', 'codeload.github.com')) {
            throw 'Unexpected download redirect.'
        }
        $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
        $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
        if ($zip.Entries.Count -gt 30000 -or ($zip.Entries | Measure-Object Length -Sum).Sum -gt 1GB) {
            throw 'Archive exceeds shader package limits.'
        }
        $entries = @($zip.Entries | Where-Object { $_.Name })
        # Select the shallowest Shaders folder with effects, preserving its include layout.
        $shaderRoots = @($entries | Where-Object { $_.FullName -match '(?i)\.fx$' } | ForEach-Object {
            if ($_.FullName -match '^(.*?/Shaders/)') { $matches[1] }
        } | Sort-Object -Unique | Sort-Object Length)
        if ($shaderRoots.Count) { $shaderRoot = $shaderRoots[0] }
        else {
            $first = $entries | Where-Object { $_.FullName -match '(?i)\.fx$' } | Sort-Object { $_.FullName.Length } | Select-Object -First 1
            if (-not $first) { throw 'Archive contains no ReShade effects.' }
            $shaderRoot = $first.FullName.Substring(0, $first.FullName.LastIndexOf('/') + 1)
        }
        $textureRoots = @($entries | ForEach-Object { if ($_.FullName -match '^(.*?/Textures/)') { $matches[1] } } | Sort-Object -Unique | Sort-Object Length)
        $textureRoot = if ($textureRoots.Count) { $textureRoots[0] } else { $null }
        $plan = @()
        $seen = @{}
        foreach ($entry in $entries) {
            $name = $entry.FullName.Replace('\', '/')
            # Validate even skipped archive paths before interpreting them.
            $null = Get-SafeChild $work $name
            $relative = $null
            $extension = [IO.Path]::GetExtension($name).ToLowerInvariant()
            if ($name.StartsWith($shaderRoot, [StringComparison]::Ordinal) -and $extension -in @('.fx', '.fxh', '.h', '.hlsl')) {
                if ($entry.Name -in @($pack.excludedEffects)) { continue }
                $relative = 'ReShade/Shaders/' + $pack.shaderPath + '/' + $name.Substring($shaderRoot.Length)
            } elseif ($textureRoot -and $name.StartsWith($textureRoot, [StringComparison]::Ordinal) -and $extension -in @('.png', '.jpg', '.jpeg', '.bmp', '.dds', '.tga')) {
                $relative = 'ReShade/Textures/' + $pack.texturePath + '/' + $name.Substring($textureRoot.Length)
            } elseif ($entry.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE|README)(\.|$)') {
                $relative = 'ReShade/Licenses/Downloaded/' + $id + '/' + $name
            }
            if (-not $relative) { continue }
            $relative = $relative -replace '/+', '/'
            if ($seen.ContainsKey($relative)) { throw "Duplicate archive target: $relative" }
            $seen[$relative] = $true
            $staged = Get-SafeChild (Join-Path $work 'staged') $relative
            $target = Get-SafeChild $runtime $relative
            New-Item -ItemType Directory -Path (Split-Path $staged -Parent) -Force | Out-Null
            $inputStream = $entry.Open()
            try {
                $outputStream = [IO.File]::Create($staged)
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
            } finally { $inputStream.Dispose() }
            $plan += [pscustomobject]@{ relative=$relative; staged=$staged; target=$target }
        }
        if (-not ($plan | Where-Object relative -Match '\.fx$')) { throw 'No installable effects remained after filtering.' }
        $zip.Dispose(); $zip = $null
        $backupRoot = Join-Path $runtime ('ShaderBackups/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $id + '-' + [Guid]::NewGuid().ToString('N'))
        $written = @()
        try {
            foreach ($file in $plan) {
                $backup = $null
                if (Test-Path -LiteralPath $file.target -PathType Leaf) {
                    if ((Get-FileHash $file.target).Hash -eq (Get-FileHash $file.staged).Hash) { continue }
                    $backup = Get-SafeChild $backupRoot $file.relative
                    New-Item -ItemType Directory -Path (Split-Path $backup -Parent) -Force | Out-Null
                    Copy-Item -LiteralPath $file.target -Destination $backup
                }
                $written += [pscustomobject]@{ target=$file.target; backup=$backup }
                New-Item -ItemType Directory -Path (Split-Path $file.target -Parent) -Force | Out-Null
                Copy-Item -LiteralPath $file.staged -Destination $file.target -Force
            }
        } catch {
            foreach ($file in $written) {
                if ($file.backup) { Copy-Item -LiteralPath $file.backup -Destination $file.target -Force }
                elseif (Test-Path -LiteralPath $file.target -PathType Leaf) { Remove-Item -LiteralPath $file.target }
            }
            throw
        }
        $record = [ordered]@{ id=$id; name=$pack.name; repositoryUrl=$pack.repositoryUrl; downloadUrl=$pack.downloadUrl; archiveSha256=$archiveHash; installedUtc=[DateTime]::UtcNow.ToString('o'); files=@($plan.relative) }
        $recordDirectory = Join-Path $runtime 'ReShade/DownloadedPackages'
        New-Item -ItemType Directory -Path $recordDirectory -Force | Out-Null
        $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $recordDirectory "$id.json") -Encoding UTF8
        $installed += $pack.name
        Write-Output "Installed $($pack.name). Existing changed files were backed up under Runtime/ShaderBackups."
    } catch {
        $failures += "$($pack.name): $($_.Exception.Message)"
        Write-Warning $failures[-1]
    } finally {
        if ($zip) { $zip.Dispose() }
        $resolved = [IO.Path]::GetFullPath($work)
        if ($resolved.StartsWith($tempBase + '\LuminexShaderDownload-', [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path $resolved -Parent) -eq $tempBase) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}
Write-Output "Installed $($installed.Count) selected packs. Open ReShade and press Reload to see newly downloaded effects."
if ($failures.Count) { throw "Some downloads failed. Open Shader packs in the manager to retry.`n$($failures -join "`n")" }
