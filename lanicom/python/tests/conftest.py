import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
VECTORS = ROOT.parent / "spec" / "vectors" / "lanicom-v1.json"


@pytest.fixture(scope="session")
def vectors():
    return json.loads(VECTORS.read_text())
