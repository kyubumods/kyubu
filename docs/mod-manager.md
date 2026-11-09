# ModManager

ModManager (`mod_manager`) adds a **MODS** row to the title menu. It opens a page in the style of the game's own
screens, where you enable, disable, configure, install and remove mods, and apply modpacks. It is a native client mod
with `manage_mods = true`, built on the loader's management API ([mods.md](mods.md#managing-mods)).

## Players

### Open the page

Click **MODS** under EXIT on the title screen. The page replaces the logo and the menu rows. **BACK** or Escape
returns to the title screen, and asks first when changes are waiting. The row exists only on the title screen, not in
Options or character creation.

### My mods

The left list shows every mod in `mods\`: a switch, the name, the version and a mark. Use the arrows to filter (All,
Enabled, Disabled, Problems, Updates) and the search field to match name, id or author. Select a mod to see its
authors, state, description, dependencies, dependents and options.

| Mark | Meaning |
|---|---|
| **Verified** | This exact package is a version in the signed catalog: reviewed and approved by the catalog's owner. It is not a sandbox; a native mod can do anything a program can. |
| **Unverified** | Either the id is not in the catalog ("Not from the catalog. Installed by hand.") or the package differs from every catalog version ("Changed since it was verified, or from another source."). It still loads. |

A red **!** marks a mod that failed or faulted; the details give the reason.

**ACTIONS** (next to the mod's name) holds the actions for the selected item: Update, Enable or Disable, Reset
options, Delete. Actions that do not apply are grey with a tip. ModManager cannot disable or delete itself.

**OPEN MODS FOLDER** opens `mods\`. A mod you drop in while the game runs shows as **New** ("Loads after a restart");
one you remove shows as **Missing**.

### Changes, Apply and restart

Nothing is saved when you click. Every change lands in the bar at the bottom, on any tab. **DISCARD** drops all of
them. The last button reads:

- **APPLY**: only option changes. They take effect at once; the mod gets `config.changed`. An option marked
  "Takes effect at the next start" is saved at once but read by the mod at its next start.
- **APPLY & RESTART**: an enable, disable, delete, download or new mod is in the list. ModManager writes
  `kyubu\pending.toml` and restarts the game; Kyubu applies the changes before it loads any mod.

An invalid option value is outlined red and blocks Apply. If the restart fails, ModManager says so; the changes are
saved, so start the game again by hand.

**Delete** asks first. It keeps the mod's settings and data unless you tick "Also delete its settings and saved data".
The package goes to `kyubu\backup\` and stays there until the next start that has no pending changes, so one undo is
possible. A package replaced by an update goes to `kyubu\backup\<id>-<old version>\` the same way.

### Install

**Install / Mods** lists the catalog: name, author, latest version, size, and *Installed*, *Update* or *Get*. The
details show every version, its changes and dependencies. **Download** (in ACTIONS) fetches the zip, checks its
SHA-256, size and contents, and adds it to the changes. Everything in the catalog is verified, so there is no warning
step. When the catalog is offline the page works from the last good copy (its signature is checked again) and
downloads are grey.

If the signature fails, the page says "The catalog could not be verified" and offers nothing.

### Modpacks

**Install / Modpacks** lists packs from the catalog and, below them, your local profiles. A pack pins an **exact
version** of every mod and dependency, so it plays the same for everyone. The details show the plan, one line per mod
(*installed*, *will download*, *will change 1.1.1 to 1.1.0*, *will be disabled*) and how many options change.

**Apply modpack** (ACTIONS) is grey while other changes are collected, while a download runs, or while the pack is
already the setup in use. It then:

1. downloads what is missing (one failure stops everything and queues nothing; the details say which mod and why);
2. saves your setup as the profile **Before <pack>**;
3. queues the installs, enables the pack's mods, disables every other mod except ModManager, and sets the pack's
   options (options the pack does not name go back to their defaults, unless the pack says otherwise);
4. restarts.

A **profile** holds the enabled mods, their versions and every option value. **Restore** brings one back: a mod at the
same version is kept or enabled, a mod at another version is reinstalled from `kyubu\backup\` if the package is still
there (otherwise the installed one stays), a mod not in the profile is disabled, and the options are set back.
**Delete profile** asks first.

### Undo last apply

Kyubu writes `kyubu\backup\last-apply.toml` before it applies a queue: the previous `[mods]` section of `config.toml`,
each options file the queue touches, and which packages moved. ModManager can queue the reverse. The record is
deleted at the next start without pending changes.

### Where things live

| Path | Holds |
|---|---|
| `kyubu\pending.toml` | The queued changes (plain TOML; delete it to drop them). |
| `kyubu\staging\` | Downloads waiting for Apply. Emptied at boot of anything the pending file does not name. |
| `kyubu\backup\` | Replaced and deleted packages, and `last-apply.toml`. |
| `kyubu\data\mod_manager\` | The last good `index.json`, its signature, the ETag, `profiles\`, `last-options.toml`. |
| `kyubu\config\mod_manager.toml` | ModManager's options. |

ModManager's option `catalog_url` is the address of the catalog folder (the one holding `index.json` and
`index.json.sig`). It defaults to `https://kyubumods.github.io/catalog/` (the catalog named Penchev, repository `kyubumods/catalog`). A `file:///` address reads from disk.

If ModManager cannot find a game symbol or a widget template, it logs why and does not draw the MODS row; the title
screen stays as the game made it. In safe mode the pending file is not applied.

## Mod authors

### Options forms

The page builds a form from your `config.toml`, in declaration order. Optional fields shape it:

- `label`: the name shown (at most 40 characters). Without it the key is shown in words (`ring_alpha` is "Ring alpha").
- `group`: the heading the option is listed under.
- `restart = true`: you read the option only at start. The value is saved at once and the form says "Takes effect at
  the next start".

