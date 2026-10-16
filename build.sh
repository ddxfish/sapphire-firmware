#!/bin/bash
# Build one board's firmware and put what a flasher needs in dist/.
#   bash build.sh <board>              build, then dist/
#   bash build.sh <board> clean        start over
#   bash build.sh <board> menuconfig
#   bash build.sh all                  every board that has a CMakeLists.txt
# <board> is a folder here. Needs ESP-IDF v5.5: set IDF_PATH (and IDF_TOOLS_PATH
# if the tools are not in their usual place). See README.md.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BOARD="${1:-}"
if [ "$BOARD" = all ]; then
    for d in "$HERE"/*/CMakeLists.txt; do
        d="$(dirname "$d")"
        if [ -d "$d/main/boards" ]; then for b in "$d"/main/boards/*.c; do bash "$0" "$(basename "$b" .c)"; done
        else bash "$0" "$(basename "$d")"; fi
    done
    exit 0
fi
# A board is a folder of its own (ai-speaker-mic, tdongle) or one of a family's boards
# (cyd/main/boards/<board>.c: one program, built once per board, into build/<board>)
DIR="$HERE/$BOARD"; FAMILY=""
if [ -n "$BOARD" ] && [ ! -f "$DIR/CMakeLists.txt" ]; then
    for f in "$HERE"/*/main/boards/"$BOARD".c; do [ -f "$f" ] && DIR="$(cd "$(dirname "$f")/../.." && pwd)" && FAMILY=yes; done
fi
[ -n "$BOARD" ] && [ -f "$DIR/CMakeLists.txt" ] || {
    echo "Usage: bash build.sh <board> [clean|menuconfig]"
    for d in "$HERE"/*/CMakeLists.txt; do d="$(dirname "$d")"; [ -d "$d/main/boards" ] && ls "$d"/main/boards/*.c | xargs -n1 basename | sed 's/\.c$//' || basename "$d"; done
    exit 1
}
# With no IDF_PATH: an ESP-IDF unpacked beside this folder (../tmp/esp-idf-dl), the way Sapphire's own tree keeps it
if [ -z "${IDF_PATH:-}" ]; then
    near=("$HERE"/../tmp/esp-idf-dl/esp-idf-v*)
    [ -d "${near[0]}" ] && export IDF_PATH="${near[0]}"
    [ -z "${IDF_TOOLS_PATH:-}" ] && [ -d "$HERE/../tmp/esp-idf-tools" ] && export IDF_TOOLS_PATH="$HERE/../tmp/esp-idf-tools"
fi
[ -f "${IDF_PATH:-}/export.sh" ] || { echo "No ESP-IDF found. Set IDF_PATH to an ESP-IDF v5.5 folder. See README.md."; exit 1; }
export GIT_CEILING_DIRECTORIES="$(dirname "$HERE")"     # the build's own git calls stay in this repository
# export.sh names the tools' Python venv after the python on PATH; inside a conda
# env that is not the one idf_tools.py made. With one venv installed and none named, use it.
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ] && [ -n "${IDF_TOOLS_PATH:-}" ]; then
    envs=("$IDF_TOOLS_PATH"/python_env/idf*_env)
    [ ${#envs[@]} -eq 1 ] && [ -d "${envs[0]}" ] && export IDF_PYTHON_ENV_PATH="${envs[0]}"
fi
. "$IDF_PATH/export.sh" > /dev/null
cd "$DIR"
if [ -n "$FAMILY" ]; then
    BUILD="build/$BOARD"
    EXTRA=(-B "$BUILD" -D "BOARD=$BOARD" -D "SDKCONFIG=$DIR/$BUILD/sdkconfig")
    NAME="$(sed -n 's|^// NAME: ||p' "main/boards/$BOARD.h" | head -1)"
else
    BUILD=build; EXTRA=(); NAME=""
fi
case "${2:-build}" in
    clean) idf.py "${EXTRA[@]}" fullclean ;;
    menuconfig) idf.py "${EXTRA[@]}" menuconfig ;;
    *) if [ -f "$BUILD/sdkconfig" ] && [ sdkconfig.defaults -nt "$BUILD/sdkconfig" ] || [ -f sdkconfig ] && [ sdkconfig.defaults -nt sdkconfig ]; then
           echo "sdkconfig.defaults changed: the configuration starts over"; rm -f "$BUILD/sdkconfig" sdkconfig   # idf.py keeps an old one over newer defaults
       fi
       mkdir -p "$BUILD"
       idf.py "${EXTRA[@]}" build       # the target comes from sdkconfig.defaults
       python "$HERE/tools/pack.py" "$BOARD" "$BUILD" "$HERE/dist" ${NAME:+--name "$NAME"} ;;
esac
