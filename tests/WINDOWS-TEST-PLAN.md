# Kyubu — Windows test plan (for Claude Code)

You are testing a **Kyubu release**, a mod loader for the 2013 game *Cube World Alpha v0.1.1* (the 2013-07-20 build). Kyubu is built with MinGW. Your job is to install the release on a Windows PC that has never had Kyubu, record what passes and what fails, and produce `RESULTS.md`. This is the clean-install check of a release. **Do not fix code unless a step says so**; this is a test run, and results go back to the developer.

Kyubu replaces the game's `zlib1.dll` with a proxy that forwards zlib calls to the original (renamed `zlib1_vanilla.dll`) and loads mods from `mods\`. It writes its log to `kyubu\logs\latest.log` in the game folder. Almost every check below is "start the game, wait, read the log".

## Ground rules

1. **Never modify the original game folder** (`$Orig` below). Work only on a copy in `kyubu-test\`.
2. Run all commands in **PowerShell** (5.1 or 7). Paths contain spaces: always quote them.
3. After every run, stop the game (`Stop-Cube`, below) before starting the next one.
4. Steps marked **[HUMAN]** need the user at the keyboard. Tell them exactly what to do, wait for their answer, then check the log. Group human steps as the plan does, so you ask as few times as possible.
5. Record every step in `kyubu-test\RESULTS.md` as you go (format at the end): PASS, FAIL (with the evidence) or SKIPPED (with the reason). Save each run's log with `Save-Log`.
6. If a step fails, record it and **continue** with the next step unless the plan says to stop.
7. Windows Defender or SmartScreen may flag the proxy `zlib1.dll` or the test executables (they are unsigned). If that happens, record it, ask the user whether to allow the file, and continue.

## 0. Layout and helpers

Ask the user for three things and put them in the variables below: the folder of the original game (it holds `Cube.exe`), the folder that holds the release zips, and the version being tested. The zips are the ones `scripts/package.py` makes:

```
kyubu-<version>.zip                 player release: proxy, installers, example mods, docs
kyubu-sdk-<version>.zip             headers, Lua stubs, templates
kyubu-test-kit-<version>.zip        kyubu_tests.exe, zlib_parity.exe, fixture mods
kyubu-source.zip                    optional: the source repository (only step 9 needs it)
```

Define these at the start of **every** PowerShell session you use:

```powershell
$Orig  = "<the original game folder>"      # the user's answer; never written to
$Out   = "<the folder with the zips>"      # the user's answer
$Ver   = "<version>"                       # for example 1.0.1
$Work  = Join-Path (Split-Path $Orig -Parent) "kyubu-test"
$Game  = "$Work\game"                      # the copy we install into
$Rel   = "$Work\kyubu-$Ver"                # extracted player release
$Kit   = "$Work\kyubu-test-kit-$Ver"       # extracted test kit
$Src   = "$Work\src\kyubu"                 # extracted source
$Res   = "$Work\results"
$Log   = "$Game\kyubu\logs\latest.log"

function Stop-Cube {
    Get-Process Cube, Server, WerFault -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

function Set-Mods([string[]]$Paths) {
    # Replaces the contents of the game's mods folder with the given mod folders/zips.
    New-Item -ItemType Directory -Force -Path "$Game\mods" | Out-Null
    Get-ChildItem "$Game\mods" -Force | Remove-Item -Recurse -Force
    foreach ($p in $Paths) { Copy-Item -LiteralPath $p -Destination "$Game\mods\" -Recurse -Force }
}

function Start-Cube([string[]]$GameArgs = @(), [string]$WaitFor = 'phase: game_ready', [int]$TimeoutSec = 120) {
    # Starts Cube.exe from the test copy and waits until the log contains $WaitFor (or the game exits).
    Stop-Cube
    Remove-Item "$Game\kyubu\logs\latest.log" -Force -ErrorAction SilentlyContinue
    $p = if ($GameArgs.Count) { Start-Process "$Game\Cube.exe" -WorkingDirectory $Game -ArgumentList $GameArgs -PassThru }
         else { Start-Process "$Game\Cube.exe" -WorkingDirectory $Game -PassThru }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if ((Test-Path $Log) -and (Select-String -Path $Log -SimpleMatch $WaitFor -Quiet)) { return $p }
        if ($p.HasExited) { Write-Warning "Cube.exe exited (code $($p.ExitCode))"; return $p }
    }
    Write-Warning "Timed out waiting for '$WaitFor'"
    return $p
}

function Wait-Log([string]$Text, [int]$TimeoutSec = 60) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $Log) -and (Select-String -Path $Log -SimpleMatch $Text -Quiet)) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Show-Log([string]$Pattern = '.') {
    # Prints matching log lines without the timestamp column.
    Select-String -Path $Log -Pattern $Pattern | ForEach-Object { $_.Line.Substring([Math]::Min(24, $_.Line.Length)) }
}