Handle `config.changed`, or mark the option `restart = true`. Controls: `bool` is an ON/OFF picker, bounded `int` and
`float` are sliders (Ctrl+click types a value), a `string` with `choices` is a picker, other `string`s are text
fields. See [config.md](config.md). A mod that uses `label`, `group` or `restart` needs `kyubu = "^1.2"`.

### Getting into the catalog

The catalog holds only mods the owner has reviewed. You cannot publish to it yourself: send your package to the
owner. Anyone can still put a mod in `mods\` by hand; it loads and shows as Unverified.

## Catalog owner: `tools/kcat`

The catalog is static files on any HTTPS host (planned: GitHub Pages from a repository of its own):

```
<catalog_url>/
  index.json            every mod, every version, every pack
  index.json.sig        Ed25519 signature of the exact bytes of index.json
  mods/<id>/<id>-<version>.zip
```

The private key stays in `%USERPROFILE%\.kyubu\catalog.key`, never in a repository or on the server. The public key is
compiled into ModManager (`src/keys.h`). `kcat` works on a local clone of the catalog (`--catalog <folder>`, default
the current folder):

| Command | Does |
|---|---|
| `kcat keygen` | Makes the key pair and prints the public key for `keys.h`. |
| `kcat build <mod folder> [--out DIR]` | Packs one mod from its source folder into the folder `kcat add` takes (default `dist/<id>-<version>/`): builds a native mod with ninja and strips the DLL, checks a Lua bundle is current, and copies only `mod.toml`, `config.toml`, `scripts`, `assets`, `symbols`. Needs `tools/kcat/kcat.toml` (`build_dir`, `toolchain_bin`; not in git) for native mods. |
| `kcat add <folder or zip> [--changes TEXT]` | The review step: checks `mod.toml` and `config.toml` and that no game files are inside, shows the manifest, trust, every DLL and script and the options, and asks before it adds the version. |
| `kcat remove <id> [<version>] [--publish]` | Withdraws a version or a mod. Refuses a version a pack pins, and names the pack. |
| `kcat pack <pack.toml> [--publish]` | Adds or updates a pack. Every mod needs one exact version; kcat adds the dependencies and checks the versions exist and fit. |
| `kcat release <mod folder> [--bump patch\|minor\|major] [--changes TEXT] [--no-push]` | The one publish command for your own mods. Needs the mod's git repository clean and (to push) `gh` signed in. It bumps `mod.toml`, builds, refuses a DLL that holds the test public key, adds the version with no prompt, signs, verifies, then commits and pushes the catalog, tags `v<version>` in the mod's repository and creates its GitHub release. Any failure before the commit puts the catalog and `mod.toml` back. A published version is never changed: an existing version stops it. `mod_manager` also runs the command list `mod_manager_tests` from `kcat.toml` first. |
| `kcat tag <mod folder>` | The mod-repository half of `release`, to retry when `gh` or the push failed after the catalog was published. |
| `kcat name <text>` | Sets the catalog's display name (shown in ModManager). Run `kcat sign` after. |
| `kcat verify [--keys-h FILE]` | `kcat check` against the real public keys read from `mod_manager/src/keys.h` (the test-key block is ignored): what players' ModManager trusts. Fails while `keys.h` has no real key. |
| `kcat hook [--keys-h FILE]` | Installs a git `pre-commit` hook in the catalog clone that runs `kcat verify`, so a forgotten `kcat sign` cannot be committed. Bypass with `git commit --no-verify`. |
| `kcat sign` | Writes `index.json.sig`. Run it after every change to the index. |
| `kcat check [--pubkey HEX]` | Re-hashes every zip, checks the signature, URLs and every pack. Run it before every upload. |

A pack file:

```toml
id = "unleashed_starter"
name = "Unleashed Starter"
version = "1.0.0"
kyubu = "^1.2"
mods = { skills_unleashed = "0.4.0", emtweaks = "1.3.0" }
reset_unlisted = true
```

Upload is `git push` of the catalog folder. Tests: `tests/tools/test_kcat.py`.

**Publishing the source too.** When a mod is developed in one tree and published from its own public repository, a
`[source]` table in `tools/kcat/kcat.toml` (kept out of git) tells `kcat release` where that repository is. The mod
is then built from the development tree, and after the catalog is signed and verified `kcat` runs `sync` (it copies
the mod's source into the repository), refuses any file `tools/check_public_files.py` rejects, commits the change as
`<id> <version>`, pushes the catalog, and tags and releases in the source repository. The source repository must be
clean before the release; any failure before the catalog push puts it back, with the catalog and `mod.toml`.

```toml
[source]
repo = "D:/work/mods/{id}"                         # a git clone with an `origin` on GitHub
sync = ["python", "D:/work/sync_mod.py", "{id}"]   # {id}, {version} and {repo} are filled in
[source.repos]                                     # a mod whose source is in another repository
mod_manager = "D:/work/kyubu"
[source.tags]                                      # its tag there, when v<version> would clash
mod_manager = "mod_manager-v{version}"
```

The package hash (what the Verified mark compares) is the SHA-256 of a zip, or of a sorted listing of a folder (path
with `/`, size and SHA-256 of each file). `kcat` and the loader use the same rule.

## Developers

A release build of ModManager is configured with `-DKYUBU_RELEASE=ON -DKYUBU_MM_TEST_KEY=OFF`; with the test key on, the configure step stops. `kcat release` and `scripts/package.py` also refuse any DLL that contains the test public key.

ModManager's source is `mods-gameplay/mod_manager/src`; the planner (`plan.cpp`) is pure and has unit tests in
`mods-gameplay/tests/mod_manager/`.
