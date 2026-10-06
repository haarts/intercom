"""Make a low-latency copy of ESPHome's i2s_audio component.

Stock ESPHome (2026.7) keeps 5 x 10 ms of DMA queued on the speaker (every write
waits ~50 ms) and reads the mic in 16 ms blocks. For an intercom that is most of
the latency budget. This copies i2s_audio from the installed ESPHome and shrinks
those buffers. Point an external_components source at the output to override the
built-in component (see lanicom-p4.yaml).

    python tools/make_lowlatency_i2s.py            # -> lowlatency/components/i2s_audio

It refuses to patch if the ESPHome source doesn't look as expected: re-run it
after every ESPHome upgrade and check the result on hardware.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

PATCHES = {
    "speaker/i2s_audio_speaker_standard.cpp": [
        ("static constexpr uint32_t DMA_BUFFER_DURATION_MS = 10;", "static constexpr uint32_t DMA_BUFFER_DURATION_MS = 5;  // lanicom: was 10"),
        ("static constexpr size_t DMA_BUFFERS_COUNT = 5;", "static constexpr size_t DMA_BUFFERS_COUNT = 3;  // lanicom: was 5"),
    ],
    "microphone/i2s_audio_microphone.cpp": [
        ("static const uint32_t READ_DURATION_MS = 16;", "static const uint32_t READ_DURATION_MS = 5;  // lanicom: was 16"),
        (".dma_frame_num = 256,", ".dma_frame_num = 80,  // lanicom: 5 ms at 16 kHz, was 256"),
    ],
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1] / "lowlatency" / "components")
    args = parser.parse_args()
    try:
        import esphome
        from esphome.const import __version__
    except ImportError:
        print("ESPHome isn't installed in this Python", file=sys.stderr)
        return 1
    src = Path(esphome.__file__).parent / "components" / "i2s_audio"
    dst = args.out / "i2s_audio"
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("__pycache__"))
    for rel, edits in PATCHES.items():
        path = dst / rel
        text = path.read_text()
        for old, new in edits:
            if text.count(old) != 1:
                shutil.rmtree(dst)
                print(f"{rel}: expected exactly one '{old}' (ESPHome {__version__} changed?); nothing written", file=sys.stderr)
                return 1
            text = text.replace(old, new)
        path.write_text(text)
    (dst / "LANICOM_PATCHED").write_text(f"Generated from ESPHome {__version__} by tools/make_lowlatency_i2s.py\n")
    print(f"wrote {dst} (from ESPHome {__version__})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
