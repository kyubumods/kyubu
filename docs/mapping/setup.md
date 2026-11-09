# Setting up the mapping workspace

The mapping workspace is how Kyubu learns about the game: a private Ghidra project built from **your own copy** of Cube World Alpha, the text database in `mapping/`, and the `kmap` tool that moves knowledge between them. This page sets it up on Windows or Linux. The rules for what goes into `mapping/` are in [mapping/conventions.md](../../mapping/conventions.md).

## What you need

| | Version | Notes |
|---|---|---|
| Python | 3.11 or newer | runs `kmap`; `pip install pefile` for `kmap survey`, `kmap export` and the full `kmap check` |
| SQLite source | the 3.7.15.2 amalgamation | `sqlite-amalgamation-3071502.zip` from sqlite.org (step 4 of Install), so the survey can name the game's built-in SQLite |
| JDK | 21 or newer | Ghidra 12 needs a full JDK (with `javac`), for example Eclipse Temurin 21. Any JDK 21 works, including the one bundled with Android Studio (`<Android Studio>\jbr`). |
| Ghidra | exactly the pinned version | `kmap ghidra install` downloads it (about 570 MB, 1.2 GB unpacked) |
| The game | Cube World Alpha v0.1.1 | `Cube.exe` and `Server.exe` (or `Server.zip`) with the SHA-256 hashes in `mapping/workspace.toml` |
| Disk | about 2.5 GB | Ghidra, the PyGhidra environment and the project for both builds |

The Ghidra GUI is optional; everything runs headless.

## Install

From a Kyubu checkout:

```
python tools/kmap ghidra install --jdk "<JDK 21 folder>"
python tools/kmap init --game "<Cube World folder>"
python tools/kmap push
python tools/kmap check
```

On Linux use `python3`. Instead of `python tools/kmap`, you can `pip install -e tools/kmap` once and then type `kmap`.

1. `kmap ghidra install` downloads the pinned Ghidra release, checks its SHA-256, unpacks it into the workspace and creates a Python environment with Ghidra's bundled PyGhidra (no internet access needed for that step). `--jdk` remembers which JDK to use; without it, kmap looks at `KMAP_JDK`, `JAVA_HOME` and `java` on `PATH`. A download whose hash is wrong is deleted, and nothing is installed.
2. `kmap init --game` checks the SHA-256 of `Cube.exe` and `Server.exe` against the pinned builds (`Server.exe` is read straight out of `Server.zip` when the game ships it zipped), copies them into the workspace, imports both into a Ghidra project and runs auto-analysis with the pinned options. It never writes to the game folder. Running it again does nothing; `--rebuild` starts over.
3. `kmap push` applies everything in `mapping/` to the project: names, namespaces, prototypes, types, enums, and each entry's status and evidence as a plate comment.
4. Before the first `kmap survey`, download `https://www.sqlite.org/2013/sqlite-amalgamation-3071502.zip` and unpack it into `$KMAP_HOME/sqlite/`, so that `$KMAP_HOME/sqlite/sqlite-amalgamation-3071502/sqlite3.c` exists. The survey reads names and strings from it (facts only; nothing is copied). Without it the survey says "SQLite amalgamation 3.7.15.2 not found", leaves the 1,205 SQLite functions of each build unnamed, and `kmap report` no longer reproduces the committed `mapping/survey-summary.md`.
5. `kmap check` validates `mapping/` and every generated file. It regenerates the symbol-map signatures from the copies in the workspace.
   - `kmap check --offline` skips the parts that need the game files and keeps the committed signatures. It needs no workspace, Ghidra or `pefile`. CI runs it in a job of its own (`mapping` in `.github/workflows/ci.yml`), so a pull request that forgets `kmap export` fails there.
   - `kmap check --game "<Cube World folder>"` is the full check without a workspace: it reads the verified builds straight from the game folder and writes nothing. The private contract test level runs it.

## Where the workspace lives

Everything derived from the game's code stays in the workspace, `$KMAP_HOME`, never in the repository:

| System | Default `KMAP_HOME` |
|---|---|
| Windows | `%LOCALAPPDATA%\kyubu-mapping` |
| Linux | `~/.kyubu-mapping` |

