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
    for d in "$HERE"/*/CMakeLists.txt; do bash "$0" "$(basename "$(dirname "$d")")"; done
    exit 0
fi
[ -n "$BOARD" ] && [ -f "$HERE/$BOARD/CMakeLists.txt" ] || { echo "Usage: bash build.sh <board> [clean|menuconfig]"; ls -d "$HERE"/*/CMakeLists.txt 2>/dev/null | xargs -n1 dirname | xargs -n1 basename; exit 1; }
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
cd "$HERE/$BOARD"
case "${2:-build}" in
    clean) idf.py fullclean ;;
    menuconfig) idf.py menuconfig ;;
    *) if [ sdkconfig.defaults -nt sdkconfig ]; then      # idf.py keeps an old sdkconfig over newer defaults: start it over
           echo "sdkconfig.defaults changed: the configuration starts over"; rm -f sdkconfig
       fi
       idf.py build                    # the target comes from sdkconfig.defaults
       python "$HERE/tools/pack.py" "$BOARD" build "$HERE/dist" ;;
esac
