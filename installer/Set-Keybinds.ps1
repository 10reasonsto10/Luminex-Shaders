param(
    [string]$DestinationRoot = (Join-Path $env:LOCALAPPDATA 'LuminexShaders'),
    [ValidatePattern('^\d{1,3},[01],[01],[01]$')][string]$Effects,
    [ValidatePattern('^\d{1,3},[01],[01],[01]$')][string]$Overlay
)
$ErrorActionPreference = 'Stop'
$path = Join-Path $DestinationRoot 'Runtime\ReShade.ini'
$content = [IO.File]::ReadAllText($path)
function Current-Binding([string]$name, [string]$fallback) {
    $match = [regex]::Match($content, '(?m)^' + $name + '=(.*)\r?$')
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return $fallback
}
if (-not $Effects -and -not $Overlay) { Write-Output 'Existing keybinds kept.'; return }
foreach ($binding in @($Effects, $Overlay)) {
    if ($binding -and [int]($binding.Split(',')[0]) -gt 255) { throw 'Key code must be between 0 and 255.' }
}
$effectiveEffects = if ($Effects) { $Effects } else { Current-Binding 'KeyEffects' '0,0,0,0' }
$effectiveOverlay = if ($Overlay) { $Overlay } else { Current-Binding 'KeyOverlay' '36,0,0,0' }
if ($effectiveEffects -eq $effectiveOverlay -and $effectiveEffects -ne '0,0,0,0') { throw 'Choose different shortcuts for effects and the menu. No changes saved.' }
$lines = [Collections.Generic.List[string]]::new()
$lines.AddRange([string[]]($content -split '\r?\n'))
foreach ($entry in @(@('KeyEffects', $Effects), @('KeyOverlay', $Overlay))) {
    if (-not $entry[1]) { continue }
    $start = -1; $end = $lines.Count
    for ($i=0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\[INPUT\]\s*$') { $start=$i; continue }
        if ($start -ge 0 -and $lines[$i] -match '^\[') { $end=$i; break }
    }
    if ($start -lt 0) { $lines.Add('[INPUT]'); $start=$lines.Count-1; $end=$lines.Count }
    for ($i=$end-1; $i -gt $start; $i--) { if ($lines[$i] -match ('^' + $entry[0] + '=')) { $lines.RemoveAt($i) } }
    $lines.Insert($start+1, ($entry[0] + '=' + $entry[1]))
}
Copy-Item -LiteralPath $path -Destination ($path + '.keybind-backup') -Force
[IO.File]::WriteAllText($path, ($lines -join "`r`n"), [Text.UTF8Encoding]::new($false))
Write-Output 'Keybinds saved. Already loaded clients keep their current settings. You can also change shortcuts in ReShade Settings.'
