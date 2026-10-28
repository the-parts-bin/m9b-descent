#!/usr/bin/env bash
# Loads a Descent build (default build-prof/) and starts level 1: the first
# pilot, New Game, Rookie, briefing skipped; then waits for the level to
# load. For tests (the pilot file must exist: create one by hand first).
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build-prof}
python3 ../core/tools/run.py "$B/descent.bin" --listen 25 > /dev/null 2>&1
python3 ../core/tools/m9bctl.py keys "<enter><wait:3000><enter><wait:2000><enter><wait:5000><esc>"
sleep 40
