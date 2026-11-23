[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$WinampDirectory = (Join-Path ${env:ProgramFiles(x86)} 'Winamp'),
    [string]$PluginPath = (Join-Path $PSScriptRoot '../dist/Plugins/in_opus.dll')
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $PluginPath).Path
$winamp = Join-Path $WinampDirectory 'winamp.exe'
if (-not (Test-Path -LiteralPath $winamp -PathType Leaf)) {
    throw "winamp.exe was not found in $WinampDirectory"
}
if (Get-Process -Name 'winamp' -ErrorAction SilentlyContinue) {
    throw 'Close Winamp before installing the plugin. No files were changed.'
}

# Refuse an accidentally supplied x64 or non-PE binary.
$bytes = [System.IO.File]::ReadAllBytes($source)
if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
    throw 'The plugin is not a Windows PE binary.'
}
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
if ($pe -lt 0 -or $pe -gt ($bytes.Length - 24) -or
    [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550 -or
    [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x014C) {
    throw 'The plugin must be an x86 (32-bit) PE binary.'
}

$plugins = Join-Path $WinampDirectory 'Plugins'
$target = Join-Path $plugins 'in_opus.dll'
if ([System.IO.Path]::GetFullPath($source) -eq [System.IO.Path]::GetFullPath($target)) {
    throw 'Source and destination are the same file.'
}
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ($PSCmdlet.ShouldProcess($target, 'Install the 32-bit Opus input plugin; back up any existing file')) {
    if (-not (Test-Path -LiteralPath $plugins)) {
        New-Item -ItemType Directory -Path $plugins | Out-Null
    }
    $backup = $null
    if (Test-Path -LiteralPath $target) {
        $oldHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        $backup = "$target.backup-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([Guid]::NewGuid().ToString('N'))"
        Copy-Item -LiteralPath $target -Destination $backup -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $oldHash) {
            throw "Backup verification failed. The installed plugin was not changed: $backup"
        }
        Write-Host "Backup: $backup"
    }
    try {
        Copy-Item -LiteralPath $source -Destination $target -Force -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $sourceHash) {
            throw 'Installed file checksum mismatch.'
        }
    }
    catch {
        if ($backup) { Copy-Item -LiteralPath $backup -Destination $target -Force -ErrorAction Stop }
        throw
    }
    Write-Host "Installed: $target"
    Write-Host "SHA256: $sourceHash"
    Write-Host 'Start Winamp and open a local .opus file.'
}
