"""Send a 48 kHz mono WAV into the Mumble channel as a normal client: speak.py SERVER WAV [REPEATS]"""
import sys, time, wave
import pymumble_py3 as pm

server, path = sys.argv[1], sys.argv[2]
repeats = int(sys.argv[3]) if len(sys.argv) > 3 else 1
with wave.open(path) as w:
    assert w.getframerate() == 48000 and w.getnchannels() == 1 and w.getsampwidth() == 2
    pcm = w.readframes(w.getnframes())
m = pm.Mumble(server, "voicetest-speaker", port=64738, reconnect=False)
m.start(); m.is_ready()
for _ in range(repeats):
    m.sound_output.add_sound(pcm)
    while m.sound_output.get_buffer_size() > 0:
        time.sleep(0.05)
    time.sleep(1.5)
m.stop()
print("done")
