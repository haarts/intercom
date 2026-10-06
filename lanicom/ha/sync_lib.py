"""Copy the protocol library into the integration (HA installs custom components as plain files).

    python lanicom/ha/sync_lib.py          # copy
    python lanicom/ha/sync_lib.py --check  # fail if the copy is stale (CI / tests)
"""

import filecmp
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "python" / "src" / "lanicom"
DST = Path(__file__).resolve().parent / "custom_components" / "lanicom" / "lanicom_lib"
FILES = ["__init__.py", "audio.py", "control.py", "jitter.py", "keys.py", "media.py", "node.py", "opus.py",
         "packet.py", "peers.py", "replay.py"]


def main() -> int:
    check = "--check" in sys.argv
    stale = [f for f in FILES if not (DST / f).exists() or not filecmp.cmp(SRC / f, DST / f, shallow=False)]
    extra = sorted(p.name for p in DST.glob("*.py") if p.name not in FILES) if DST.exists() else []
    if check:
        if stale or extra:
            print(f"lanicom_lib is stale: {stale + extra}; run python lanicom/ha/sync_lib.py", file=sys.stderr)
            return 1
        return 0
    DST.mkdir(exist_ok=True)
    for name in extra:
        (DST / name).unlink()
    for f in FILES:
        shutil.copy2(SRC / f, DST / f)
    print(f"copied {len(FILES)} files to {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