function Save-Log([string]$Name) {
    New-Item -ItemType Directory -Force -Path $Res | Out-Null
    if (Test-Path $Log) { Copy-Item $Log "$Res\$Name.log" -Force }
}
```

A log line looks like `2026-09-26 10:02:11.482 INFO  [hello_lua] [client] message`: timestamp, level (`DEBUG`, `INFO`, `WARN`, `ERROR`), source (`kyubu` or a mod id), side (`client`, `server` or `-`). "Expected" lines below are matched **without** the timestamp, and `...` means "anything".

## 1. Setup

```powershell
New-Item -ItemType Directory -Force -Path $Work, $Res, "$Work\src" | Out-Null
Expand-Archive "$Out\kyubu-$Ver.zip"           -DestinationPath $Work -Force
Expand-Archive "$Out\kyubu-test-kit-$Ver.zip"  -DestinationPath $Work -Force
Expand-Archive "$Out\kyubu-sdk-$Ver.zip"       -DestinationPath $Work -Force
if (Test-Path "$Out\kyubu-source.zip") { Expand-Archive "$Out\kyubu-source.zip" -DestinationPath "$Work\src" -Force }
robocopy $Orig $Game /E /NFL /NDL /NJH /NJS | Out-Null
if (-not (Test-Path "$Game\Server.exe") -and (Test-Path "$Game\Server.zip")) {
    Expand-Archive "$Game\Server.zip" -DestinationPath $Game -Force  # some copies ship Server.exe zipped
}
# windowed 1024x768 for all automated runs (step 11 switches to fullscreen)
(Get-Content "$Game\options.cfg") -replace '^fullscreen .*','fullscreen 0' -replace '^resolutionX .*','resolutionX 1024' -replace '^resolutionY .*','resolutionY 768' | Set-Content "$Game\options.cfg"
```

**1a. Tool inventory.** Record the versions (or "not installed") of: Windows (`[Environment]::OSVersion`), PowerShell (`$PSVersionTable.PSVersion`), Python 3 (`py -3 --version` or `python --version`), Git, CMake, and Visual Studio with C++ (`& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`). Nothing is required except PowerShell; missing tools only skip optional steps.

**1b. Game files are the supported build.** Compare SHA-256 hashes:

```powershell
Get-FileHash "$Game\Cube.exe","$Game\Server.exe","$Game\zlib1.dll" -Algorithm SHA256 | Format-Table Hash, Path
```

| File | Expected SHA-256 |
|---|---|
| Cube.exe | `84a7a132a84d4282338e7ea45a1940d32066d64e39cbafed8f2418d5a6dc30bf` (the 2013-07-20 build) |
| Server.exe | `1ffbcbce251e1506a63c9ce18ab0009d5553fcbb9e6202be2bc3a7d870d34fb1` |
| zlib1.dll | `e96b397b499d9eaa3f52eaf496ca8941e80c0ad1544879ccadf02bf2c6a1ecfc` |

If `Cube.exe` or `zlib1.dll` differ, **stop** and report: Kyubu only activates on this exact build.

**1c. Hashes before installing** (used in step 13):

```powershell
Get-FileHash "$Game\Cube.exe","$Game\Server.exe","$Game\zlib1.dll","$Game\data1.db","$Game\data2.db","$Game\data3.db","$Game\data4.db" -Algorithm SHA256 |
  Select-Object Path, Hash | ConvertTo-Json | Set-Content "$Res\hashes-before.json"
