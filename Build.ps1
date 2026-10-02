[CmdletBinding()]
param(
    [string]$WorkDirectory,
    [string]$OutputDirectory,
    [ValidateRange(1,64)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
if (-not [Environment]::Is64BitOperatingSystem -or $env:OS -ne 'Windows_NT') { throw 'Build on Windows x64.' }
function Run([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
$dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue).Source
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $git) { throw 'Install Git for Windows and open a new PowerShell window.' }
if (-not $dotnet) { throw 'Install the .NET 10 SDK and open a new PowerShell window.' }
if (-not $python) { throw 'Install Python 3.10 or newer with pip and add it to PATH.' }
Run $python @('-c','import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)')
if (-not ((& $dotnet --list-sdks) -match '^10\.')) { throw 'The .NET 10 SDK is required.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 or newer with Desktop development with C++, Windows SDK, and CMake tools.' }
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json | Select-Object -First 1)
if (-not $vs) { throw 'Visual Studio C++ x64 build tools were not found.' }
$cmake = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $cmake) { throw 'Install the C++ CMake tools component or CMake 3.24 or newer.' }
$major = [int]($vs.installationVersion.Split('.')[0])
$generator = switch ($major) { 17 { 'Visual Studio 17 2022' } 18 { 'Visual Studio 18 2026' } default { throw 'This script supports Visual Studio 2022 and 2026.' } }
$kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (-not $kitsRoot) { $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$fxc = Get-ChildItem -LiteralPath (Join-Path $kitsRoot 'bin') -Directory -ErrorAction SilentlyContinue |
    Where-Object Name -Match '^10\.\d+\.\d+\.\d+$' | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\fxc.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $fxc) { throw 'Install a Windows 10 or 11 SDK with the x64 shader compiler (fxc.exe).' }
