#!/bin/sh
# MapleThief launcher of the Linux game folder (#84, ADR 0065, docs/play/linux-and-steam-deck.md).
# scripts/linux-game copies it to the folder's top level, next to bin/MapleThief and game/.
#
#   ./MapleThief.sh [MapleThief options...]   start the game (the options: README.md, Build)
#   ./MapleThief.sh --install-desktop         add MapleThief to the desktop's application menu
#                                             (and so to Steam's "Add a Non-Steam Game" list)
#   ./MapleThief.sh --uninstall-desktop       remove that menu entry again (#332)
#
# It runs the game from the copy of your bundle in game/ (MAPLETHIEF_BUNDLE_DIR, unless you set
# it), if the folder has one: the player download doesn't (ADR 0125), and its game imports your
# IPA into game/ itself instead (ADR 0127). scripts/linux-game player and the linux-player preset
# install it at the top of the player tarball, next to bin/MapleThief and icon.png. Under gamescope (Steam Deck Game Mode, or `gamescope -- ./MapleThief.sh`) it opens a
# window as large as the screen, 1280x800 (MAPLETHIEF_GAMESCOPE_WINDOW sets another size), so
# that the game's own scaling (settings.ini scale_mode) fills it instead of gamescope's; a
# --window you pass wins. Saves and settings.ini live in data/ in this folder (ADR 0127), or in
# ~/.local/share/MapleThief when the folder can't be written or user-data.txt is here.
#
# The player AppImage runs it as its AppRun (ADR 0193): from the image's read-only mount, with
# APPIMAGE set to the .AppImage file, which --install-desktop then names (and copies the icon
# out of the mount). In the Flatpak bundle (FLATPAK_ID set) Flatpak installs the menu entry
# itself, so --install-desktop and --uninstall-desktop only say so.
set -eu

here="$(cd "$(dirname "$0")" && pwd -P)"

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
desktop="$data_home/applications/io.github.armaatus.maplethief.desktop"
# The AppImage's icon, copied out of its temporary mount.
appimage_icon="$data_home/icons/hicolor/256x256/apps/io.github.armaatus.maplethief.png"

if [ -n "${FLATPAK_ID:-}" ]; then
    case "${1:-}" in
        --install-desktop | --uninstall-desktop)
            echo "MapleThief.sh: Flatpak adds and removes MapleThief's menu entry itself"
            exit 0
            ;;
    esac
fi

if [ "${1:-}" = "--uninstall-desktop" ]; then
    rm -f "$appimage_icon"
    if [ -f "$desktop" ]; then
        rm -f "$desktop"
        echo "MapleThief.sh: removed $desktop"
    else
        echo "MapleThief.sh: no menu entry at $desktop; nothing to remove"
    fi
    exit 0
fi

if [ "${1:-}" = "--install-desktop" ]; then
    # In an AppImage the entry starts the .AppImage file: $here is a temporary mount.
    program="$here/MapleThief.sh"
    folder="$here"
    icon="$here/icon.png"
    if [ -n "${APPIMAGE:-}" ]; then
        program="$APPIMAGE"
        folder="$(dirname "$APPIMAGE")"
    fi
    case "$program" in
        *[\"\`\$\\%]*)
            echo "MapleThief.sh: the path $program has a character a .desktop file" \
                "can't quote; move it and run --install-desktop again" >&2
            exit 1
            ;;
    esac
    if [ -n "${APPIMAGE:-}" ] && [ -f "$icon" ]; then
        mkdir -p "$(dirname "$appimage_icon")"
        cp "$icon" "$appimage_icon"
        icon="$appimage_icon"
    fi
    mkdir -p "$(dirname "$desktop")"
    {
        echo "[Desktop Entry]"
        echo "Type=Application"
        echo "Name=MapleThief"
        echo "Comment=MapleStory Thief Edition, played from your own copy"
        echo "Exec=\"$program\""
        echo "Path=$folder"
        [ -f "$icon" ] && echo "Icon=$icon"
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
