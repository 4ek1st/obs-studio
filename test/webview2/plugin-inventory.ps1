[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InstalledLog,
    [string]$InstalledRoot = 'C:\Program Files\obs-studio',
    [string]$SystemPluginRoot = 'C:\ProgramData\obs-studio\plugins',
    [string]$ForkRoot = (Join-Path $PSScriptRoot '../../build_webview2_full/rundir/RelWithDebInfo'),
    [string]$ReportPath
)

# Static inspection only. No LoadLibrary, OBS launch, plugin execution, or config access.
# InstalledLog also accepts an explicitly supplied Loaded Modules excerpt; its hash
# identifies the supplied evidence, not a full original log when it is an excerpt.
$ErrorActionPreference = 'Stop'

function Read-PortableExecutable([string]$Path) {
    try {
        [byte[]]$bytes = [IO.File]::ReadAllBytes($Path)
        function U16([long]$offset) {
            if ($offset -lt 0 -or $offset + 2 -gt $bytes.Length) { throw 'Truncated PE' }
            [BitConverter]::ToUInt16($bytes, [int]$offset)
        }
        function U32([long]$offset) {
            if ($offset -lt 0 -or $offset + 4 -gt $bytes.Length) { throw 'Truncated PE' }
            [BitConverter]::ToUInt32($bytes, [int]$offset)
        }
        if ((U16 0) -ne 0x5a4d) { throw 'Missing DOS signature' }
        $pe = U32 60
        if ((U32 $pe) -ne 0x4550) { throw 'Missing PE signature' }
        $machine = U16 ($pe + 4)
        $sectionCount = U16 ($pe + 6)
        if ($sectionCount -gt 96) { throw 'Excessive section count' }
        $optional = $pe + 24
        $optionalSize = U16 ($pe + 20)
        $sections = $optional + $optionalSize
        $magic = U16 $optional
        if ($magic -eq 0x20b) { $directory = $optional + 112 }
        elseif ($magic -eq 0x10b) { $directory = $optional + 96 }
        else { throw 'Unknown optional header' }
        function FileOffset([long]$rva) {
            for ($i = 0; $i -lt $sectionCount; $i++) {
                $s = $sections + $i * 40
                $start = U32 ($s + 12)
                $length = [Math]::Max((U32 ($s + 8)), (U32 ($s + 16)))
                if ($rva -ge $start -and $rva -lt $start + $length) {
                    $offset = $rva - $start + (U32 ($s + 20))
                    if ($offset -ge $bytes.Length) { throw 'RVA outside file' }
                    return $offset
                }
            }
            throw 'RVA outside sections'
        }
        function AsciiAtRva([long]$rva) {
            $start = FileOffset $rva
            $end = $start
            while ($end -lt $bytes.Length -and $end - $start -lt 4096 -and $bytes[$end] -ne 0) { $end++ }
            if ($end -eq $bytes.Length -or $end - $start -ge 4096) { throw 'Unterminated PE string' }
            [Text.Encoding]::ASCII.GetString($bytes, [int]$start, [int]($end - $start))
        }
        $exports = @()
        $exportRva = U32 $directory
        if ($exportRva) {
            $export = FileOffset $exportRva
            $count = U32 ($export + 24)
            if ($count -gt 100000) { throw 'Excessive export count' }
            if ($count) {
                $names = FileOffset (U32 ($export + 32))
                $exports = @(for ($i = 0; $i -lt $count; $i++) { AsciiAtRva (U32 ($names + $i * 4)) })
            }
        }
        $imports = @()
        foreach ($kind in @('direct', 'delay')) {
            $index = if ($kind -eq 'direct') { 1 } else { 13 }
            if ($directory + $index * 8 + 8 -gt $optional + $optionalSize) { continue }
            $rva = U32 ($directory + $index * 8)
            if (!$rva) { continue }
            $offset = FileOffset $rva
            $stride = if ($kind -eq 'direct') { 20 } else { 32 }
            $nameOffset = if ($kind -eq 'direct') { 12 } else { 4 }
            for ($i = 0; $i -lt 4096; $i++) {
                $entry = $offset + $i * $stride
                $nameRva = U32 ($entry + $nameOffset)
                if (!$nameRva) { break }
                if ($kind -eq 'delay' -and ((U32 $entry) -band 1) -ne 1) { throw 'Unsupported VA-based delay imports' }
                $imports += [ordered]@{ name = (AsciiAtRva $nameRva); kind = $kind }
            }
            if ($i -eq 4096) { throw 'Excessive import count' }
        }
        [ordered]@{
            machine = ('0x{0:x4}' -f $machine)
            isObsModule = ($exports -contains 'obs_module_load' -and $exports -contains 'obs_module_set_pointer' -and $exports -contains 'obs_module_ver')
            imports = @($imports)
            error = $null
        }
    } catch { [ordered]@{ machine = $null; isObsModule = $null; imports = @(); error = $_.Exception.Message } }
}

function DllFiles([string]$Directory, [bool]$Recurse = $false) {
    if (Test-Path -LiteralPath $Directory -PathType Container) {
        Get-ChildItem -LiteralPath $Directory -Filter '*.dll' -File -Recurse:$Recurse
    }
}

