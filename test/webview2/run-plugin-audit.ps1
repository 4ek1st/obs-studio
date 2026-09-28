[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StageRoot,
    [Parameter(Mandatory = $true)][ValidateSet('native-qt', 'webview2')][string]$Frontend
)
$ErrorActionPreference = 'Stop'
# Explicit runner, separate from static inspection/staging. The caller authorizes
# executing the staged third-party modules by invoking this script.
$stage = (Resolve-Path -LiteralPath $StageRoot).Path
$manifestPath = Join-Path (Split-Path -Parent $stage) 'stage-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$entry = @($manifest.stages | Where-Object { $_.root -ieq $stage -and $_.mode -eq $Frontend })
if ($entry.Count -ne 1) { throw 'Stage and frontend must match the explicit staging manifest.' }
$binary = Join-Path $stage 'bin/64bit/obs64.exe'
if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash -ne $entry[0].executableSha256) { throw 'Staged executable changed after inventory.' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $binary
$start.WorkingDirectory = Split-Path -Parent $binary
$start.UseShellExecute = $false
$start.Arguments = '--portable --webview2-self-test --webview2-plugin-audit --multi --disable-updater --disable-missing-files-check ' + $(if ($Frontend -eq 'native-qt') { '--webview2-qt-baseline' } else { '--webview2' })
foreach ($key in @('OBS_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_DATA_PATH')) { $start.EnvironmentVariables.Remove($key) }
$start.EnvironmentVariables['OBS_WEBVIEW2_TEST_ARTIFACTS'] = Join-Path $stage 'audit-artifacts'
$process = [Diagnostics.Process]::Start($start)
# Return ownership information immediately; caller observes output/timeout and
# must never stop unrelated installed OBS instances.
[ordered]@{ pid = $process.Id; frontend = $Frontend; root = $stage; report = Join-Path $stage 'audit-artifacts/plugin-audit.json' } | ConvertTo-Json