```

## 2. Unit and hook-engine tests (native Windows)

The hook-engine tests patch code in the test executable itself, so they are worth running on each new Windows machine.

```powershell
& "$Kit\bin\kyubu_tests.exe" 2>&1 | Tee-Object "$Res\unit-tests.txt" | Select-Object -Last 4
$env:KYUBU_STRESS_ITERATIONS = 10000
& "$Kit\bin\kyubu_tests.exe" -tc="*stress*" 2>&1 | Tee-Object "$Res\stress-10000.txt" | Select-Object -Last 4
Remove-Item Env:KYUBU_STRESS_ITERATIONS
```

Expected: `0 failed` with one skipped case (it needs extracted game models), and `Status: SUCCESS!`. The stress run: `1 passed | 0 failed`. On failure, copy the failing test names and their messages into RESULTS.md.

## 3. zlib parity

The proxy must behave exactly like the original zlib.

```powershell
$zp = "$Work\zlib-parity"; New-Item -ItemType Directory -Force -Path $zp | Out-Null
Copy-Item "$Kit\bin\zlib_parity.exe" $zp; Copy-Item "$Rel\zlib1.dll" $zp; Copy-Item "$Orig\zlib1.dll" "$zp\zlib1_vanilla.dll"
Push-Location $zp; & .\zlib_parity.exe; "exit=$LASTEXITCODE"; Pop-Location
```

Expected: `zlib parity: ok (zlib 1.2.3, 21 cases)` and `exit=0`.

## 4. Install

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "$Rel\install\kyubu_install.ps1" -Action install -GameDir $Game
powershell -NoProfile -ExecutionPolicy Bypass -File "$Rel\install\kyubu_install.ps1" -Action status -GameDir $Game
```

