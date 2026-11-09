#!/usr/bin/env bash
# DEPRECATED 2026-11-06: in-game checks are native now (`kmap probe replay`, see RECIPES.md). This Wine script stays as a parity reference.
# Starts a sandboxed game in the background (see wine_sandbox.sh for variables). Stop with: wineserver -k
set -u
GAME=$1; BUILD=$2; SB=$3; EXE=${4:-Cube.exe}; shift 4 || shift $#
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine32} WINEARCH=win32 WINEDEBUG=${WINEDEBUG:--all} DISPLAY=${DISPLAY:-:99}
pgrep -x Xvfb >/dev/null || (Xvfb :99 -screen 0 1030x800x24 >/dev/null 2>&1 &) ; sleep 0.5
pgrep -x pulseaudio >/dev/null || (pulseaudio -D --exit-idle-time=-1 >/dev/null 2>&1; pactl load-module module-null-sink sink_name=null >/dev/null 2>&1)
if [ "${KEEP:-0}" != 1 ]; then
  rm -rf "$SB"; mkdir -p "$SB"
  for f in "$GAME"/*; do n=$(basename "$f"); case "$n" in data*.db) cp "$f" "$SB/";; Save) ;; *) ln -s "$f" "$SB/$n";; esac; done
  mkdir -p "$SB/Save"   # its own saves: a link would write them into the game folder
  rm -f "$SB/zlib1.dll" "$SB/options.cfg"; cp "$GAME/zlib1.dll" "$SB/zlib1_vanilla.dll"
  printf 'fullscreen 0\r\nresolutionX 1024\r\nresolutionY 768\r\nantiAliasing 0\r\nrenderDistance 40\r\nsoundVolume 0\r\nmusicVolume 0\r\ncameraSpeed 80\r\ncameraSmoothness 0\r\nlanguage 0\r\n' > "$SB/options.cfg"
fi
cp "$BUILD/loader/zlib1.dll" "$SB/zlib1.dll"
mkdir -p "$SB/kyubu/symbols" "$SB/mods"
cp "$(dirname "$0")/../symbols/"*.toml "$SB/kyubu/symbols/"
[ -n "${MODS:-}" ] && for m in $MODS; do rm -rf "$SB/mods/$(basename "$m")"; cp -r "$m" "$SB/mods/"; done
[ -n "${CONFIG:-}" ] && cp "$CONFIG" "$SB/kyubu/config.toml"
wineserver -k 2>/dev/null; sleep 0.5
cd "$SB" && (wine "$EXE" "$@" > "$SB/wine-stdout.txt" 2>&1 &)
