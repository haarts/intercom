"""Load spec/lanicom.proto with the official protobuf library (reference codec for tests)."""

from __future__ import annotations

import importlib
import sys
import tempfile
from pathlib import Path

SPEC = Path(__file__).resolve().parents[2] / "spec"


def load():
    from grpc_tools import protoc

    out = Path(tempfile.mkdtemp(prefix="lanicom-pb-"))
    rc = protoc.main(["protoc", f"-I{SPEC}", f"--python_out={out}", str(SPEC / "lanicom.proto")])
    if rc:
        raise RuntimeError("protoc failed")
    sys.path.insert(0, str(out))
    try:
        return importlib.import_module("lanicom_pb2")
    finally:
        sys.path.remove(str(out))
