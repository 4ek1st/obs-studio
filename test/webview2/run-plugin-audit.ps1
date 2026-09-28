[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StageRoot,
    [Parameter(Mandatory = $true)][ValidateSet('native-qt', 'webview2')][string]$Frontend,
    [switch]$RenderTest,
    [switch]$WaitForCompletion
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
foreach ($file in $entry[0].pluginFiles) {
    if ((Get-FileHash -LiteralPath (Join-Path $stage $file.relativePath) -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Staged plugin file changed after inventory: $($file.relativePath)"
    }
}
$arguments = '--portable --webview2-self-test --webview2-plugin-audit --multi --disable-updater --disable-missing-files-check ' + $(if ($Frontend -eq 'native-qt') { '--webview2-qt-baseline' } else { '--webview2' })
if ($RenderTest) { $arguments += ' --webview2-plugin-render-test' }
$artifacts = Join-Path $stage 'audit-artifacts'
if (Test-Path -LiteralPath (Join-Path $artifacts 'plugin-audit.json')) { throw 'Probe already has evidence; stage a fresh pair.' }
$environment = @{ OBS_WEBVIEW2_TEST_ARTIFACTS = $artifacts; OBS_PLUGINS_PATH = $null;
    OBS_LEGACY_PLUGINS_PATH = $null; OBS_LEGACY_PLUGINS_DATA_PATH = $null }
# Keep verbose third-party stdout/stderr in this probe's evidence directory.
# PowerShell 7.4+ provides child-scoped environment overrides.
$process = Start-Process -FilePath $binary -WorkingDirectory (Split-Path -Parent $binary) -ArgumentList $arguments `
    -Environment $environment -RedirectStandardOutput (Join-Path $artifacts 'stdout.log') `
    -RedirectStandardError (Join-Path $artifacts 'stderr.log') -WindowStyle Normal -PassThru
# Return ownership information immediately; caller observes output/timeout and
# must never stop unrelated installed OBS instances.
[ordered]@{ pid = $process.Id; frontend = $Frontend; root = $stage; report = Join-Path $stage 'audit-artifacts/plugin-audit.json' } | ConvertTo-Json
if ($WaitForCompletion) {
    if (!$process.WaitForExit(45000)) { throw "Probe exceeded 45 seconds; owned PID $($process.Id) retained for inspection." }
    $inventoryWritten = Test-Path -LiteralPath (Join-Path $artifacts 'plugin-audit.json')
    $renderPassed = if ($RenderTest) {
        $renderPath = Join-Path $artifacts 'render/plugin-render.json'
        (Test-Path -LiteralPath $renderPath) -and (Get-Content -LiteralPath $renderPath -Raw | ConvertFrom-Json).passed
    } else { $true }
    $result = [ordered]@{ pid = $process.Id; exitCode = $process.ExitCode; inventoryWritten = $inventoryWritten;
        renderRequested = [bool]$RenderTest; renderPassed = $renderPassed;
        passed = ($process.ExitCode -eq 0 -and $inventoryWritten -and $renderPassed) }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $artifacts 'process.json')
    $result | ConvertTo-Json
    if (!$result.passed) { throw 'Plugin probe failed; inspect its preserved reports.' }
}
