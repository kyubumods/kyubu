#!/usr/bin/env bash
# Compiles the SDK headers with every toolchain available here (task 4.4), and the generated cubeworld headers
# (kmap export, add-mapping-workspace task 4.3). A missing toolchain is skipped; finding none at all fails.
# MSVC runs when cl is on PATH (an x86 developer environment: vcvarsall.bat x86); on Linux, clang in MSVC mode
# checks the _MSC_VER code paths.
set -u
cd "$(dirname "$0")"
INC=../../sdk/include
PROBE=../../mods-dev/kyubu_probe/include
LOG=${TMPDIR:-/tmp}/kyubu-hdr.log
fail=0
ran=0
run() { local name=$1; shift; ran=1; if "$@" >"$LOG" 2>&1; then echo "ok    $name"; else echo "FAIL  $name"; cat "$LOG"; fail=1; fi; }
if command -v i686-w64-mingw32-gcc >/dev/null; then
  run "C99 (MinGW gcc, -pedantic -Werror)"      i686-w64-mingw32-gcc -std=c99 -pedantic -Wall -Wextra -Werror -fsyntax-only -I$INC c99_check.c
  run "C11 (MinGW gcc)"                          i686-w64-mingw32-gcc -std=c11 -Wall -Werror -fsyntax-only -I$INC c99_check.c
  run "C++20 kyubu.hpp (MinGW g++)"              i686-w64-mingw32-g++ -std=c++20 -Wall -Wextra -Werror -fsyntax-only -I$INC cxx_check.cpp
  run "cubeworld C99 (MinGW gcc, -pedantic)"     i686-w64-mingw32-gcc -std=c99 -pedantic -Wall -Wextra -Werror -fsyntax-only -I$INC cubeworld_check.c
  run "cubeworld C11 (MinGW gcc)"                i686-w64-mingw32-gcc -std=c11 -pedantic -Wall -Wextra -Werror -fsyntax-only -I$INC cubeworld_check.c
  run "cubeworld C++20 (MinGW g++)"              i686-w64-mingw32-g++ -std=c++20 -Wall -Wextra -Werror -fsyntax-only -I$INC cubeworld_check.cpp
  run "kyubu_probe.h C99 (MinGW gcc, -pedantic)" i686-w64-mingw32-gcc -std=c99 -pedantic -Wall -Wextra -Werror -fsyntax-only -I$INC -I$PROBE probe_check.c
  run "kyubu_probe.h C++20 (MinGW g++)"          i686-w64-mingw32-g++ -std=c++20 -Wall -Wextra -Werror -fsyntax-only -x c++ -I$INC -I$PROBE probe_check.c
else
  echo "skip  MinGW gcc (i686-w64-mingw32-gcc not found)"
fi
if command -v clang >/dev/null; then
  run "C99 (clang, MSVC target, freestanding)" clang --target=i686-pc-windows-msvc -ffreestanding -std=c99 -Wall -Werror -fsyntax-only -I$INC c99_check.c
  run "C++20 kyubu.hpp (clang, MinGW target)"  clang++ --target=i686-w64-mingw32 -std=c++20 -Wall -Werror -fsyntax-only -I$INC cxx_check.cpp
  run "cubeworld C99 (clang, MSVC target)"     clang --target=i686-pc-windows-msvc -ffreestanding -std=c99 -pedantic -Wall -Wextra -Werror -fsyntax-only -I$INC cubeworld_check.c
  run "cubeworld C++20 (clang, MSVC target)"   clang++ --target=i686-pc-windows-msvc -ffreestanding -std=c++20 -Wall -Wextra -Werror -fsyntax-only -I$INC cubeworld_check.cpp
  run "kyubu_probe.h C99 (clang, MSVC target)" clang --target=i686-pc-windows-msvc -ffreestanding -std=c99 -pedantic -Wall -Wextra -Werror -fsyntax-only -I$INC -I$PROBE probe_check.c
else
  echo "skip  clang (not found)"
fi
if command -v cl >/dev/null; then
  run "cubeworld C (MSVC, default C mode)"     cl -nologo -W4 -WX -Zs -TC -I$INC cubeworld_check.c
  run "cubeworld C11 (MSVC)"                   cl -nologo -W4 -WX -Zs -TC -std:c11 -I$INC cubeworld_check.c
  run "cubeworld C++20 (MSVC)"                 cl -nologo -W4 -WX -Zs -TP -std:c++20 -I$INC cubeworld_check.cpp
  run "kyubu_probe.h C and C++20 (MSVC)"       sh -c "cl -nologo -W4 -WX -Zs -TC -I$INC -I$PROBE probe_check.c && cl -nologo -W4 -WX -Zs -TP -std:c++20 -I$INC -I$PROBE probe_check.c"
else
  echo "skip  MSVC (cl not on PATH: run from an x86 developer environment)"
fi
if [ $ran = 0 ]; then echo "FAIL  no compiler found"; exit 1; fi
exit $fail