$evidence = Get-Item -LiteralPath $InstalledLog
# An installed OBS may still hold its log open. Read it without excluding its
# writer, then parse and hash the same captured bytes rather than two reads of
# a potentially changing file. This does not write to or lock the live log.
$evidenceStream = [IO.File]::Open($evidence.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
$evidenceBuffer = [IO.MemoryStream]::new()
try { $evidenceStream.CopyTo($evidenceBuffer); $evidenceBytes = $evidenceBuffer.ToArray() }
finally { $evidenceStream.Dispose(); $evidenceBuffer.Dispose() }
$evidenceHasher = [Security.Cryptography.SHA256]::Create()
try { $evidenceSha256 = [BitConverter]::ToString($evidenceHasher.ComputeHash($evidenceBytes)).Replace('-', '') }
finally { $evidenceHasher.Dispose() }
$lines = @([Text.Encoding]::UTF8.GetString($evidenceBytes).TrimStart([char]0xfeff) -split '\r?\n')
$starts = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match 'Loaded Modules:') { $i } })
if (!$starts.Count) { throw 'Supplied evidence has no Loaded Modules block' }
$loaded = @()
for ($i = $starts[-1] + 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*(?:\d{2}:\d{2}:\d{2}\.\d+:\s*)?([^\s]+\.dll)\s*$') { $loaded += $Matches[1] }
    else { break }
}
if (!$loaded.Count) { throw 'Loaded Modules block is empty or unrecognized' }
$loaded = @($loaded | Sort-Object -Unique)
$version = @($lines | Where-Object { $_ -match '\bOBS \d+\.\d+\.' } | Select-Object -First 1)
$versionText = $null
if ($version.Count -and $version[0] -match '\bOBS (\d+\.\d+\.\S+)') { $versionText = $Matches[1] }

$core = @(Get-ChildItem -LiteralPath (Join-Path $ForkRoot 'core') -Directory | ForEach-Object {
    $primary = Join-Path $_.FullName ($_.Name + '.dll')
    if (Test-Path -LiteralPath $primary -PathType Leaf) {
        [ordered]@{ name = $_.Name + '.dll'; path = $primary; sha256 = (Get-FileHash -LiteralPath $primary -Algorithm SHA256).Hash }
    }
})
$coreNames = @($core | ForEach-Object { $_.name })
$files = @(
    DllFiles (Join-Path $InstalledRoot 'obs-plugins/64bit')
    DllFiles (Join-Path $InstalledRoot 'plugins') $true
    DllFiles $SystemPluginRoot $true
) | Sort-Object -Property FullName -Unique
$inventory = @(foreach ($file in $files) {
    $peInfo = Read-PortableExecutable $file.FullName
    [ordered]@{
        name = $file.Name; path = $file.FullName; bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        pe = $peInfo; nameLoggedLoaded = ($loaded -contains $file.Name)
        bundledByNameInFork = ($coreNames -contains $file.Name)
    }
})
$runtimeNames = @(DllFiles (Join-Path $ForkRoot 'bin/64bit') | ForEach-Object { $_.Name })
$systemDirectory = [Environment]::SystemDirectory
$comparisons = @(foreach ($name in $loaded) {
    $candidates = @($inventory | Where-Object { $_.name -ieq $name -and $_.pe.isObsModule })
    [ordered]@{
        name = $name; bundledByNameInFork = ($coreNames -contains $name)
        installedCandidatePaths = @($candidates | ForEach-Object { $_.path })
        pathResolution = $(if ($candidates.Count -eq 1) { 'unique_inventory_candidate_not_log_proven' } elseif ($candidates.Count -gt 1) { 'ambiguous' } else { 'not_found_in_scanned_roots' })
    }
})
$external = @(foreach ($entry in $inventory | Where-Object { $_.pe.isObsModule -and !$_.bundledByNameInFork }) {
    $directory = Split-Path -Parent $entry.path
    $dependencies = @(foreach ($dependency in $entry.pe.imports) {
        $name = $dependency.name
        $resolution = if ($runtimeNames -contains $name) { 'fork_bin_name_present' }
            elseif (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf) { 'installed_plugin_sibling_requires_staging_review' }
            elseif ($name -match '^(api|ext)-ms-') { 'windows_api_set_not_resolved' }
            elseif (Test-Path -LiteralPath (Join-Path $systemDirectory $name) -PathType Leaf) { 'system32_name_present' }
            else { 'unresolved_in_scanned_locations' }
        [ordered]@{ name = $name; kind = $dependency.kind; staticResolution = $resolution }
    })
    [ordered]@{ name = $entry.name; path = $entry.path; sha256 = $entry.sha256; officiallyLoggedLoadedByName = $entry.nameLoggedLoaded; dependencies = $dependencies }
})
$report = [ordered]@{
    schemaVersion = 1; generatedUtc = [DateTime]::UtcNow.ToString('o')
    mode = 'static_no_plugin_execution'
    evidence = [ordered]@{ path = $evidence.FullName; sha256 = $evidenceSha256; modifiedUtc = $evidence.LastWriteTimeUtc.ToString('o'); readMode = 'shared_read_single_byte_snapshot'; obsVersion = $versionText; loadedModuleCount = $loaded.Count }
    roots = [ordered]@{ installed = $InstalledRoot; systemPlugins = $SystemPluginRoot; fork = [IO.Path]::GetFullPath($ForkRoot) }
    limits = @('A supplied excerpt is not a full log; its OBS version can be unavailable.', 'Loaded names prove that log session loaded a module, not feature functionality or its exact file path.', 'Static dependency name availability does not prove exports, ABI, dynamically loaded dependencies, or successful initialization.', 'Plugin code was not executed. No personal configuration was read or changed.')
    loadedComparison = $comparisons; forkBundledCore = $core
    thirdPartyCandidates = $external; installedDllInventory = $inventory
}
$json = $report | ConvertTo-Json -Depth 14
if ($ReportPath) {
    $target = [IO.Path]::GetFullPath($ReportPath)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    [IO.File]::WriteAllText($target, $json, [Text.UTF8Encoding]::new($false))
}
$json
