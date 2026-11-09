#!/usr/bin/env bash
# DEPRECATED 2026-11-06: in-game checks are native now (`kmap probe replay`, see RECIPES.md). This Wine script stays as a parity reference.
# Builds a throw-away game sandbox with the Kyubu proxy installed and runs a host under Wine.
# usage: wine_sandbox.sh <game_dir> <build_dir> <sandbox_dir> <seconds> [Cube.exe|Server.exe] [args...]
# It returns once the host has exited, or after <seconds> at the latest (kmap probe replay closes the game once its
# script has finished). It also waits for a process the host started itself (game_restart), up to the same limit.
# Env: KEEP=1 keeps an existing sandbox (mods/config), SHOT=<png> captures the screen 3 s before the time is up,
# WINEARCH=win64 for a WoW64 Wine's prefix (the default is win32), NOKILL=1 leaves the prefix's wineserver alone (a
# second host beside a running one, e.g. a client joining a server; the first host's closing wineserver -k ends both),
# RENDER_DISTANCE=<n> the options' renderDistance (default 40; a
# percentage of the game's chunk mesh budget: lower draws fewer chunks, so frames are faster under software rendering).
set -u
GAME=$1; BUILD=$2; SB=$3; SECS=$4; EXE=${5:-Cube.exe}; shift 5 || shift $#
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine32} WINEARCH=${WINEARCH:-win32} WINEDEBUG=${WINEDEBUG:--all} DISPLAY=${DISPLAY:-:99}
pgrep -x Xvfb >/dev/null || (Xvfb :99 -screen 0 1030x800x24 >/dev/null 2>&1 &) ; sleep 0.5
pgrep -x pulseaudio >/dev/null || (pulseaudio -D --exit-idle-time=-1 >/dev/null 2>&1; pactl load-module module-null-sink sink_name=null >/dev/null 2>&1)
if [ "${KEEP:-0}" != 1 ]; then
  rm -rf "$SB"; mkdir -p "$SB"
  for f in "$GAME"/*; do n=$(basename "$f"); case "$n" in data*.db) cp "$f" "$SB/";; Save) ;; *) ln -s "$f" "$SB/$n";; esac; done
  mkdir -p "$SB/Save"   # its own saves: a link would write them into the game folder
  rm -f "$SB/zlib1.dll"; cp "$GAME/zlib1.dll" "$SB/zlib1_vanilla.dll"
  printf 'fullscreen 0\r\nresolutionX 1280\r\nresolutionY 720\r\nantiAliasing 0\r\nrenderDistance '"${RENDER_DISTANCE:-40}"'\r\nsoundVolume 0\r\nmusicVolume 0\r\ncameraSpeed 80\r\ncameraSmoothness 0\r\nlanguage 0\r\n' > "$SB/options.cfg.tmp"; rm -f "$SB/options.cfg"; mv "$SB/options.cfg.tmp" "$SB/options.cfg"
fi
cp "$BUILD/loader/zlib1.dll" "$SB/zlib1.dll"
mkdir -p "$SB/kyubu/symbols" "$SB/mods"
cp "$(dirname "$0")/../symbols/"*.toml "$SB/kyubu/symbols/"
mkdir -p "$SB/kyubu/lua"
cp "$(dirname "$0")/../loader/lua/"*.lua "$SB/kyubu/lua/"   # require("cubeworld") for full-trust mods (a 1.0.1 build has none)
[ -n "${MODS:-}" ] && for m in $MODS; do rm -rf "$SB/mods/$(basename "$m")"; cp -r "$m" "$SB/mods/"; done
[ -n "${CONFIG:-}" ] && cp "$CONFIG" "$SB/kyubu/config.toml"
[ "${NOKILL:-0}" = 1 ] || { wineserver -k 2>/dev/null; sleep 0.5; }
cd "$SB"
T0=$SECONDS
timeout "$SECS" wine "$EXE" "$@" > "$SB/wine-stdout.txt" 2>&1 &
PID=$!
SHOTPID=
if [ -n "${SHOT:-}" ]; then   # if the game still runs then
  (sleep $((SECS - 3)); kill -0 $PID 2>/dev/null && import -window root "$SHOT" 2>/dev/null) & SHOTPID=$!
fi
wait $PID; RC=$?              # the time is up, or the game has ended before
# A host that restarts itself (game_restart) leaves a new process in the prefix: wait for it, for what is left of the time
# (kmap probe replay stops the wineserver once the script has finished).
if [ "${NOKILL:-0}" != 1 ]; then LEFT=$((SECS - (SECONDS - T0))); [ "$LEFT" -gt 0 ] && timeout "$LEFT" wineserver -w 2>/dev/null; fi
[ -n "$SHOTPID" ] && { pkill -P $SHOTPID 2>/dev/null; kill $SHOTPID 2>/dev/null; }
[ "${NOKILL:-0}" = 1 ] || wineserver -k 2>/dev/null
echo "exit=$RC"
