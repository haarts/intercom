#!/usr/bin/env bash
# Voice test over lanicom: speech on this laptop's speakers -> board mic -> lanicom -> Whisper.
#
# Usage: run-lanicom.sh [AUDIO.wav=berend-nl-pad.wav] [LANG=nl] [BOARD=lanicom-p4-e80332.local]
#   The board's "Talk" switch is on while the speech plays (like holding PTT).
#   Needs: the network key in ~/.config/lanicom/key, UDP 47100 open on this laptop,
#   and the laptop close to the board.
set -euo pipefail
cd "$(dirname "$0")"
WAV=${1:-berend-nl-pad.wav}; LANG_=${2:-nl}; BOARD=${3:-lanicom-p4-e80332.local}
PY=/home/harm/prj/intercom/.venv/bin
RUN=runs/$(date +%Y%m%d-%H%M%S)-lanicom; mkdir -p "$RUN"
cp "$WAV" "$RUN/reference.wav"; REFTEXT=-
if [ -f "${WAV%.wav}.txt" ]; then cp "${WAV%.wav}.txt" "$RUN/text.txt"; REFTEXT="$RUN/text.txt"; fi
DUR=$(python3 -c "import wave;w=wave.open('$RUN/reference.wav');print(w.getnframes()/w.getframerate())")

# 1. Record what the board sends (lanicom peer on this laptop); wait until it found the board.
#    15 s of slack: connecting to the board API takes a few seconds.
$PY/lanicom record "$RUN/received.wav" --duration "$(python3 -c "print($DUR+15)")" --name voicetest \
  > "$RUN/record.log" 2>&1 &
REC=$!
until grep -q "^+ " "$RUN/record.log" 2>/dev/null; do sleep 0.2; done

# 2. Talk on, play the speech, talk off.
$PY/python lanicom_board.py talk on "$BOARD"
sleep 0.3  # bus switch + mic warm-up
date +%s.%N > "$RUN/t0"
pw-play "$RUN/reference.wav"
sleep 0.5
$PY/python lanicom_board.py talk off "$BOARD"
wait $REC
cat "$RUN/record.log"

# 3. Transcribe and score.
.venv-stt/bin/python score.py "$REFTEXT" "$RUN/reference.wav" "$RUN/received.wav" "$LANG_" 2>"$RUN/score.err" | tee "$RUN/score.json"
echo "run: $RUN (reference ${DUR}s)"
