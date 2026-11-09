<#
.SYNOPSIS
  Kyubu installer for Cube World Alpha (v0.1.1). Same behaviour as kyubu_install.py.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File kyubu_install.ps1 -Action install -GameDir "C:\Games\Cube World"
  powershell -ExecutionPolicy Bypass -File kyubu_install.ps1 -Action uninstall -GameDir "C:\Games\Cube World" [-Purge]
  powershell -ExecutionPolicy Bypass -File kyubu_install.ps1 -Action status -GameDir "C:\Games\Cube World"

.DESCRIPTION
  install   renames zlib1.dll to zlib1_vanilla.dll, copies the Kyubu proxy in as zlib1.dll and adds kyubu\
            (symbol maps, default config) and mods\ with the mods the release carries (ModManager).
            Running it again upgrades in place.
            Refuses when Cube.exe/Server.exe or zlib1.dll are not the known v0.1.1 files (-Force skips the
            executable check; Kyubu then stays inactive on that build).
  uninstall deletes the proxy and renames zlib1_vanilla.dll back. -Purge also deletes kyubu\ (mods\ is kept).
  No other game file is written, renamed or deleted.
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('install', 'uninstall', 'status')][string]$Action,
    [string]$GameDir,
    [switch]$Force,
    [switch]$Purge
)
$ErrorActionPreference = 'Stop'

$KnownHosts = @{
    'Cube.exe'   = '84a7a132a84d4282338e7ea45a1940d32066d64e39cbafed8f2418d5a6dc30bf'   # the 2013-07-20 build
    'Server.exe' = '1ffbcbce251e1506a63c9ce18ab0009d5553fcbb9e6202be2bc3a7d870d34fb1'
}
$VanillaZlib = 'e96b397b499d9eaa3f52eaf496ca8941e80c0ad1544879ccadf02bf2c6a1ecfc'

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

function Test-KyubuProxy([string]$Path) {
    # Latin-1 maps every byte to one char, so an ordinal IndexOf is a fast byte search.
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($Path))
    return $text.IndexOf('Kyubu crash report', [System.StringComparison]::Ordinal) -ge 0
}

function Get-PayloadDir {
    foreach ($d in @($PSScriptRoot, (Split-Path -Parent $PSScriptRoot))) {
        if ((Test-Path -LiteralPath (Join-Path $d 'zlib1.dll') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $d 'kyubu') -PathType Container)) { return $d }
    }
    throw 'cannot find the Kyubu files (zlib1.dll and kyubu\) next to this script'
}

function Get-InstallState([string]$Game) {
    $hosts = [ordered]@{}
    foreach ($name in $KnownHosts.Keys) {
        $p = Join-Path $Game $name
        if (Test-Path -LiteralPath $p -PathType Leaf) { $hosts[$name] = Get-Sha256 $p }
    }
    $zlib = Join-Path $Game 'zlib1.dll'
    $van = Join-Path $Game 'zlib1_vanilla.dll'
    $s = [ordered]@{
        hosts          = $hosts
        zlib1          = $(if (Test-Path -LiteralPath $zlib) { Get-Sha256 $zlib } else { $null })
        zlib1_is_proxy = $((Test-Path -LiteralPath $zlib) -and (Test-KyubuProxy $zlib))
        zlib1_vanilla  = $(if (Test-Path -LiteralPath $van) { Get-Sha256 $van } else { $null })
        state          = 'unknown'
    }
    if ($s.zlib1 -eq $VanillaZlib -and $null -eq $s.zlib1_vanilla) { $s.state = 'vanilla' }
    elseif ($s.zlib1_is_proxy -and $s.zlib1_vanilla -eq $VanillaZlib) { $s.state = 'installed' }
    return $s
}

function Copy-Tree([string]$Src, [string]$Dst, [scriptblock]$Overwrite) {
    $count = 0
    Get-ChildItem -LiteralPath $Src -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($Src.Length).TrimStart('\', '/')
        $target = Join-Path $Dst $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        if ((Test-Path -LiteralPath $target) -and -not (& $Overwrite $rel)) { return }
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        $count++
    }
    return $count
}

