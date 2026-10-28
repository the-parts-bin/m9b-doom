#!/usr/bin/env bash
# Renders a WAD's music to the WAV files the port streams (see README.md,
# Music): each D_* MUS lump through libADLMIDI (Nuked OPL3, Doom's GENMIDI
# bank, DMX volume model), then 24 kHz 16-bit mono with some gain (the
# renders peak around -10 dB). Optionally copies them to the SD card in
# the board with ../../core/tools/sdput/sdput.py (about 64 KB/s: 22 minutes
# for the shareware's 81 MB).
#
# Usage: make_music.sh WAD OUTDIR [--upload | --to CARDDIR] [LUMP...]
#   LUMP...   only these (e.g. D_E1M1 D_INTER); default: all of the WAD's
# Needs git, cmake, a C/C++ compiler and ffmpeg. libADLMIDI is fetched and
# built by ../../core/tools/midirender/build.sh the first time.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
WAD=$1
OUT=$(realpath -m "$2")
shift 2
PUT=()   # --upload: to the board's SD card; --to: into a card's file tree
if [ "${1-}" = "--upload" ]; then PUT=(put); shift; fi
if [ "${1-}" = "--to" ]; then PUT=(put --to "${2:?--to needs a directory}"); shift 2; fi
HERE=$(dirname "$(realpath "$0")")
mkdir -p "$OUT/raw" "$OUT/wav"

MR=$HERE/../../core/tools/midirender
"$MR/build.sh" >/dev/null
LIB=$MR/build/libADLMIDI
cc -O2 "$HERE/musrender.c" -I"$LIB/include" "$LIB/build/libADLMIDI.a" -lstdc++ -lm -o "$OUT/musrender"

"$OUT/musrender" "$WAD" "$OUT/raw"
LUMPS=("$@")
if [ ${#LUMPS[@]} -eq 0 ]; then
    for f in "$OUT"/raw/D_*.WAV; do LUMPS+=("$(basename "$f" .WAV)"); done
fi
ARGS=()
for l in "${LUMPS[@]}"; do
    ffmpeg -loglevel error -y -i "$OUT/raw/$l.WAV" -af "pan=mono|c0=0.5*c0+0.5*c1,volume=2.5" \
           -ar 24000 -c:a pcm_s16le -fflags +bitexact -map_metadata -1 "$OUT/wav/$l.WAV"
    ARGS+=("$OUT/wav/$l.WAV" "DOOMS/MUSIC/$l.WAV")
done
du -sh "$OUT/wav"
if [ ${#PUT[@]} -gt 0 ]; then
    python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${ARGS[@]}"
fi