Expected: the install prints `Kyubu installed.`; status shows `"state": "installed"`; `$Game\zlib1_vanilla.dll`, `$Game\kyubu\symbols\cube-0x51EA955E.toml`, `$Game\kyubu\config.toml` and `$Game\mods\` exist.

If Python is installed, also run `py -3 "$Rel\install\kyubu_install.py" status "$Game"`: it must also report `installed`.

## 5. Boot without mods

```powershell
Set-Mods @(); Start-Cube | Out-Null; Save-Log "05-boot"; Show-Log 'starting|verified|boot complete|game_ready|ERROR|WARN'
```

Expected (and **no `ERROR` lines**):

```
INFO  [kyubu] [-] Kyubu <version> (API 1.<n>) starting in Cube.exe (...\kyubu-test\game\Cube.exe)
INFO  [kyubu] [-] Host build cube-0x51EA955E verified (image base 0x........)
INFO  [kyubu] [-] boot complete: 0 mod(s) active
INFO  [kyubu] [-] phase: game_ready
```

Note the image base: under Windows ASLR it is probably not `0x00400000`. That is fine, and exactly what we want to see tested.

**Graceful exit.** While the game runs, try `(Get-Process Cube).CloseMainWindow()`, wait 10 s, and check for `Kyubu shut down cleanly` in the log. If the game does not close that way, record SKIPPED and use `Stop-Cube`.

## 6. Example mods together

```powershell
Set-Mods (Get-ChildItem "$Rel\example-mods" -Directory).FullName
Start-Cube | Out-Null; Start-Sleep 5; Save-Log "06-examples"; Show-Log 'Load order|ERROR|WARN|slot|spell|hello|hook_sample|assets:'
```

Expected lines (order may differ; **no `ERROR` or `WARN`**):

```
INFO  [kyubu] [-] Load order: gold_sword, hello_native, hello_lua, hook_sample, spells_lib, frost_spells
INFO  [hook_sample] [client] hook_sample:ModelLoader::loadAll resolved to 0x........
INFO  [hook_sample] [client] model table sized at 0x........ (esp=0x........)
INFO  [kyubu] [-] Model slots: 2571 total: vanilla's 2569, 1 from mods, 1 Kyubu placeholder(s)
INFO  [gold_sword] [-] assets: overrides 2 vanilla key(s), adds 0 new key(s)
INFO  [hello_native] [-] assets: overrides 0 vanilla key(s), adds 1 new key(s)
INFO  [spells_lib] [client] spell #0 frost_spells:frost_nova: Frost Nova, 30 mana
INFO  [spells_lib] [client] spell #1 frost_spells:ice_lance: Ice Lance, 12 mana
INFO  [frost_spells] [client] frost_spells:ice_lance is spell number 1
INFO  [hello_native] [client] game ready; hello_native:crate is model slot 2569
INFO  [hello_lua] [client] hello from Lua on the client side
INFO  [hello_lua] [server] hello from Lua on the server side
```

(The exact `Load order` can differ if you add or remove mods; dependencies must come before their dependents.) Keep the game running for step 10 if the user is available, otherwise `Stop-Cube`.

## 7. Fixture mods (automated)

Each run: set the mods, start, wait, save the log, check the expected lines. `$F = "$Kit\fixture-mods"`.

| # | Mods | Start with | Expected in the log |
|---|---|---|---|
| 7.1 | `fx_parity` | (default) | `API parity: <n>/<m> native functions reachable from Lua on the client side` and the same for the server side, with the gap `tests/README.md` names for `fx_parity`; `side rules: 5/5 client-only calls refused on the server side`; `text: current=Hello en=Hello de=Hallo dictionaries=ok`. The 5 `WARN ... is client-only and was called from the server side` lines are expected. Wait for `API parity` (up to 60 s after game_ready). |
| 7.2 | `fx_parity`, with `language 1` in `$Game\options.cfg` | (default) | `text: current=Hallo ...`. Set `language 0` back afterwards. |
| 7.3 | `fx_fault_frame` | (default) | `ERROR [fx_fault_frame] [client] FAULTED and disabled: access violation in frame.begin at ...`, and the game **keeps running** (process alive 20 s later). |
| 7.4 | `fx_script_error` | (default) | `handler ... disabled after more than 20 errors within 1 second` and `still running after the failing handler was disabled`. |
| 7.5 | `fx_restricted` | (default) | `require('ffi') ok=false err=...trust level 'restricted'...`; `hook ok=false err=...trust level 'restricted'...`; `io available=false os available=false`. |
| 7.6 | `fx_missing_dep` | (default) | `mod fx_missing_dep: not loaded: fx_missing_dep needs nonexistent >=1.0, which is not installed` |
| 7.7 | `fx_dup_a`, `fx_dup_b` | (default) | `duplicate mod id 'fx_dup'` for both. |
| 7.8 | `fx_patch_a`, `fx_patch_b` | (default) | `patch a: <number> nil`; `patch b: nil conflicts with another mod`; and an `ERROR [fx_patch_b] ... patch_bytes(...) failed: patch ... conflicts with fx_patch_a` line |
| 7.9 | `fx_globals_a`, `fx_globals_b` | (default) | `[fx_globals_a] [client] my global 'shared' is a` and `[fx_globals_b] [client] my global 'shared' is b` |
| 7.10 | `fx_asset_watch` | (default) | `asset.request: <about 2670> events (... data1.db=<about 2559> ...), first: data4.db/dict_en.xml in phase post_register` |
| 7.11 | `fx_features` + `$Rel\example-mods\hello_native` | `--kyubu-dev` | `provider generated proc-tree7.cub`; `slot 899 (cubeworld:iron-sword1) loads fx-remap-sword.cub (remap)`; `mid hook at 0x........`; `Database::get calls seen by the Lua hook: <about 2670>`; `hello_native:counter interface: userdata: ... v1`; `proc-tree7.cub: 588 bytes from provider:fx_features` |
| 7.12 | `fx_asset_watch` | `--kyubu-safe` | `SAFE MODE (--kyubu-safe on the command line): all mods are disabled for this session`, and no `fx_asset_watch` lines |
| 7.13 | `fx_asset_watch`, and create an empty file `$Game\kyubu\SAFE` | (default) | `SAFE MODE (kyubu/SAFE exists): all mods are disabled for this session`. **Delete `$Game\kyubu\SAFE` afterwards.** |

Example for one row:

```powershell
$F = "$Kit\fixture-mods"
Set-Mods @("$F\fx_parity"); Start-Cube | Out-Null; Wait-Log 'API parity' 60 | Out-Null; Start-Sleep 5
Save-Log "07-1-parity"; Show-Log 'API parity|side rules|text:|ERROR'
```

**7.14 Hot reload** (dev mode):

```powershell
Set-Mods @("$F\fx_hot"); Start-Cube @('--kyubu-dev') | Out-Null
Wait-Log 'hot version 1' 60
$s = "$Game\mods\fx_hot\scripts\client.lua"; (Get-Content $s) -replace 'version 1','version 2' | Set-Content $s
Wait-Log 'hot version 2' 60; Save-Log "07-14-hot"; Show-Log 'hot|unloading'
```

Expected: `unloading version 1`, `hot reload: reloaded fx_hot (1 side(s)); registry entries are kept`, `hot version 2`.

**7.15 Crash recovery.** `fx_crash` crashes the game on purpose 60 frames after start (a raw hook writes to address 0).

```powershell
Set-Content "$Game\kyubu\config.toml" "[crash]`r`non_mod_crash = `"disable`"`r`n"
Set-Mods @("$F\fx_crash"); Start-Cube -WaitFor 'Crash report written' -TimeoutSec 120 | Out-Null
Start-Sleep 5; Stop-Cube                          # also closes a Windows error-report dialog if one appeared
Get-ChildItem "$Game\kyubu\logs\crash-*.txt" | Select-Object -Last 1 | Get-Content -TotalCount 12
Start-Cube | Out-Null; Save-Log "07-15-after-crash"; Show-Log 'crashed|disabled|fx_crash'
Remove-Item "$Game\kyubu\config.toml"; Copy-Item "$Game\kyubu\config.default.toml" "$Game\kyubu\config.toml"
```

