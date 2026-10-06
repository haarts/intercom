"""The CLI end to end: one process sends a file, another records it (real UDP on 127.0.0.x)."""

import shutil
import subprocess
import sys
import wave

import pytest

from lanicom import opus

from test_node import can_bind_loopback_aliases

pytestmark = pytest.mark.skipif(
    not (can_bind_loopback_aliases() and shutil.which("ffmpeg") and opus.available()),
    reason="needs loopback aliases, ffmpeg and libopus",
)

PORT = "47198"


def cli(*args, home):
    common = ["--key", "cli-test-key-123", "--port", PORT, "--no-broadcast"]
    cmd, rest = args[0], list(args[1:])
    return subprocess.Popen([sys.executable, "-m", "lanicom", cmd, *common, *rest], env={"XDG_CONFIG_HOME": str(home), "PATH": "/usr/bin:/bin"},
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def test_send_then_record(tmp_path):
    tone = tmp_path / "tone.wav"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=600:duration=1", str(tone)], check=True)
    out = tmp_path / "out.wav"
    # Same port, different loopback addresses.
    rec = cli("record", str(out), "--duration", "3", "--name", "rec", "--bind", "127.0.0.1", "--peer", "127.0.0.2", home=tmp_path / "a")
    send = cli("send", str(tone), "--name", "snd", "--bind", "127.0.0.2", "--peer", "127.0.0.1", home=tmp_path / "b")
    send_out, _ = send.communicate(timeout=20)
    rec_out, _ = rec.communicate(timeout=20)
    assert send.returncode == 0, send_out
    assert "sent 1.0 s" in send_out
    assert rec.returncode == 0, rec_out
    with wave.open(str(out)) as w:
        pcm = w.readframes(w.getnframes())
    samples = memoryview(pcm).cast("h")
    energy = sum(s * s for s in samples)
    expected = 16000 * 4096**2 / 2  # 1 s of ffmpeg's sine (amplitude 1/8 of full scale)
    assert 0.7 * expected < energy < 1.3 * expected, energy / expected
