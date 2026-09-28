[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceStage,
    [Parameter(Mandatory = $true)][string]$DestinationRoot,
    [Parameter(Mandatory = $true)][string]$InstalledLog,
    [Parameter(Mandatory = $true)][string]$SeedConfig,
    [string]$UpdatedExecutable,
    [string]$WebAssets,
    [string]$CompatibilityPackage,
    [string]$InstalledRoot = 'C:\Program Files\obs-studio',
    [string]$SystemPluginRoot = 'C:\ProgramData\obs-studio\plugins'
)
$ErrorActionPreference = 'Stop'
# Creates fresh copies only. Never launches OBS and never copies personal config.
$source = (Resolve-Path -LiteralPath $SourceStage).Path
$seed = (Resolve-Path -LiteralPath $SeedConfig).Path
$updatedBinary = if ($UpdatedExecutable) { (Resolve-Path -LiteralPath $UpdatedExecutable).Path } else { $null }
$assetSource = if ($WebAssets) { (Resolve-Path -LiteralPath $WebAssets).Path } else { $null }
$destination = [IO.Path]::GetFullPath($DestinationRoot)
if (Test-Path -LiteralPath $destination) { throw 'Destination must not exist; existing test results are preserved.' }
if ($destination.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Destination must be outside the source stage.' }
foreach ($part in @('bin', 'core', 'data')) {
    if (!(Test-Path -LiteralPath (Join-Path $source $part) -PathType Container)) { throw "Missing source stage $part" }
}
# The supplied seed is an explicit disposable fixture, never the installed
# user's configuration. Fail before copying if it can initialize a device.
foreach ($name in @('global.ini', 'user.ini')) {
    if (!(Test-Path -LiteralPath (Join-Path $seed $name) -PathType Leaf)) { throw "Missing seed $name" }
}
$sceneFiles = @(Get-ChildItem -LiteralPath (Join-Path $seed 'basic/scenes') -File | Where-Object { $_.Name -match '\.json(?:\.bak)?$' })
if (!$sceneFiles.Count) { throw 'Silent seed must contain an explicit saved scene collection.' }
$seedSourceIds = @()
foreach ($file in $sceneFiles) {
    $scene = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if (@($scene.PSObject.Properties | Where-Object { $_.Name -match '^(DesktopAudioDevice|AuxAudioDevice)' }).Count) {
        throw "Audio device channel found in seed collection $($file.Name)"
    }
    foreach ($entry in @($scene.sources) + @($scene.groups)) {
        if ($null -eq $entry) { continue }
        if ($entry.id -notin @('scene', 'group', 'color_source')) { throw "Non-silent-fixture source $($entry.id) in $($file.Name)" }
        $seedSourceIds += $entry.id
        foreach ($filter in @($entry.filters)) {
            if ($null -ne $filter -and $filter.id -ne 'color_filter') { throw "Unexpected seed filter $($filter.id) in $($file.Name)" }
        }
    }
}
$seedFiles = @(Get-ChildItem -LiteralPath $seed -Recurse -File | ForEach-Object {
    [ordered]@{ relativePath = $_.FullName.Substring($seed.Length + 1); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
})
$assetNames = @('index.html', 'app.js', 'style.css', 'bridge.mjs', 'dialog.html', 'dialog.js', 'dialog.css', 'external-drop.mjs')
if ($assetSource) {
    foreach ($name in $assetNames) {
        if (!(Test-Path -LiteralPath (Join-Path $assetSource $name) -PathType Leaf)) { throw "Missing current web asset $name" }
    }
}
$inventory = (& (Join-Path $PSScriptRoot 'plugin-inventory.ps1') -InstalledLog $InstalledLog -InstalledRoot $InstalledRoot -SystemPluginRoot $SystemPluginRoot -ForkRoot $source) | ConvertFrom-Json
$names = @('3d-effect', 'audio-wave', 'blur-filter-obs-plugin', 'gradient-source', 'logi_obs_plugin_x64',
    'obs-backgroundremoval', 'obs-composite-blur', 'obs-shaderfilter', 'scrab', 'source-clone',
    'spectralizer', 'SRBeep2', 'win-capture-audio', 'win-openvr', 'distroav')
$sidecars = @{ 'obs-backgroundremoval' = @('DirectML.dll'); 'spectralizer' = @('libfftw3-3.dll'); 'win-openvr' = @('openvr_api.dll') }
$copyPlan = @(foreach ($name in $names) {
    $matches = @($inventory.thirdPartyCandidates | Where-Object { $_.name -ieq ($name + '.dll') -and $_.officiallyLoggedLoadedByName })
    if ($matches.Count -ne 1) { throw "Expected one officially logged custom module for $name; found $($matches.Count)" }
    $binary = $matches[0].path
    $data = if ($name -eq 'distroav') { Join-Path $SystemPluginRoot 'distroav/data' } else { Join-Path $InstalledRoot ('data/obs-plugins/' + $name) }
    $extras = @(foreach ($extra in $sidecars[$name]) {
        $path = Join-Path (Split-Path -Parent $binary) $extra
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing explicitly allowed dependency $path" }
        $path
    })
    [ordered]@{ name = $name; binary = $binary; data = $data; dataPresent = (Test-Path -LiteralPath $data -PathType Container); sidecars = $extras }
})
if ($CompatibilityPackage) {
    $compatibilityRoot = (Resolve-Path -LiteralPath $CompatibilityPackage).Path
    foreach ($name in @('obs-composite-blur', 'obs-shaderfilter')) {
        $entry = @($copyPlan | Where-Object { $_.name -eq $name })[0]
        $root = Join-Path $compatibilityRoot ('plugins/' + $name)
        $binary = Join-Path $root ($name + '.dll')
        if (!(Test-Path -LiteralPath $binary -PathType Leaf) -or !(Test-Path -LiteralPath (Join-Path $root 'data') -PathType Container)) {
            throw "Incomplete compatibility package: $name"
        }
        $entry['originalBinary'] = $entry.binary
        $entry.binary = $binary
        $entry.data = Join-Path $root 'data'
        $entry.dataPresent = $true
    }
}
[IO.Directory]::CreateDirectory($destination) | Out-Null
$manifests = @()
foreach ($mode in @('native-qt', 'webview2')) {
    $stage = Join-Path $destination $mode
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    foreach ($part in @('bin', 'core', 'data')) { Copy-Item -LiteralPath (Join-Path $source $part) -Destination $stage -Recurse }
    if ($updatedBinary) { Copy-Item -LiteralPath $updatedBinary -Destination (Join-Path $stage 'bin/64bit/obs64.exe') }
    if ($assetSource) {
        foreach ($name in $assetNames) { Copy-Item -LiteralPath (Join-Path $assetSource $name) -Destination (Join-Path $stage 'data/obs-studio/webview2') }
    }
    $stageConfig = Join-Path $stage 'config/obs-studio'
    [IO.Directory]::CreateDirectory($stageConfig) | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $seed -Force) { Copy-Item -LiteralPath $item.FullName -Destination $stageConfig -Recurse }
    foreach ($file in $seedFiles) {
        if ((Get-FileHash -LiteralPath (Join-Path $stageConfig $file.relativePath) -Algorithm SHA256).Hash -ne $file.sha256) { throw 'Seed changed during staging.' }
    }
    # A prior stage's installed user plugins must never be inherited accidentally.
    # Modern plugin directories are populated exclusively from the explicit list.
    foreach ($entry in $copyPlan) {
        $target = Join-Path $stage ('plugins/' + $entry.name)
        [IO.Directory]::CreateDirectory((Join-Path $target 'data')) | Out-Null
        Copy-Item -LiteralPath $entry.binary -Destination (Join-Path $target ($entry.name + '.dll'))
        foreach ($extra in $entry.sidecars) { Copy-Item -LiteralPath $extra -Destination $target }
        if ($entry.dataPresent) {
            foreach ($item in Get-ChildItem -LiteralPath $entry.data -Force) { Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $target 'data') -Recurse }
        }
    }
    [IO.Directory]::CreateDirectory((Join-Path $stage 'audit-artifacts')) | Out-Null
    $files = @(Get-ChildItem -LiteralPath (Join-Path $stage 'plugins') -Recurse -File | ForEach-Object {
        [ordered]@{ relativePath = $_.FullName.Substring($stage.Length + 1); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    $assets = @(Get-ChildItem -LiteralPath (Join-Path $stage 'data/obs-studio/webview2') -Recurse -File | ForEach-Object {
        [ordered]@{ relativePath = $_.FullName.Substring($stage.Length + 1); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    $manifests += [ordered]@{ mode = $mode; root = $stage; executableSha256 = (Get-FileHash -LiteralPath (Join-Path $stage 'bin/64bit/obs64.exe') -Algorithm SHA256).Hash; pluginFiles = $files; webAssets = $assets; seedFiles = $seedFiles }
}
$manifest = [ordered]@{ evidence = $inventory.evidence; sourceStage = $source; copyPlan = $copyPlan; stages = $manifests;
    updatedExecutable = $updatedBinary; assetSource = $assetSource; compatibilityPackage = $CompatibilityPackage;
    seed = [ordered]@{ path = $seed; sourceIds = @($seedSourceIds | Sort-Object -Unique); collectionFiles = @($sceneFiles.Name); files = $seedFiles };
    omitted = @('obs-toolbar: not present in supplied loaded-module evidence', 'obs-ios-camera-source: not present in supplied loaded-module evidence', 'StreamFX: not present in supplied loaded-module evidence');
    scope = 'Same OBS33 binaries; logged custom modules with explicitly recorded compatibility replacements when requested; identical explicit silent fixture configuration, no personal configuration. No process launched by staging.' }
$json = $manifest | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText((Join-Path $destination 'stage-manifest.json'), $json, [Text.UTF8Encoding]::new($false))
$json
