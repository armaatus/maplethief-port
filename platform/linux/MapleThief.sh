#!/bin/sh
# MapleThief launcher of the Linux game folder (#84, ADR 0065, docs/play/linux-and-steam-deck.md).
# scripts/linux-game copies it to the folder's top level, next to bin/MapleThief and game/.
#
#   ./MapleThief.sh [MapleThief options...]   start the game (the options: README.md, Build)
#   ./MapleThief.sh --install-desktop         add MapleThief to the desktop's application menu
#                                             (and so to Steam's "Add a Non-Steam Game" list)
#
# It runs the game from the copy of your bundle in game/ (MAPLETHIEF_BUNDLE_DIR, unless you set
# it), if the folder has one: the player download doesn't (ADR 0125), and its game imports your
# IPA into game/ itself instead (ADR 0127). scripts/linux-game player and the linux-player preset
# install it at the top of the player tarball, next to bin/MapleThief and icon.png. Under gamescope (Steam Deck Game Mode, or `gamescope -- ./MapleThief.sh`) it opens a
# window as large as the screen, 1280x800 (MAPLETHIEF_GAMESCOPE_WINDOW sets another size), so
# that the game's own scaling (settings.ini scale_mode) fills it instead of gamescope's; a
# --window you pass wins. Saves and settings.ini live in data/ in this folder (ADR 0127), or in
# ~/.local/share/MapleThief when the folder can't be written or user-data.txt is here.
set -eu

here="$(cd "$(dirname "$0")" && pwd -P)"

if [ "${1:-}" = "--install-desktop" ]; then
    case "$here" in
        *[\"\`\$\\%]*)
            echo "MapleThief.sh: the folder's path $here has a character a .desktop file" \
                "can't quote; move the folder and run --install-desktop again" >&2
            exit 1
            ;;
    esac
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    mkdir -p "$apps"
    desktop="$apps/io.github.armaatus.maplethief.desktop"
    {
        echo "[Desktop Entry]"
        echo "Type=Application"
        echo "Name=MapleThief"
        echo "Comment=MapleStory Thief Edition, played from your own copy"
        echo "Exec=\"$here/MapleThief.sh\""
        echo "Path=$here"
        [ -f "$here/icon.png" ] && echo "Icon=$here/icon.png"
        echo "Terminal=false"
        echo "Categories=Game;"
    } >"$desktop"
    echo "MapleThief.sh: wrote $desktop"
    exit 0
fi

# The personal game folder carries the bundle in game/; the player download (#182, ADR 0125) has
# none, and the game finds the copy it imported into game/ (or its data folder's game/) instead.
if [ -z "${MAPLETHIEF_BUNDLE_DIR:-}" ] && [ -d "$here/game/MapleThief.app" ]; then
    MAPLETHIEF_BUNDLE_DIR="$here/game/MapleThief.app"
    export MAPLETHIEF_BUNDLE_DIR
fi

window_given=0
for arg in "$@"; do
    [ "$arg" = "--window" ] && window_given=1
done
if [ "$window_given" -eq 0 ] &&
    { [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ] || [ "${XDG_CURRENT_DESKTOP:-}" = "gamescope" ]; }; then
    size="${MAPLETHIEF_GAMESCOPE_WINDOW:-1280x800}"
    echo "MapleThief.sh: gamescope: --window $size" >&2
    set -- --window "$size" "$@"
fi

exec "$here/bin/MapleThief" "$@"
