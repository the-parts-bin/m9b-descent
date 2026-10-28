#!/usr/bin/env bash
# Fetches the Descent 1.4 shareware (desc14sw.zip, freely distributable)
# and extracts descent.hog and descent.pig into DIR (default: ../data).
#
# The zip holds an installer and a two-volume ARJ archive (DESCENT1.SOW,
# DESCENT2.SOW); descent.pig spans both volumes. 7-Zip extracts the first
# part from the first volume, and from the second volume the whole file
# with the first part as zeros, so the two are joined here. Needs curl,
# unzip, 7z and python3.
set -euo pipefail

HERE=$(dirname "$(realpath "$0")")
DEST=$(realpath -m "${1:-$HERE/../data}")
URL="https://www.classicdosgames.com/files/games/parallax/desc14sw.zip"
mkdir -p "$DEST"
if [ -s "$DEST/descent.hog" ] && [ -s "$DEST/descent.pig" ]; then
    echo "OK: data already in $DEST"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fL --progress-bar -o "$TMP/desc14sw.zip" "$URL"
unzip -q "$TMP/desc14sw.zip" -d "$TMP"
cp "$TMP/DESCENT1.SOW" "$TMP/DESCENT.ARJ"
cp "$TMP/DESCENT2.SOW" "$TMP/DESCENT.A01"
7z x -y "$TMP/DESCENT.ARJ" -o"$TMP/v1" DESCENT.HOG DESCENT.PIG > /dev/null
7z x -y "$TMP/DESCENT.A01" -o"$TMP/v2" DESCENT.PIG > /dev/null
cp "$TMP/v1/DESCENT.HOG" "$DEST/descent.hog"
python3 - "$TMP/v1/DESCENT.PIG" "$TMP/v2/DESCENT.PIG" "$DEST/descent.pig" <<'EOF'
import sys
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
assert len(b) > len(a) and not any(b[:len(a)]), "unexpected volume layout"
open(sys.argv[3], "wb").write(a + b[len(a):])
EOF
ls -l "$DEST/descent.hog" "$DEST/descent.pig"
echo "OK: data in $DEST (sizes should be 2339773 and 5092871)"
