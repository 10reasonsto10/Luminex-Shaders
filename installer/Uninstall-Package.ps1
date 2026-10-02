param(
    [string]$DestinationRoot = (Join-Path $env:LOCALAPPDATA 'LuminexShaders'),
    [switch]$RemoveSettings,
    [switch]$RemoveShaders,
    [switch]$RemoveLogs,
    [switch]$RemoveAll
)
$ErrorActionPreference = 'Stop'
$destination = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\')
$manifest = Join-Path $destination 'install-manifest.json'
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    throw "Luminex installation manifest not found at $manifest"
}
$metadata = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if ($metadata.product -ne 'Luminex Shaders') { throw 'Installation manifest does not match Luminex Shaders.' }
if ((Split-Path $destination -Leaf) -ne 'LuminexShaders') { throw 'Installation folder must be named LuminexShaders.' }
$cursor = Get-Item -LiteralPath $destination -Force
while ($null -ne $cursor) {
    if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to uninstall through a directory link.' }
    $cursor = $cursor.Parent
}
if (@(Get-ChildItem -LiteralPath $destination -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Remove directory links before uninstalling.' }
function Remove-InstalledPath([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    if ($full -ne $destination -and -not $full.StartsWith($destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe removal path: $full" }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
# Check binary locks before deleting anything; do not stop running clients.
foreach ($relative in @('Launcher\LuminexLoader.exe', 'Runtime\LuminexRuntime.dll', 'Runtime\LuminexReShadeRuntime.dll')) {
    $path = Join-Path $destination $relative
    if (Test-Path -LiteralPath $path) {
        try { $stream = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None'); $stream.Dispose() }
        catch { throw "Cannot access $path. Close the client using Luminex and retry. Nothing removed." }
    }
}
if ($RemoveAll) {
    Remove-InstalledPath $destination
    Write-Output "Deleted the entire $destination folder. Not recoverable from the Recycle Bin."
    return
}
foreach ($relative in @('Launcher\LuminexLoader.exe', 'Launcher\Launch Luminex.cmd',
    'Runtime\LuminexRuntime.dll', 'Runtime\LuminexReShadeRuntime.dll')) {
    $path = Join-Path $destination $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
}
if ($RemoveSettings) {
    Remove-InstalledPath (Join-Path $destination 'Runtime\ReShade.ini.keybind-backup')
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $destination 'Runtime') -Filter '*.ini' -File -Recurse -ErrorAction SilentlyContinue)) { Remove-InstalledPath $file.FullName }
    foreach ($relative in @('Runtime\Config', 'Runtime\Presets')) { Remove-InstalledPath (Join-Path $destination $relative) }
}
Remove-InstalledPath (Join-Path $destination 'PreviousVersions')
if ($RemoveShaders) {
    foreach ($relative in @('Runtime\ReShade', 'Runtime\ShaderBackups')) { Remove-InstalledPath (Join-Path $destination $relative) }
}
if ($RemoveLogs) {
    Remove-InstalledPath (Join-Path $destination 'Runtime\Logs')
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $destination 'Runtime') -File -ErrorAction SilentlyContinue |
        Where-Object Name -Match '^(ReShade\.log(?:\d+)?|Luminex\.log)$')) {
        Remove-InstalledPath $file.FullName
    }
}
# Keep the manifest so uninstall can later remove retained data.
Write-Output "Removed program files and selected data from $destination. Unselected data was preserved. Deleted files are not in the Recycle Bin."