function Install-Kyubu([string]$Game) {
    $src = Get-PayloadDir
    $s = Get-InstallState $Game
    if ($s.hosts.Count -eq 0) { throw "$Game contains neither Cube.exe nor Server.exe" }
    foreach ($name in $s.hosts.Keys) {
        if ($s.hosts[$name] -ne $KnownHosts[$name]) {
            $msg = "$name is not the Cube World Alpha v0.1.1 file Kyubu supports (sha256 $($s.hosts[$name].Substring(0, 16))...)"
            if (-not $Force) { throw "$msg; use -Force to install anyway (Kyubu will stay inactive on that build)" }
            Write-Warning $msg
        }
    }
    $zlib = Join-Path $Game 'zlib1.dll'
    $van = Join-Path $Game 'zlib1_vanilla.dll'
    if ($s.state -eq 'vanilla') {
        $backup = Join-Path $Game 'kyubu\backup'
        New-Item -ItemType Directory -Force -Path $backup | Out-Null
        Copy-Item -LiteralPath $zlib -Destination (Join-Path $backup 'zlib1.dll') -Force
        Move-Item -LiteralPath $zlib -Destination $van
        Write-Host 'renamed zlib1.dll -> zlib1_vanilla.dll (a copy is kept in kyubu\backup\)'
    } elseif ($s.state -eq 'installed') {
        Write-Host 'Kyubu is already installed; upgrading'
    } else {
        throw 'zlib1.dll is neither the original Cube World file nor a Kyubu proxy (another tool may have replaced it); restore the original zlib1.dll first'
    }
    Copy-Item -LiteralPath (Join-Path $src 'zlib1.dll') -Destination $zlib -Force
    Write-Host 'installed the Kyubu proxy as zlib1.dll'
    $n = Copy-Tree (Join-Path $src 'kyubu') (Join-Path $Game 'kyubu') { param($rel) ($rel -split '[\\/]')[0] -in @('symbols', 'lua') }
    $cfg = Join-Path $Game 'kyubu\config.toml'
    $def = Join-Path $Game 'kyubu\config.default.toml'
    if (-not (Test-Path -LiteralPath $cfg) -and (Test-Path -LiteralPath $def)) { Copy-Item -LiteralPath $def -Destination $cfg }
    New-Item -ItemType Directory -Force -Path (Join-Path $Game 'mods') | Out-Null
    Write-Host "copied $n file(s) into kyubu\, created mods\"
    # The mods the release carries (ModManager): replaced on an upgrade, apart from a player's config.toml.
    $bundled = Join-Path $src 'mods'
    if (Test-Path -LiteralPath $bundled) {
        $m = Copy-Tree $bundled (Join-Path $Game 'mods') { param($rel) ((($rel -split '[\\/]') | Select-Object -Skip 1) -join '/') -ne 'config.toml' }
        $names = (Get-ChildItem -LiteralPath $bundled -Directory | ForEach-Object { $_.Name }) -join ', '
        Write-Host "installed $names into mods\ ($m file(s))"
    }

    $after = Get-InstallState $Game
    if ($after.state -ne 'installed') { throw "verification failed after install: $($after | ConvertTo-Json -Compress)" }
    [ordered]@{
        installed_at         = (Get-Date).ToString('s')
        proxy_sha256         = $after.zlib1
        zlib1_vanilla_sha256 = $after.zlib1_vanilla
        hosts                = $after.hosts
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Game 'kyubu\install.json') -Encoding UTF8
    Write-Host 'Kyubu installed. Start Cube.exe as usual; hold Shift while it starts for safe mode.'
}

function Uninstall-Kyubu([string]$Game) {
    $s = Get-InstallState $Game
    $zlib = Join-Path $Game 'zlib1.dll'
    $van = Join-Path $Game 'zlib1_vanilla.dll'
    if ($s.state -eq 'vanilla') {
        Write-Host 'Kyubu is not installed (zlib1.dll is the original file)'
    } elseif ($s.state -eq 'installed' -or ($Force -and $null -ne $s.zlib1_vanilla)) {
        if ($s.state -ne 'installed') { Write-Warning 'forcing uninstall of a folder in an unknown state' }
        Remove-Item -LiteralPath $zlib
        Move-Item -LiteralPath $van -Destination $zlib
        Write-Host 'removed the proxy and renamed zlib1_vanilla.dll -> zlib1.dll'
    } else {
        $backup = Join-Path $Game 'kyubu\backup\zlib1.dll'
        $hint = $(if (Test-Path -LiteralPath $backup) { " A copy of the original is in $backup." } else { '' })
        throw "cannot uninstall: zlib1.dll / zlib1_vanilla.dll are not in the state Kyubu left them.$hint"
    }
    if ($Purge -and (Test-Path -LiteralPath (Join-Path $Game 'kyubu'))) {
        Remove-Item -LiteralPath (Join-Path $Game 'kyubu') -Recurse -Force
        Write-Host 'deleted kyubu\ (mods\ was kept)'
    }
    $after = Get-InstallState $Game
    if ($after.state -ne 'vanilla') { throw "verification failed after uninstall: $($after | ConvertTo-Json -Compress)" }
    Write-Host 'Cube World is back to vanilla.'
}

try {
    if (-not $GameDir) {
        $guess = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        if (Test-Path -LiteralPath (Join-Path $guess 'Cube.exe')) { $GameDir = $guess }
        else { $GameDir = Read-Host 'Cube World folder (the one containing Cube.exe)' }
    }
    $GameDir = (Resolve-Path -LiteralPath $GameDir).Path
    switch ($Action) {
        'install' { Install-Kyubu $GameDir }
        'uninstall' { Uninstall-Kyubu $GameDir }
        'status' { Get-InstallState $GameDir | ConvertTo-Json -Depth 3 }
    }
    exit 0
} catch {
    Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
