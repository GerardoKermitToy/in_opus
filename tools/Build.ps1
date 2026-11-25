[CmdletBinding()]
param(
    [ValidateSet('vs2022-win32', 'vs2026-win32')]
    [string]$Preset = 'vs2022-win32',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$SampleFile = '',
    [switch]$DependenciesOnly,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($SampleFile) {
    $SampleFile = (Resolve-Path -LiteralPath $SampleFile).Path.Replace('\', '/')
}
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

Push-Location $root
try {
    $only = if ($DependenciesOnly) { 'ON' } else { 'OFF' }
    $tests = if ($RunTests -and -not $DependenciesOnly) { 'ON' } else { 'OFF' }
    Run 'cmake' @('--preset', $Preset, "-DWINAMP_OPUS_DEPENDENCIES_ONLY=$only", "-DWINAMP_OPUS_BUILD_TESTS=$tests", "-DWINAMP_OPUS_TEST_FILE=$SampleFile")
    $build = Join-Path $root "build/$Preset"
    $arguments = @('--build', $build, '--config', $Configuration, '--parallel', '4')
    if ($DependenciesOnly) { $arguments += @('--target', 'ogg', 'opus', 'opusfile') }
    Run 'cmake' $arguments
    if (-not $DependenciesOnly) {
        if ($RunTests) {
            Run 'ctest' @('--test-dir', $build, '-C', $Configuration, '--output-on-failure')
        }
        Run 'cmake' @('--install', $build, '--config', $Configuration, '--component', 'Runtime')
    }
}
finally {
    Pop-Location
}
