#!/usr/bin/env bash
# Puts the shareware Doom 1.9 IWAD (DOOM1.WAD, 4,196,020 bytes, MD5
# f0cefca49926d00903cf57551d901abe) into DIR (default: ../data), from
# SOURCE, which is any of:
#   - DOOM1.WAD itself
#   - doom19s.zip, id Software's shareware 1.9 distribution
#   - the directory doom19s.zip unpacks to (DOOMS_19.1, DOOMS_19.2)
# The zip's DOOMS_19.1 and DOOMS_19.2 are one LHA archive in two parts
# (DEICE.EXE unpacks them on DOS); 7-Zip reads the joined parts. Needs
# unzip and 7z for the zip and the directory.
#
# Usage: get_data.sh SOURCE [DIR]
set -euo pipefail

if [ $# -lt 1 ]; then
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
SRC=$(realpath "$1")
HERE=$(dirname "$(realpath "$0")")
DEST=$(realpath -m "${2:-$HERE/../data}")
MD5=f0cefca49926d00903cf57551d901abe
mkdir -p "$DEST"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if [ -d "$SRC" ]; then
    parts=$SRC
elif [ "$(head -c 4 "$SRC")" = IWAD ]; then
    cp "$SRC" "$TMP/DOOM1.WAD"
else
    unzip -q "$SRC" -d "$TMP/zip"
    parts=$TMP/zip
fi
if [ ! -e "$TMP/DOOM1.WAD" ]; then
    p1=$(find "$parts" -maxdepth 1 -iname 'dooms_19.1' | head -1)
    p2=$(find "$parts" -maxdepth 1 -iname 'dooms_19.2' | head -1)
    [ -n "$p1" ] && [ -n "$p2" ] || { echo "no DOOMS_19.1 and DOOMS_19.2 in $SRC" >&2; exit 1; }
    cat "$p1" "$p2" > "$TMP/dooms_19.lzh"
    7z e -y "$TMP/dooms_19.lzh" -o"$TMP" DOOM1.WAD > /dev/null
fi
if [ "$(md5sum < "$TMP/DOOM1.WAD" | cut -d' ' -f1)" != $MD5 ]; then
    echo "warning: this DOOM1.WAD isn't the shareware 1.9 one (MD5 $MD5)" >&2
fi
cp "$TMP/DOOM1.WAD" "$DEST/DOOM1.WAD"
echo "OK: $DEST/DOOM1.WAD"