if (-not $WorkDirectory) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $id = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root)))).Replace('-','').Substring(0,12) }
    finally { $sha.Dispose() }
    $WorkDirectory = Join-Path $env:LOCALAPPDATA ('LuminexBuild\' + $id)
}
$work = [IO.Path]::GetFullPath($WorkDirectory)
if ($work -eq $root -or $work.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Choose a work directory outside the source folder.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $work 'Release' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ($output -eq $root -or $output.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Choose an output directory outside the source folder.' }
New-Item -ItemType Directory -Path $work -Force | Out-Null
$originalPath = $env:PATH
try {
# ReShade invokes python and pip during graphics-header generation. Keep their packages local to this build.
$venv = Join-Path $work 'python'
if (-not (Test-Path -LiteralPath (Join-Path $venv 'Scripts\python.exe'))) { Run $python @('-m','venv',$venv) }
$env:PATH = (Join-Path $venv 'Scripts') + ';' + $originalPath
$reshade = Join-Path $work 'reshade'
$commit = '358c345ca2fe64f86e67c694f8379c356627adcb'
Write-Host 'Fetching the pinned ReShade source and its dependencies...'
if (-not (Test-Path -LiteralPath $reshade)) {
    Run $git @('init', $reshade)
    Run $git @('-C', $reshade, 'remote', 'add', 'origin', 'https://github.com/crosire/reshade.git')
}
if (-not (Test-Path -LiteralPath (Join-Path $reshade '.git'))) { throw 'The cached ReShade folder is not a Git checkout. Choose a new work directory.' }
Run $git @('-C', $reshade, 'config', 'core.longpaths', 'true')
$head = & $git -C $reshade rev-parse --verify --quiet HEAD 2>$null
if ($LASTEXITCODE -ne 0) {
    Run $git @('-C', $reshade, 'fetch', '--depth=1', 'origin', $commit)
    Run $git @('-C', $reshade, 'checkout', '--detach', 'FETCH_HEAD')
} elseif ($head -ne $commit) { throw 'The cached ReShade revision differs. Choose a new work directory.' }
Run $git @('-C', $reshade, 'submodule', 'update', '--init', '--recursive', '--jobs', "$Jobs")
$patch = Join-Path $root 'patches\reshade-embedded.patch'
$oldPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $git -C $reshade apply --reverse --check $patch 2>$null
$patched = $LASTEXITCODE -eq 0
$ErrorActionPreference = $oldPreference
if (-not $patched) { Run $git @('-C', $reshade, 'apply', '--check', $patch); Run $git @('-C', $reshade, 'apply', $patch) }
Copy-Item -LiteralPath (Join-Path $root 'patches\dll_main_embedded.cpp') -Destination (Join-Path $reshade 'source\dll_main_embedded.cpp') -Force
# ReShade's Visual Studio project normally generates this ignored header.
# Its CMake build does not. These version numbers match the pinned source used by Luminex.
$versionHeader = @'
#pragma once
#define VERSION_FULL 6.8.0.0
#define VERSION_MAJOR 6
#define VERSION_MINOR 8
#define VERSION_REVISION 0
#define VERSION_BUILD 0
#define VERSION_STRING_FILE "6.8.0.0"
#define VERSION_STRING_PRODUCT "6.8.0 UNOFFICIAL"
'@
$versionPath = Join-Path $reshade 'res\version.h'
if (-not (Test-Path -LiteralPath $versionPath) -or
    ((Get-Content -LiteralPath $versionPath -Raw).Trim() -replace "`r`n", "`n") -ne ($versionHeader.Trim() -replace "`r`n", "`n")) {
    $versionHeader | Set-Content -LiteralPath $versionPath -Encoding ASCII
}
# The upstream Visual Studio project also compiles these built-in UI shaders.
$shaderProfiles = [ordered]@{
    'copy_ps'='ps_4_0'; 'fullscreen_vs'='vs_4_0'; 'imgui_ps_3_0'='ps_3_0';
    'imgui_ps_4_0'='ps_4_0'; 'imgui_vs_3_0'='vs_3_0'; 'imgui_vs_4_0'='vs_4_0'; 'mipmap_cs_5_0'='cs_5_0'
}
foreach ($shader in $shaderProfiles.GetEnumerator()) {
    $shaderSource = Join-Path $reshade ('res\shaders\' + $shader.Key + '.hlsl')
    $shaderBinary = Join-Path $reshade ('res\shaders\' + $shader.Key + '.cso')
    Run $fxc @('/nologo','/T',$shader.Value,'/E','main','/O3','/Fo',$shaderBinary,$shaderSource)
}
$native = Join-Path $work 'native'
$embedded = Join-Path $work 'reshade-build'
Write-Host 'Building the native runtime and loader...'
Run $cmake @('-S',$root,'-B',$native,'-G',$generator,'-A','x64',"-DCMAKE_GENERATOR_INSTANCE=$($vs.installationPath)","-DLUMINEX_RESHADE_SOURCE_DIR=$reshade")
Run $cmake @('--build',$native,'--config','Release','--target','LuminexRuntime','LuminexLoader','--parallel',"$Jobs")
Run $cmake @('-S',$reshade,'-B',$embedded,'-G',$generator,'-A','x64',"-DCMAKE_GENERATOR_INSTANCE=$($vs.installationPath)",'-DRESHADE_EMBEDDED_LIBRARY=ON')
Run $cmake @('--build',$embedded,'--config','Release','--parallel',"$Jobs")
$stage = Join-Path $work ('package-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $stage 'resources'
$published = Join-Path $stage 'manager'
$artifacts = Join-Path $work 'dotnet'
$nuget = Join-Path $work 'nuget'
$project = Join-Path $root 'manager\LuminexManager.csproj'
[xml]$projectXml = Get-Content -LiteralPath $project -Raw
$version = [string]($projectXml.Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1)
Write-Host 'Publishing the manager and collecting dependency notices...'
Run $dotnet @('publish',$project,'-c','Release','-r','win-x64','--self-contained','true','--artifacts-path',$artifacts,'-o',$published,"-p:RestorePackagesPath=$nuget")
function Include-File([string]$Source, [string]$Relative) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required build file missing: $Source" }
    $target = Join-Path $package $Relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
}
foreach ($file in @('Install-Package.ps1','Uninstall-Package.ps1','Install-ShaderPacks.ps1','Set-Keybinds.ps1','ShaderPackages.json')) {
    Include-File (Join-Path $root ('installer\' + $file)) ('installer\' + $file)
}
Include-File (Join-Path $root 'LICENSE') 'LICENSE'
Include-File (Join-Path $root 'LICENSE') 'Licenses\Luminex-LICENSE.txt'
Include-File (Join-Path $native 'bin\Release\LuminexLoader.exe') 'Payload\Launcher\LuminexLoader.exe'
Include-File (Join-Path $native 'bin\Release\LuminexRuntime.dll') 'Payload\Runtime\LuminexRuntime.dll'
Include-File (Join-Path $embedded 'Release\LuminexReShadeRuntime.dll') 'Payload\Runtime\LuminexReShadeRuntime.dll'
Include-File (Join-Path $root 'assets\Config\Luminex.ini') 'Payload\Runtime\Config\Luminex.ini'
Include-File (Join-Path $root 'assets\Config\ReShade.ini') 'Payload\Runtime\ReShade.ini'
Include-File (Join-Path $root 'assets\Presets\ReShadePreset.ini') 'Payload\Runtime\ReShadePreset.ini'
function Copy-Notice([string]$Source, [string]$Relative) { Include-File $Source ('Licenses\' + $Relative) }
Copy-Notice (Join-Path $reshade 'LICENSE.md') 'ReShade\LICENSE.md'
Copy-Notice (Join-Path $native '_deps\imgui-src\LICENSE.txt') 'Luminex-ImGui\LICENSE.txt'
Copy-Notice (Join-Path $native '_deps\minhook-src\LICENSE.txt') 'Luminex-MinHook\LICENSE.txt'
$deps = Join-Path $reshade 'deps'
foreach ($dependency in @('d3d12','d3d12on7','d3d911on12','glad','imgui','jxl_simple_lossless','minhook','openvr','openxr','spirv','stb','utfcpp','vma')) {
    $sourceRoot = Join-Path $deps $dependency
    $notices = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File | Where-Object Name -Match '^(LICENSE|COPYING|NOTICE|AUTHORS|COPYRIGHT|OFL)(\.|$)')
    if (-not $notices.Count) { throw "Missing notices for $dependency" }
    foreach ($notice in $notices) { Copy-Notice $notice.FullName ('ReShade-deps/' + $dependency + '/' + $notice.FullName.Substring($sourceRoot.Length + 1)) }
}
$fpng = Get-Content -LiteralPath (Join-Path $deps 'fpng/src/fpng.cpp') -Raw
$notice = [regex]::Match($fpng, '(?s)/\*\s*(This is free and unencumbered software released into the public domain\..*?)\*/\s*$')
if (-not $notice.Success) { throw 'Missing fpng notice.' }
$notice.Groups[1].Value.Trim() | Set-Content -LiteralPath (Join-Path $package 'Licenses\fpng-Unlicense.txt') -Encoding UTF8
$khr = Get-Content -LiteralPath (Join-Path $deps 'glad/target/include/KHR/khrplatform.h') -Raw
$notice = [regex]::Match($khr, '(?s)/\*\s*(\*\* Copyright.*?THE MATERIALS\.)\s*\*/')
if (-not $notice.Success) { throw 'Missing Khronos notice.' }
$notice.Groups[1].Value | Set-Content -LiteralPath (Join-Path $package 'Licenses\Khronos-platform.txt') -Encoding UTF8
$depsFile = Get-ChildItem -LiteralPath (Join-Path $artifacts 'bin') -Recurse -Filter 'LuminexManager.deps.json' | Select-Object -First 1
$assetsFile = Get-ChildItem -LiteralPath (Join-Path $artifacts 'obj') -Recurse -Filter 'project.assets.json' | Select-Object -First 1
if (-not $depsFile -or -not $assetsFile) { throw 'Missing .NET dependency metadata.' }
$depsJson = Get-Content -LiteralPath $depsFile.FullName -Raw | ConvertFrom-Json
$assetsJson = Get-Content -LiteralPath $assetsFile.FullName -Raw | ConvertFrom-Json
$runtimePacks = @($depsJson.libraries.PSObject.Properties.Name | Where-Object { $_ -like 'runtimepack.*' })
if ($runtimePacks.Count -lt 2) { throw 'Expected self-contained .NET and Windows Desktop runtime packs.' }
foreach ($pack in $runtimePacks) {
    $relative = $pack.Substring('runtimepack.'.Length).ToLowerInvariant()
    $sourceRoot = $null
    foreach ($cache in $assetsJson.packageFolders.PSObject.Properties.Name) {
        $candidate = Join-Path $cache $relative
        if (Test-Path -LiteralPath $candidate -PathType Container) { $sourceRoot = $candidate; break }
    }
    if (-not $sourceRoot) { throw "Missing runtime pack: $relative" }
    $notices = @(Get-ChildItem -LiteralPath $sourceRoot -File | Where-Object Name -Match 'LICENSE|THIRD.PARTY.NOTICE')
    if (-not ($notices | Where-Object Name -Match 'LICENSE')) { throw "Missing runtime pack license: $relative" }
    foreach ($notice in $notices) { Copy-Notice $notice.FullName ('dotnet/' + $relative + '/' + $notice.Name) }
}
foreach ($notice in Get-ChildItem -LiteralPath (Join-Path $root 'assets\ThirdPartyNotices') -File) { Copy-Notice $notice.FullName $notice.Name }
$inventory = @(Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($package.Length + 1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
[ordered]@{version=$version;shaderDelivery='direct-author-download';files=$inventory} | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $package 'installer\release-manifest.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = Join-Path $stage 'embedded.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($package, $archive)
Write-Host 'Embedding installation resources in the final executable...'
Run $dotnet @('publish',$project,'-c','Release','-r','win-x64','--self-contained','true','--artifacts-path',$artifacts,'-o',$published,"-p:RestorePackagesPath=$nuget","-p:LuminexPackage=$archive",'-p:EnableCompressionInSingleFile=true')
New-Item -ItemType Directory -Path $output -Force | Out-Null
$installer = Join-Path $output 'LuminexSetup.exe'
Copy-Item -LiteralPath (Join-Path $published 'LuminexManager.exe') -Destination $installer -Force
Write-Host "Built: $installer"
Write-Host "SHA256: $((Get-FileHash -LiteralPath $installer).Hash)"
} finally { $env:PATH = $originalPath }
