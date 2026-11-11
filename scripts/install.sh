#!/bin/sh
# sloperinaGZ installer for Linux, macOS and Steam Deck.
#
#   ./install.sh                      list the emulators found and ask where to install
#   ./install.sh <target> [<path>]    target: lichii fritchii eden yuzu suyu sudachi citron torzu ryujinx switch folder
#                                     path: the emulator's user folder, or the SD card's root (switch)
#   ./install.sh --uninstall ...      remove sloperinaGZ instead (your settings stay)
#   ./install.sh --yes ...            don't ask before replacing files
#
# It copies the module (subsdk9), the game-version patches and the translation
# template into the layout each emulator or Atmosphere reads. Nothing else on your system is changed.
set -e

TITLE=0400486024DEC000
TITLE_LC=0400486024dec000
NAME=sloperinaGZ
BUILDS="33FA6C2C23848282A77E8B88A0A93F4887C55920 E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
HERE=$(cd "$(dirname "$0")" && pwd)
MODULE="$HERE/files/subsdk9"
TEMPLATE="$HERE/files/lang/template.json"

UNINSTALL=0
YES=0
while [ $# -gt 0 ]; do
  case "$1" in
    --uninstall) UNINSTALL=1; shift ;;
    --yes|-y) YES=1; shift ;;
    *) break ;;
  esac
done
TARGET=${1:-}
TPATH=${2:-}

[ -f "$MODULE" ] || { echo "Missing $MODULE. Run this from the unzipped release folder."; exit 1; }
for build in $BUILDS; do
  f="$HERE/files/$build.ips"
  [ -f "$f" ] || { echo "Missing $f. Run this from the unzipped release folder."; exit 1; }
done

# where an emulator keeps its data: the first that exists of the usual places
data_dir() {
  case "$(uname -s)" in
    Darwin) base="$HOME/Library/Application Support" ;;
    *) base="${XDG_DATA_HOME:-$HOME/.local/share}" ;;
  esac
  case "$1" in
    ryujinx)
      for d in "${XDG_CONFIG_HOME:-$HOME/.config}/Ryujinx" "$HOME/Library/Application Support/Ryujinx" \
               "$HOME/.var/app/org.ryujinx.Ryujinx/config/Ryujinx" "$HOME/.var/app/io.github.ryubing.Ryujinx/config/Ryujinx"; do
        [ -d "$d" ] && { echo "$d"; return; }
      done ;;
    *)
      for d in "$base/$1" "$HOME/.var/app/"*"/data/$1"; do
        [ -d "$d" ] && { echo "$d"; return; }
      done ;;
  esac
  echo ""
}

# how a target lays out mods: the yuzu family, Ryujinx or Atmosphere
kind_of() { case "$1" in ryujinx) echo ryujinx ;; switch) echo switch ;; *) echo yuzu ;; esac; }

# may $1 be written? Yes if it isn't there yet or --yes was given; otherwise ask
replace_ok() {
  if [ "$YES" = 1 ] || [ ! -e "$1" ]; then return 0; fi
  printf '  %s already exists. Replace it? [Y/n] ' "$1"
  read -r a
  case "$a" in ""|y|Y*) return 0 ;; *) return 1 ;; esac
}

copy_one() { # from to-dir to-name
  mkdir -p "$2"
  if replace_ok "$2/$3"; then cp -f "$1" "$2/$3"; echo "  copied  $2/$3"; else echo "  skipped $2/$3"; fi
}

# path note: delete path if it is there, saying so (followed by note)
remove() { [ -e "$1" ] && { rm -rf "$1"; echo "  removed $1$2"; }; return 0; }

layout() { # kind dir: sets EXEFS PATCHDIR LANGDIR OLD REMOVE
  case "$1" in
    yuzu)    EXEFS="$2/load/$TITLE/$NAME/exefs"; PATCHDIR=""; LANGDIR="$2/sdmc/$NAME/lang"
             OLD="$2/load/$TITLE/ootgz"; REMOVE="$2/load/$TITLE/$NAME" ;;
    ryujinx) EXEFS="$2/mods/contents/$TITLE_LC/$NAME/exefs"; PATCHDIR=""; LANGDIR="$2/sdcard/$NAME/lang"
             OLD="$2/mods/contents/$TITLE_LC/ootgz"; REMOVE="$2/mods/contents/$TITLE_LC/$NAME" ;;
    switch)  EXEFS="$2/atmosphere/contents/$TITLE/exefs"; PATCHDIR="$2/atmosphere/exefs_patches/$NAME"
             LANGDIR="$2/$NAME/lang"; OLD="$2/atmosphere/exefs_patches/ootgz"; REMOVE="$PATCHDIR" ;;
  esac
}

