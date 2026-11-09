<#
.SYNOPSIS
  Kyubu installer for Cube World Alpha (v0.1.1). Same behaviour as kyubu_install.py.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File kyubu_install.ps1 -Action install -GameDir "C:\Games\Cube World"
  powershell -ExecutionPolicy Bypass -File kyubu_install.ps1 -Action uninstall -GameDir "C:\Games\Cube World" [-Purge]
  powershell -ExecutionPolicy Bypass -File kyubu_install.ps1 -Action status -GameDir "C:\Games\Cube World"

.DESCRIPTION
  install   renames zlib1.dll to zlib1_vanilla.dll, copies the Kyubu proxy in as zlib1.dll, adds kyubu\
            (symbol maps, default config) and mods\, then downloads ModManager from the mod catalog into
            mods\mod_manager\: the newest version that fits this loader, the catalog's signature and the zip's
            SHA-256 checked. Without a connection Kyubu is still installed and ModManager is skipped with a
            warning. -NoModManager skips it. Running it again upgrades in place.
            Refuses when Cube.exe/Server.exe or zlib1.dll are not the known v0.1.1 files (-Force skips the
            executable check; Kyubu then stays inactive on that build).
  uninstall deletes the proxy and renames zlib1_vanilla.dll back. -Purge also deletes kyubu\ (mods\ is kept).
  No other game file is written, renamed or deleted.
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('install', 'uninstall', 'status')][string]$Action,
    [string]$GameDir,
    [switch]$Force,
    [switch]$Purge,
    [switch]$NoModManager
)
$ErrorActionPreference = 'Stop'

# ModManager comes from the mod catalog, not from the release (as in kyubu_install.py). The catalog's index.json is
# signed with this key; LoaderApi is KYUBU_API_VERSION of the proxy this release ships. KYUBU_INSTALL_CATALOG (a URL or a
# folder) and KYUBU_INSTALL_CATALOG_KEY (hex) replace the two for tests.
$CatalogUrl = 'https://kyubumods.github.io/catalog/'
$CatalogKey = 'e58fd9207ab9e11b754c87850bdb5f239138a485c7b9805444a7fb821572047e'
$LoaderApi = @(1, 4)

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

# ---- Ed25519 signature check (RFC 8032), only what verifying the catalog's index.json needs ----------------------------
Add-Type -AssemblyName System.Numerics
$EdP = [bigint]::Pow(2, 255) - 19
$EdQ = [bigint]::Pow(2, 252) + [bigint]::Parse('27742317777372353535851937790883648493')
function EdMod([bigint]$a) { $r = [bigint]::Remainder($a, $EdP); if ($r.Sign -lt 0) { $r += $EdP }; return $r }
function EdInv([bigint]$a) { return [bigint]::ModPow((EdMod $a), $EdP - 2, $EdP) }
$EdD = EdMod ([bigint](-121665) * (EdInv 121666))
$EdI = [bigint]::ModPow(2, ($EdP - 1) / 4, $EdP)

function EdX([bigint]$y, [int]$sign) {
    $xx = EdMod (($y * $y - 1) * (EdInv ($EdD * $y * $y + 1)))
    $x = [bigint]::ModPow($xx, ($EdP + 3) / 8, $EdP)
    if ((EdMod ($x * $x - $xx)) -ne 0) { $x = EdMod ($x * $EdI) }
    if ((EdMod ($x * $x - $xx)) -ne 0) { return $null }
    if ([int]($x % 2) -ne $sign) { $x = $EdP - $x }
    return $x
}

function EdAdd($a, $b) {
    $pa = EdMod (($a[1] - $a[0]) * ($b[1] - $b[0])); $pb = EdMod (($a[1] + $a[0]) * ($b[1] + $b[0]))
    $pc = EdMod ($a[3] * 2 * $EdD * $b[3]); $pd = EdMod ($a[2] * 2 * $b[2])
    $e = $pb - $pa; $f = $pd - $pc; $g = $pd + $pc; $h = $pb + $pa
    return , @((EdMod ($e * $f)), (EdMod ($g * $h)), (EdMod ($f * $g)), (EdMod ($e * $h)))
}

function EdMul([bigint]$s, $pt) {
    $out = @([bigint]0, [bigint]1, [bigint]1, [bigint]0)
    while ($s -gt 0) {
        if (-not $s.IsEven) { $out = EdAdd $out $pt }
        $pt = EdAdd $pt $pt
        $s = $s / 2
    }
    return , $out
}

function EdNumber([byte[]]$b) { return [bigint]::new([byte[]]($b + [byte]0)) }   # little-endian, never negative

function EdDecode([byte[]]$b) {
    $y = EdNumber $b
    $sign = [int](($b[31] -shr 7) -band 1)
    $y = $y - [bigint]$sign * [bigint]::Pow(2, 255)
    if ($y -ge $EdP) { return $null }
    $x = EdX $y $sign
    if ($null -eq $x -or ($x -eq 0 -and $sign -eq 1)) { return $null }
    return , @($x, $y, [bigint]1, (EdMod ($x * $y)))
}

function Test-Ed25519([byte[]]$Public, [byte[]]$Message, [byte[]]$Signature) {
    if ($Public.Length -ne 32 -or $Signature.Length -ne 64) { return $false }
    $a = EdDecode $Public
    $r = EdDecode $Signature[0..31]
    $s = EdNumber $Signature[32..63]
    if ($null -eq $a -or $null -eq $r -or $s -ge $EdQ) { return $false }
    $sha = [System.Security.Cryptography.SHA512]::Create()
    $h = [bigint]::Remainder((EdNumber $sha.ComputeHash([byte[]]($Signature[0..31] + $Public + $Message))), $EdQ)
    $by = EdMod (4 * (EdInv 5))
    $bx = EdX $by 0
    $base = @($bx, $by, [bigint]1, (EdMod ($bx * $by)))
    $left = EdMul $s $base
    $right = EdAdd $r (EdMul $h $a)
    return ((EdMod ($left[0] * $right[2] - $right[0] * $left[2])) -eq 0) -and ((EdMod ($left[1] * $right[2] - $right[1] * $left[2])) -eq 0)
}

