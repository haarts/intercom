#!/usr/bin/env bash
# End-to-end voice test: TTS -> speaker in the closet -> P4 mic -> Mumble -> Whisper.
#
# Usage: run.sh [PLAYER=local|meklit] [MODE=always_on|ptt] [TEXT_FILE|AUDIO.wav=sentences.txt] [LANG=en]
#   A .wav instead of a text file is played as-is; its Whisper transcript is the reference.
#   PLAYER local:  play on this laptop's default output (e.g. the Bluetooth speaker), its own volume.
#   PLAYER meklit: play on meklit's speakers at $MEKLIT_VOL (default 0.4).
#   MODE ptt:      board in push_to_talk with "Talk" on, i.e. no voice detection.
set -euo pipefail
cd "$(dirname "$0")"
PLAYER=${1:-local}; MODE=${2:-always_on}; TEXT=${3:-sentences.txt}; LANG_=${4:-en}
MEKLIT_VOL=${MEKLIT_VOL:-0.4}
SERVER=192.168.188.45
BOARD="/home/harm/prj/intercom/.venv/bin/python board.py"
RUN=runs/$(date +%Y%m%d-%H%M%S)-$PLAYER-$MODE; mkdir -p "$RUN"
R='XDG_RUNTIME_DIR=/run/user/$(id -u)'

# 1. Reference audio: the given recording, or speech for the text generated on meklit (espeak-ng lives there).
if [[ "$TEXT" == *.wav ]]; then
  cp "$TEXT" "$RUN/reference.wav"; scp -q "$TEXT" meklit:/tmp/voicetest.wav; REFTEXT=-
  # A transcript next to the recording (same name, .txt) beats Whisper's guess as the reference.
  if [ -f "${TEXT%.wav}.txt" ]; then cp "${TEXT%.wav}.txt" "$RUN/text.txt"; REFTEXT="$RUN/text.txt"; fi
else
  scp -q "$TEXT" meklit:/tmp/voicetest.txt
  ssh -o BatchMode=yes meklit "espeak-ng -v en-us -s 150 -f /tmp/voicetest.txt -w /tmp/voicetest.wav"
  scp -q meklit:/tmp/voicetest.wav "$RUN/reference.wav"
  cp "$TEXT" "$RUN/text.txt"; REFTEXT="$RUN/text.txt"
fi
DUR=$(python3 -c "import wave;w=wave.open('$RUN/reference.wav');print(w.getnframes()/w.getframerate())")

# 2. Record the board while the speech plays.
.venv-mumble/bin/python record.py $SERVER "$(python3 -c "print($DUR+3)")" "$RUN/received.wav" "$RUN/t0" \
  > "$RUN/record.json" 2> "$RUN/record.err" &
REC=$!
until grep -q READY "$RUN/record.json" 2>/dev/null; do sleep 0.2; done
if [ "$MODE" = ptt ]; then $BOARD mode push_to_talk; $BOARD talk on; fi
date +%s.%N > "$RUN/t0.tmp" && mv "$RUN/t0.tmp" "$RUN/t0"
if [ "$PLAYER" = local ]; then
  pw-play "$RUN/reference.wav"
else
  ssh -o BatchMode=yes meklit "bash -c '$R wpctl set-volume @DEFAULT_AUDIO_SINK@ $MEKLIT_VOL; $R pw-play /tmp/voicetest.wav; $R wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.4'"
fi
wait $REC
if [ "$MODE" = ptt ]; then $BOARD talk off; $BOARD mode always_on; fi
grep -v READY "$RUN/record.json"

# 3. Transcribe and score.
.venv-stt/bin/python score.py "$REFTEXT" "$RUN/reference.wav" "$RUN/received.wav" "$LANG_" 2>"$RUN/score.err" | tee "$RUN/score.json"
echo "run: $RUN (reference ${DUR}s)"