echo "sloperinaGZ installer"
echo "For The Legend of Zelda: Ocarina of Time (Switch 2), versions 1.0.0 and 1.1.1."

if [ -z "$TARGET" ]; then
  echo
  echo "Where should it go?"
  i=0; CHOICES=""
  for e in lichii fritchii eden yuzu suyu sudachi citron torzu ryujinx; do
    d=$(data_dir "$e")
    [ -n "$d" ] || continue
    i=$((i + 1)); CHOICES="$CHOICES$e|$d
"
    echo "  $i. $e  ($d)"
  done
  i=$((i + 1)); echo "  $i. Nintendo Switch SD card (Atmosphere)"; CHOICES="${CHOICES}switch|
"
  i=$((i + 1)); echo "  $i. Another emulator folder (yuzu-style: has load and sdmc)"; CHOICES="${CHOICES}folder|
"
  printf "Number: "; read -r n
  case $n in ''|*[!0-9]*) n=0 ;; esac
  line=$(printf '%s' "$CHOICES" | sed -n "${n}p")
  [ -n "$line" ] || { echo "Nothing chosen."; exit 1; }
  TARGET=${line%%|*}; TPATH=${line#*|}
fi

KIND=$(kind_of "$TARGET")
case "$TARGET" in lichii|fritchii|eden|yuzu|suyu|sudachi|citron|torzu|ryujinx|switch|folder) ;; *) echo "Unknown target '$TARGET'."; exit 1 ;; esac
[ -n "$TPATH" ] || [ "$TARGET" = switch ] || [ "$TARGET" = folder ] || TPATH=$(data_dir "$TARGET")
if [ -z "$TPATH" ]; then
  if [ "$KIND" = switch ]; then printf "SD card (its folder, e.g. /media/you/SWITCH): "; else printf "The emulator's user folder (with load and sdmc): "; fi
  read -r TPATH
fi
TPATH=${TPATH%/}
[ -d "$TPATH" ] || { echo "$TPATH doesn't exist."; exit 1; }
if [ "$KIND" = switch ] && [ ! -d "$TPATH/atmosphere" ] && [ "$YES" != 1 ]; then
  printf "%s has no atmosphere folder. Is it really the Switch's SD card? [y/N] " "$TPATH"; read -r a
  case "$a" in y|Y*) ;; *) exit 1 ;; esac
fi

layout "$KIND" "$TPATH"
if [ "$UNINSTALL" = 1 ]; then
  echo; echo "Removing sloperinaGZ from $TPATH"
  remove "$REMOVE" ""; remove "$OLD" ""
  [ "$KIND" = switch ] && remove "$EXEFS/subsdk9" ""
  echo "Done. Your settings (in the SD card's $NAME folder) were kept; delete that folder to remove them too."
else
  echo; echo "Installing sloperinaGZ in $TPATH"
  patch_dest=${PATCHDIR:-$EXEFS}
  replace_ok "$EXEFS/subsdk9" || exit 0
  for build in $BUILDS; do
    replace_ok "$patch_dest/$build.ips" || exit 0
  done
  remove "$OLD" "  (an older ootgz install: two copies would clash)"
  (
    YES=1
    for build in $BUILDS; do
      copy_one "$HERE/files/$build.ips" "$patch_dest" "$build.ips"
    done
    copy_one "$MODULE" "$EXEFS" subsdk9
  )
  [ -f "$TEMPLATE" ] && copy_one "$TEMPLATE" "$LANGDIR" template.json
  echo; echo "Done. Start the game and press R + right stick down for the menu."
  if [ "$KIND" = switch ]; then echo "Put the SD card back in the console and start the game with Atmosphere."; fi
fi