```
kyubu-mapping/
  ghidra_<version>_PUBLIC/    the pinned Ghidra
  venv/                       Python environment with PyGhidra
  local.toml                  the JDK remembered by `kmap ghidra install --jdk`
  inputs/<build>/             verified copies of Cube.exe and Server.exe
  ghidra/kyubu.gpr, kyubu.rep the Ghidra project (both builds) and pushed.json
  sqlite/                     the SQLite 3.7.15.2 amalgamation (step 4 of Install)
  survey/<build>/             raw survey output (JSON)
  decomp/<build>/             `kmap decompile` output
  ghidra-settings/, ghidra-cache/, downloads/, tmp/
```

Set `KMAP_HOME` to use another folder. kmap refuses a folder inside a git checkout, and the repository's `.gitignore` and `tools/check_public_files.py` reject Ghidra projects (`*.gpr`, `*.rep/`, `*.gzf`), type archives (`*.gdt`) and anything under a `decomp/` folder.

**Windows, apps installed from the Store or as MSIX packages.** Such apps (the Claude desktop app is one) see a private copy of `%LOCALAPPDATA%`: a workspace created by a command started from them lives in `%LOCALAPPDATA%\Packages\<app>\LocalCache\Local\kyubu-mapping` and is invisible to programs started elsewhere, such as the Ghidra GUI. If you mix the two, set `KMAP_HOME` to a folder outside `AppData` (for example `C:\kyubu-mapping`) in both places.

`KMAP_GHIDRA` points kmap at an existing Ghidra installation; it must be exactly the pinned version, or every command stops and says which version it found.

## Working in the Ghidra GUI

Start Ghidra from the workspace (`ghidra_<version>_PUBLIC\ghidraRun.bat` or `ghidraRun`) and open `ghidra/kyubu.gpr`. Rename functions, retype them, create structures under the `/kyubu` data type category. Then close the project (Ghidra locks it) and run:

```
python tools/kmap pull           shows your changes as a diff against mapping/
python tools/kmap pull --write   writes them
```

New names arrive as `guess` entries: give them a subsystem, evidence and a status by hand (the rules are in [mapping/conventions.md](../../mapping/conventions.md)), then `kmap check` and commit. pull never raises a status, and an entry that changed both in `mapping/` and in Ghidra since the last push is reported as a conflict and left alone.

## Other commands

| Command | Does |
|---|---|
| `kmap ghidra status` | the pin, the installed Ghidra, the JDK and the workspace in use |
| `kmap survey` | the automated passes over both builds (RTTI, libraries, strings and imports, correlation, coverage) |
| `kmap survey query --string data1.db` | look things up in the survey index (`--import d3d9.dll!Direct3DCreate9`, `--function 0x489130`) |
| `kmap report` | writes `mapping/survey-summary.md` |
| `kmap import-cuwo <checkout>` | proposes candidate types and enums from cuwo at the pinned commit |
| `kmap export` | regenerates the symbol maps, headers, Lua types, reference docs and Ghidra mappings |
| `kmap lock` | lists the exported names that `mapping/exported-names.lock` doesn't hold yet; `--release` adds them, on release (see "Name stability" in `mapping/conventions.md`) |
| `kmap decompile <function, class or subsystem>` | pseudo-C for reading, under `$KMAP_HOME/decomp/` only; never commit or share it |
| `kmap probe new <id>` | starts a probe record in `mapping/probes/` (the format is in [docs/probes.md](../probes.md)); `kmap probe list` lists them |
| `kmap probe replay <id> --game <folder> --kyubu <release>` | repeats a record's script in a copy of the game and writes a report; changes nothing in `mapping/` ([docs/probes.md](../probes.md#replay)) |

## Timings

Measured runs for both builds, unattended:

| Machine | `kmap init` | `kmap survey` | `kmap export` |
|---|---|---|---|
| Windows 10 laptop (Intel Core i5-1235U, 12 threads, 16 GB) | 16 min 43 s (`Cube.exe` 599 s, `Server.exe` 397 s) | 46 s | 57 s |
| the same laptop, a fresh workspace (2026-09-28) | 17 min 20 s (`Cube.exe` 615 s, `Server.exe` 402 s) | 80 s | 148 s |
| the same laptop, Ubuntu 24.04 in WSL 2 with OpenJDK 21 | 19 min | about 40 s | not run |

A re-run of `kmap init` with nothing changed takes under a second. `kmap survey` re-reads every function
from the Ghidra project each time; its output depends only on the project and on `mapping/` (the coverage
pass counts what is already mapped), so two runs with the same `mapping/` are byte-identical. `kmap export`
spends most of its time growing the byte signatures until each one is unique.
