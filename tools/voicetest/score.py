"""Transcribe the reference and received audio with Whisper and score them.

Usage: score.py REFERENCE_TEXT_FILE|- REFERENCE.wav RECEIVED.wav [LANG=en]
Prints word error rate (WER) for both files, plus the transcripts, as JSON.
With "-" as text file, the reference text is Whisper's transcript of REFERENCE.wav.
"""

import json
import re
import sys

import wave

import jiwer
import numpy as np
from faster_whisper import WhisperModel

text_file, ref_wav, rx_wav = sys.argv[1:4]
lang = sys.argv[4] if len(sys.argv) > 4 else "en"


NUMBERS = {"en": "zero one two three four five six seven eight nine ten",
           "nl": "nul een twee drie vier vijf zes zeven acht negen tien"}


def norm(s):
    s = s.lower().replace("-", " ")
    # Whisper writes numbers as digits or words at will; compare them as words.
    words = NUMBERS.get(lang, NUMBERS["en"]).split()
    s = re.sub(r"\b(10|[0-9])\b", lambda m: words[int(m.group(1))], s)
    return re.sub(r"\s+", " ", re.sub(r"[^a-z0-9à-ÿ' ]", " ", s)).strip()


model = WhisperModel("small.en" if lang == "en" else "small", device="cpu", compute_type="int8")


def load_16k(path):
    # Read the WAV directly (faster-whisper's PyAV decoder breaks on newer PyAV).
    with wave.open(path) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    n = int(len(x) * 16000 / rate)
    return np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x).astype(np.float32)


def transcribe(path):
    segments, _ = model.transcribe(load_16k(path), language=lang, beam_size=5, vad_filter=False,
                                   condition_on_previous_text=False)
    return " ".join(seg.text.strip() for seg in segments)


reference = open(text_file).read() if text_file != "-" else transcribe(ref_wav)
out = {"reference": reference}
for label, path in (("baseline", ref_wav), ("received", rx_wav)):
    hyp = transcribe(path)
    out[label] = {"wer": round(jiwer.wer(norm(reference), norm(hyp)), 3), "text": hyp}
print(json.dumps(out, indent=1))
