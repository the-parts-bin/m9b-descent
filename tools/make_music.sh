#!/usr/bin/env bash
# Renders Descent's music to the WAV files the port streams (see
# ../README.md, Music): takes the HMP songs out of descent.hog
# (tools/hogx.py), converts them to MIDI (tools/hmp2mid.py), renders them
# with libADLMIDI (../core/tools/midirender: Nuked OPL3, Descent's own FM
# instruments, HMI's volume model) and makes 24 kHz 16-bit mono at a
# common loudness (../core/tools/midirender/finish.py). Optionally copies
# them to the SD card in the board (DESCENT/MUSIC/) with
# ../../core/tools/sdput/sdput.py (about 70 KB/s: 17 minutes for the
# shareware's 8 songs, 69 MB).
#
# Usage: make_music.sh DESCENT.HOG OUTDIR [--upload | --to CARDDIR]
# Needs python3, ffmpeg, git, cmake and a C/C++ compiler.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
case "${3-}" in   # --upload: to the board's SD card; --to: into a card's file tree
"" | --upload) PUT=(put) ;;
--to) PUT=(put --to "${4:?--to needs a directory}") ;;
*) echo "unknown option $3" >&2; exit 1 ;;
esac
HOG=$1
OUT=$(realpath -m "$2")
HERE=$(dirname "$(realpath "$0")")
MR=$HERE/../../core/tools/midirender
mkdir -p "$OUT/hmp" "$OUT/mid" "$OUT/raw" "$OUT/wav"

"$MR/build.sh" >/dev/null
python3 "$HERE/hogx.py" "$HOG" "$OUT/hmp" '*.hmp'
args=()
for f in "$OUT"/hmp/*.hmp; do
    b=$(basename "$f" .hmp)
    B=$(echo "$b" | tr a-z A-Z)
    python3 "$HERE/hmp2mid.py" "$f" "$OUT/mid/$b.mid"
    "$MR/build/midirender" "$OUT/mid/$b.mid" "$OUT/raw/$b.wav" -b "HMI (Descent, Asterix)" -v hmi >/dev/null
    python3 "$MR/finish.py" "$OUT/raw/$b.wav" "$OUT/wav/$B.WAV"
    args+=("$OUT/wav/$B.WAV" "DESCENT/MUSIC/$B.WAV")
done
du -sh "$OUT/wav"
if [ -n "${3-}" ]; then
    python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${args[@]}"
fi
