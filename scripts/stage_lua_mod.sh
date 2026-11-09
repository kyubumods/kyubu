#!/usr/bin/env bash
# Copies a Lua gameplay mod into the install the user plays (kyubu-play/game/mods/<id>/), the way a player would install it.
# usage: stage_lua_mod.sh <mod folder name under mods-gameplay> [--dry-run]
# Env:   KYUBU_PLAY = the kyubu-play folder (default: two folders above this repository, next to kyubu-mapping)
#
# It refuses while Cube.exe is running (the game holds the mod's files), checks that scripts/client.lua is the current
# bundle when the mod has a src/ folder, and copies only what the game reads: mod.toml, config.toml, scripts/, assets/, symbols/
# Sources (src/), icon grids (icons-src/), briefs and tests stay behind. No update/ folder: the files go straight in.
set -eu
MOD=${1:?usage: stage_lua_mod.sh <mod folder name> [--dry-run]}
DRY=${2:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$HERE/mods-gameplay/$MOD"
PLAY=${KYUBU_PLAY:-$(cd "$HERE/../.." && pwd)/kyubu-play}
DEST="$PLAY/game/mods/$MOD"

[ -f "$SRC/mod.toml" ] || { echo "stage_lua_mod: $SRC/mod.toml not found" >&2; exit 2; }
[ -d "$PLAY/game/mods" ] || { echo "stage_lua_mod: $PLAY/game/mods not found (set KYUBU_PLAY)" >&2; exit 2; }
ID=$(sed -n 's/^id *= *"\(.*\)"/\1/p' "$SRC/mod.toml" | head -1)
[ "$ID" = "$MOD" ] || { echo "stage_lua_mod: the folder is $MOD but mod.toml says id = $ID" >&2; exit 2; }

if tasklist //FI "IMAGENAME eq Cube.exe" 2>/dev/null | grep -qi "Cube.exe"; then
  echo "stage_lua_mod: Cube.exe is running: close the game, then run this again" >&2
  exit 3
fi
if [ -d "$SRC/src" ]; then
  python "$HERE/tools/bundle_lua.py" --mod "$SRC" --label "mods-gameplay/$MOD/src" --check
fi

LIST=""
for part in mod.toml config.toml scripts assets symbols; do [ -e "$SRC/$part" ] && LIST="$LIST $part"; done
echo "stage_lua_mod: $SRC -> $DEST"
echo "stage_lua_mod: copying:$LIST"
if [ "$DRY" = "--dry-run" ]; then (cd "$SRC" && find $LIST -type f | sort); exit 0; fi

mkdir -p "$DEST"
for part in scripts assets symbols; do rm -rf "${DEST:?}/$part"; done   # only this mod's own folders under mods/<id>/
for part in $LIST; do cp -r "$SRC/$part" "$DEST/"; done
(cd "$DEST" && find . -type f | sort)
echo "stage_lua_mod: done. Start the game from kyubu-play."