Expected: a `crash-*.txt` (and `crash-*.dmp`) in `$Game\kyubu\logs\` with `Implicated mod: fx_crash - ...`; on the next start `The previous session crashed. Report: ...` and `Mod 'fx_crash' disabled after the previous crash`. Record what Windows showed when the game crashed (error dialog or nothing).

**7.16 Passthrough on a modified executable.** Kyubu must stay inactive on a build it does not recognise.

```powershell
$pt = "$Work\game-modified"; robocopy $Game $pt /E /NFL /NDL /NJH /NJS | Out-Null
$b = [IO.File]::ReadAllBytes("$pt\Cube.exe"); "byte at 0x28108 = 0x{0:X2}" -f $b[0x28108]   # must be 0xCC (padding, never executed)
$b[0x28108] = 0x90; [IO.File]::WriteAllBytes("$pt\Cube.exe", $b)
Get-ChildItem "$pt\mods" | Remove-Item -Recurse -Force; Copy-Item "$F\fx_asset_watch" "$pt\mods\" -Recurse
Remove-Item "$pt\kyubu\logs\latest.log" -ErrorAction SilentlyContinue
$p = Start-Process "$pt\Cube.exe" -WorkingDirectory $pt -PassThru; Start-Sleep 30
Select-String "$pt\kyubu\logs\latest.log" -Pattern 'passthrough|verified|mod\(s\)'
Stop-Cube
```

Expected: a `WARN` line `Unsupported game build (...). Running in passthrough mode: no hooks, no mods.`, no `boot complete` line, and the game still reaches the start menu (ask the user if needed). If the byte at 0x28108 is not `0xCC`, SKIP this step.

## 8. SDK checks (optional; needs CMake plus a compiler)

- **MinGW** (if an i686 MinGW-w64 toolchain is installed): build `$Work\kyubu-sdk-$Ver\templates\native-mod` (`cmake -S <template> -B "$Work\tpl-mingw" -G "MinGW Makefiles"` then `cmake --build`). Expected: `my_mod\bin\my_mod.dll`.
- **Visual Studio** (if VS with the C++ x86 tools is installed): open an x86 developer environment (`& "<VS path>\VC\Auxiliary\Build\vcvarsall.bat" x86` inside `cmd /c`, or use "x86 Native Tools Command Prompt"), then build the template with `cmake -S <template> -B "$Work\tpl-msvc" -A Win32` and `cmake --build "$Work\tpl-msvc" --config Release`. Expected: `my_mod\bin\Release\my_mod.dll` or `my_mod\bin\my_mod.dll`. Then copy the staged `my_mod` folder into `$Game\mods`, start the game, and check for `my_mod loaded on the client side` and `my_mod:cube is model slot 2569`.

## 9. Full source build with Visual Studio (optional, informational)

Kyubu itself has **never been built with MSVC**. If Visual Studio with C++ x86 tools and CMake are present, try:

```
cmd /c "call ""<VS path>\VC\Auxiliary\Build\vcvarsall.bat"" x86 && cmake -S ""%SRC%"" -B ""%SRC%\build-msvc"" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build ""%SRC%\build-msvc"""
```

(`%SRC%` = `$Src`; use `-G "Visual Studio 17 2022" -A Win32` if Ninja is missing.) Record the result. If it fails, put the **first 40 error lines** in RESULTS.md. You may fix *trivial* compile errors (a missing include, a type cast) to get further; keep every change and save it as a patch (`git -C $Src diff > $Res\msvc.patch`). Do not restructure code. If it builds, run `build-msvc\tests\kyubu_tests.exe` and record the summary line, and note the size of `build-msvc\loader\zlib1.dll`. Do **not** install the MSVC-built proxy into the game.

## 10. [HUMAN] Interactive checks, session A

Ask the user to sit at the PC, then start:

```powershell
Set-Mods @("$Rel\example-mods\hello_lua", "$Rel\example-mods\hello_native", "$Rel\example-mods\gold_sword", "$F\fx_input", "$F\fx_slow")
Start-Cube @('--kyubu-dev') | Out-Null
```

Ask the user to do and report each of these (you check the log where noted):

1. On the start menu, a notice at the top left says `Kyubu: 5 mod(s) loaded`, and windows called **Hello Lua** and **fx_input** are visible.
2. In **Hello Lua**, drag the `speed` slider (the number changes), then click **ping server** three times. You check: `pong #1`, `pong #2`, `pong #3` in the log.
3. Press the key left of `1` (`` ` ``) to open the **Kyubu console**. Type `mods` and Enter (all 5 mods listed as `loaded`), then `hello_native:hello` (prints `hello from native code! ...`), then `events` (prints event counts). Press `` ` `` again to close it.
4. Press **Ctrl+F3**: a **Kyubu diagnostics** panel appears; the `fx_slow` row shows about 10-12 ms/frame, the others well under 1 ms. Press Ctrl+F3 again to close.
5. Click the **fx_input** window while it lies over a menu button (drag it over **START GAME** first if needed): the menu must **not** react to that click. You check: `click consumed by the overlay` may appear; the key point is that the game did not act.
6. Start a game (existing character and world, or create them). In the world, press **Enter**: with `fx_input` loaded the chat box must **not** open. You check: `cancelled Enter #...`.
7. Look at the character's iron sword if one is equipped or in the inventory: it should be **gold**. (Only if it is easy to find; otherwise skip.)

Then `Stop-Cube`, and do the control run for item 6:

```powershell
Set-Mods @("$Rel\example-mods\hello_lua"); Start-Cube | Out-Null
```

Ask the user to enter the world and press **Enter**: the chat box **should** open now. If it does not open in the control run either, record item 6 as "inconclusive". `Stop-Cube`.

## 11. [HUMAN] Interactive checks, session B

1. **Shift safe mode.** Tell the user: "I'll start the game; press and **hold Shift** now and keep holding it until the Cube World window appears." Then run `Set-Mods @("$Rel\example-mods\hello_lua"); Start-Cube | Out-Null` and check the log for `SAFE MODE (Shift held at startup)`. `Stop-Cube`.
2. **Device reset (alt-tab).** Set `fullscreen 1` in `$Game\options.cfg`, start with `hello_lua`, ask the user to alt-tab away from the game and back twice and confirm the **Hello Lua** window is still drawn afterwards. You check: `render.device_lost` / `render.device_reset` counts via the console (`events`) if the user can type it, otherwise just their answer. Set `fullscreen 0` back.

## 12. [HUMAN] Multiplayer against a local server

The client must join through the game's menu, so this needs the user. Windows Firewall may ask about `Server.exe` the first time: tell the user to allow it (private networks).

First make a second installed copy for the client, because a client and a server started from the same folder would write the same log:

```powershell
Stop-Cube
Set-Mods @("$F\fx_net")
$cl = "$Work\game-client"; robocopy $Game $cl /E /NFL /NDL /NJH /NJS | Out-Null
Remove-Item "$Game\kyubu\logs\latest.log", "$cl\kyubu\logs\latest.log" -ErrorAction SilentlyContinue
$srv = Start-Process "$Game\Server.exe" -WorkingDirectory $Game -PassThru
Start-Sleep 15
Select-String "$Game\kyubu\logs\latest.log" -Pattern 'Server.exe|game_ready'      # the server booted with Kyubu
$cli = Start-Process "$cl\Cube.exe" -WorkingDirectory $cl -PassThru
```

Ask the user: **Start Game** → pick a character → **Connect to server** (on the world selection screen) → address `127.0.0.1` → **Connect**, then walk around for 20 seconds. Then check:

- client log (`$cl\kyubu\logs\latest.log`): `connected to 127.0.0.1:12345 (multiplayer: client side only)`, `net.connect 127.0.0.1:12345`, `first net.recv_raw: ... bytes`, and later `net: <n> recv (...)`;
- server log (`$Game\kyubu\logs\latest.log`): `net.connect 127.0.0.1:<port>` and `first net.recv_raw`.

Then the **vanilla-server safety check**: stop both, put `$F\fx_requires_server` (instead of `fx_net`) into `$cl\mods`, start the server again **without** Kyubu mods (`Set-Mods @()`), start the client, and ask the user to connect again. Expected: a message box saying the server is not a Kyubu server and naming `fx_requires_server`, and the connection does not happen. Stop both processes (`Stop-Cube` also stops `Server.exe`).

## 13. Uninstall and verify

```powershell
Stop-Cube
powershell -NoProfile -ExecutionPolicy Bypass -File "$Rel\install\kyubu_install.ps1" -Action uninstall -GameDir $Game
$before = Get-Content "$Res\hashes-before.json" | ConvertFrom-Json
$after  = Get-FileHash "$Game\Cube.exe","$Game\Server.exe","$Game\zlib1.dll","$Game\data1.db","$Game\data2.db","$Game\data3.db","$Game\data4.db" -Algorithm SHA256
Compare-Object $before.Hash $after.Hash      # no output = identical
powershell -NoProfile -ExecutionPolicy Bypass -File "$Rel\install\kyubu_install.ps1" -Action uninstall -GameDir $Game -Purge
```

Expected: `Cube World is back to vanilla.`; `Compare-Object` prints nothing; after `-Purge`, `$Game\kyubu` is gone and `$Game\mods` remains. Finally ask the user to start the uninstalled copy once (`$Game\Cube.exe`) to confirm vanilla still starts.

## 14. Optional: Application Verifier

Only if `appverif.exe` exists (Windows SDK): enable the **Basics** checks for `Cube.exe`, reinstall Kyubu into `$Game` (step 4), run step 5 once, record any verifier break or log entry about the loader lock, then **disable** the checks for `Cube.exe` again (`appverif -disable * -for Cube.exe`). Skip otherwise.

## Report: `kyubu-test\RESULTS.md`

```markdown
# Kyubu Windows test results
Date: <date>  Machine: <Windows version, CPU>  Tools: <from 1a>

| Step | Result | Evidence / notes |
|---|---|---|
| 1b game hashes | PASS | all three match |
| 2 unit tests | PASS | <n> passed, 1 skipped |
| ... | ... | ... |

## Failures in detail
<for each FAIL: what you ran, what you expected, what happened, the relevant log lines>

## Anything unexpected
<dialogs, antivirus, crashes, slow starts, visual glitches the user mentioned>
```

When you are done, tell the user where `RESULTS.md` and the `results\` folder are, and that both should be shared with the Kyubu developers. Leave `kyubu-test\` in place; do not delete anything.
