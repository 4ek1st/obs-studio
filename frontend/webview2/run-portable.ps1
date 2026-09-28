param(
    [string]$BuildDirectory = 'build_webview2',
    [switch]$OnlyBundledPlugins
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$binaryDirectory = Join-Path $repository "$BuildDirectory\rundir\RelWithDebInfo\bin\64bit"
$executable = Join-Path $binaryDirectory 'obs64.exe'
if (!(Test-Path -LiteralPath $executable)) {
    throw "Build OBS first. Expected executable: $executable"
}
$arguments = @('--portable', '--webview2', '--disable-updater', '--disable-missing-files-check')
if ($OnlyBundledPlugins) { $arguments += '--only-bundled-plugins' }
Start-Process -FilePath $executable -WorkingDirectory $binaryDirectory -ArgumentList $arguments -WindowStyle Hidden