# ---- ModManager from the catalog -----------------------------------------------------------------------------------------
function Get-ReleaseVersion([string]$Text) {
    if ($Text -match '^(\d+)\.(\d+)\.(\d+)$') { return , @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3]) }
    return $null
}

function Test-FitsLoader([string]$Req) {
    if ($Req -eq '*' -or $Req -eq '') { return $true }
    if ($Req -notmatch '^\^(\d+)(?:\.(\d+))?(?:\.(\d+))?$') { return $false }
    $minor = $(if ($Matches[2]) { [int]$Matches[2] } else { 0 })
    return ($LoaderApi[0] -eq [int]$Matches[1]) -and ($LoaderApi[1] -ge $minor)
}

function Read-CatalogFile([string]$Base, [string]$Rel) {
    if ($Base -match '^https?://') {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $wc = New-Object System.Net.WebClient
        $wc.Headers.Add('User-Agent', 'kyubu-installer')
        return , $wc.DownloadData($Base.TrimEnd('/') + '/' + $Rel)
    }
    return , [System.IO.File]::ReadAllBytes((Join-Path $Base ($Rel -replace '/', '\')))
}

function Get-InstalledVersion([string]$Folder) {
    $p = Join-Path $Folder 'mod.toml'
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    $m = [regex]::Match([System.IO.File]::ReadAllText($p), '(?m)^version\s*=\s*"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

function Install-ModManager([string]$Game) {
    $catalog = $(if ($env:KYUBU_INSTALL_CATALOG) { $env:KYUBU_INSTALL_CATALOG } else { $CatalogUrl })
    $keyHex = $(if ($env:KYUBU_INSTALL_CATALOG_KEY) { $env:KYUBU_INSTALL_CATALOG_KEY } else { $CatalogKey })
    $key = [byte[]]@(for ($i = 0; $i -lt $keyHex.Length; $i += 2) { [Convert]::ToByte($keyHex.Substring($i, 2), 16) })
    try {
        $index = Read-CatalogFile $catalog 'index.json'
        $sig = Read-CatalogFile $catalog 'index.json.sig'
    } catch {
        throw "cannot read the mod catalog at $catalog ($($_.Exception.Message))"
    }
    if (-not (Test-Ed25519 $key $index $sig)) { throw 'the mod catalog is not signed with the Kyubu catalog key' }
    $doc = [System.Text.Encoding]::UTF8.GetString($index) | ConvertFrom-Json
    $best = $null
    foreach ($m in @($doc.mods | Where-Object { $_.id -eq 'mod_manager' })) {
        foreach ($v in @($m.versions)) {
            $ver = Get-ReleaseVersion $v.version
            if ($null -eq $ver -or -not (Test-FitsLoader $v.kyubu)) { continue }
            if ($null -eq $best) { $best = $v; continue }
            $old = Get-ReleaseVersion $best.version
            for ($i = 0; $i -lt 3; $i++) {
                if ($ver[$i] -gt $old[$i]) { $best = $v; break }
                if ($ver[$i] -lt $old[$i]) { break }
            }
        }
    }
    if ($null -eq $best) { throw "the mod catalog has no ModManager for loader API $($LoaderApi[0]).$($LoaderApi[1])" }
    $dst = Join-Path $Game 'mods\mod_manager'
    $have = Get-InstalledVersion $dst
    if ($have -eq $best.version -and (Test-Path -LiteralPath (Join-Path $dst 'bin'))) {
        Write-Host "ModManager $have is up to date"
        return
    }
    try { $data = Read-CatalogFile $catalog $best.url } catch { throw "cannot download ModManager $($best.version) ($($_.Exception.Message))" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $got = ([BitConverter]::ToString($sha.ComputeHash($data)) -replace '-', '').ToLowerInvariant()
    if ($got -ne $best.sha256) { throw "the ModManager $($best.version) download does not match the catalog (sha256)" }
    Add-Type -AssemblyName System.IO.Compression
    $zip = New-Object System.IO.Compression.ZipArchive((New-Object System.IO.MemoryStream(, $data)), [System.IO.Compression.ZipArchiveMode]::Read)
    $tmp = "$dst.new"
    try {
        foreach ($e in $zip.Entries) {
            $parts = $e.FullName -split '[\\/]'
            if ($e.FullName.StartsWith('/') -or $parts -contains '..' -or $e.FullName.Contains(':')) {
                throw "the ModManager zip holds an unsafe path: $($e.FullName)"
            }
        }
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
        foreach ($e in $zip.Entries) {
            if ($e.FullName.EndsWith('/')) { continue }
            $out = Join-Path $tmp ($e.FullName -replace '/', '\')
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
            $in = $e.Open(); $fs = [System.IO.File]::Create($out)
            try { $in.CopyTo($fs) } finally { $fs.Dispose(); $in.Dispose() }
        }
    } finally { $zip.Dispose() }
    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
    Move-Item -LiteralPath $tmp -Destination $dst
    $was = $(if ($have) { " (was $have)" } else { '' })
    Write-Host "installed ModManager $($best.version) into mods\mod_manager\$was"
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
    $missing = $null
    if (-not $NoModManager) {
        try { Install-ModManager $Game } catch { $missing = $_.Exception.Message }
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
    if ($missing) {
        Write-Warning "ModManager was not installed: $missing. Kyubu works without it; run the installer again when online to add it."
    }
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
