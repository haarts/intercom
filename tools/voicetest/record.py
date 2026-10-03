"""Join the Mumble server and record what the ESP32 board transmits.

Writes a 48 kHz mono WAV on a real-time timeline: where the board stopped
sending, the WAV has silence, so cut-off words stay missing for the STT step.
Time zero is the moment the start-file appears (the runner writes it right
before playback starts).

Usage: record.py SERVER SECONDS OUT.wav START_FILE
"""

import json
import os
import struct
import sys
import time
import wave

import pymumble_py3 as pm
from pymumble_py3.constants import PYMUMBLE_CLBK_SOUNDRECEIVED

RATE = 48000
server, seconds, out_wav, start_file = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]

chunks = []  # (arrival_time, pcm)
others = {}


def on_sound(user, chunk):
    if user["name"].startswith("esp32"):
        chunks.append((time.time(), chunk.pcm))
    else:
        others[user["name"]] = others.get(user["name"], 0) + 1


m = pm.Mumble(server, "voicetest-recorder", port=64738, reconnect=False)
m.set_receive_sound(True)
m.callbacks.set_callback(PYMUMBLE_CLBK_SOUNDRECEIVED, on_sound)
m.start()
m.is_ready()
users = [u["name"] for u in m.users.values()]
print("READY", flush=True)

while not os.path.exists(start_file):
    time.sleep(0.01)
t0 = float(open(start_file).read())
time.sleep(max(0.0, t0 + seconds - time.time()))
m.stop()

# Lay chunks on the timeline. A chunk's arrival marks its end; contiguous
# chunks are placed back to back so network jitter doesn't add fake gaps.
total = int(seconds * RATE)
buf = bytearray(total * 2)
cursor = None
bursts = []
for arrival, pcm in chunks:
    n = len(pcm) // 2
    start = int((arrival - t0) * RATE) - n
    if cursor is not None and abs(start - cursor) < int(0.1 * RATE):
        start = cursor  # same burst
    else:
        bursts.append([start / RATE, None])
    end = start + n
    bursts[-1][1] = end / RATE
    if 0 <= start and end <= total:
        buf[start * 2 : end * 2] = pcm
    cursor = end

with wave.open(out_wav, "w") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(bytes(buf))

samples = struct.unpack("<%dh" % (len(buf) // 2), bytes(buf))
peak = max((abs(s) for s in samples), default=0)
print(
    json.dumps(
        {
            "users": users,
            "chunks": len(chunks),
            "bursts": [(round(a, 2), round(b, 2)) for a, b in bursts],
            "received_s": round(sum(len(p) for _, p in chunks) / 2 / RATE, 2),
            "peak": peak,
            "others_chunks": others,
        }
    )
)
