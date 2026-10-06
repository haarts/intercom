"""Minimal ctypes binding to the system libopus (apt install libopus0)."""

from __future__ import annotations

import ctypes
import ctypes.util
from functools import lru_cache

SAMPLE_RATE = 16_000
FRAME_10MS = SAMPLE_RATE // 100

_APPLICATION_VOIP = 2048
_SET_BITRATE = 4002
_SET_COMPLEXITY = 4010
_SET_INBAND_FEC = 4012
_SET_PACKET_LOSS_PERC = 4014
_SET_SIGNAL = 4024
_SIGNAL_VOICE = 3001


class OpusError(RuntimeError):
    pass


@lru_cache(maxsize=1)
def _lib() -> ctypes.CDLL:
    for name in (ctypes.util.find_library("opus"), "libopus.so.0", "libopus.dylib", "opus.dll"):
        if not name:
            continue
        try:
            lib = ctypes.CDLL(name)
            break
        except OSError:
            continue
    else:
        raise OpusError("libopus not found (Debian/Ubuntu: apt install libopus0, macOS: brew install opus)")
    vp, i32, buf = ctypes.c_void_p, ctypes.c_int32, ctypes.c_char_p
    lib.opus_encoder_create.restype = vp
    lib.opus_encoder_create.argtypes = [i32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encode.restype = i32
    lib.opus_encode.argtypes = [vp, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, buf, i32]
    lib.opus_encoder_destroy.argtypes = [vp]
    lib.opus_decoder_create.restype = vp
    lib.opus_decoder_create.argtypes = [i32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_decode.restype = ctypes.c_int
    lib.opus_decode.argtypes = [vp, buf, i32, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
    lib.opus_decoder_destroy.argtypes = [vp]
    lib.opus_packet_get_nb_samples.restype = ctypes.c_int
    lib.opus_packet_get_nb_samples.argtypes = [buf, i32, i32]
    return lib


def available() -> bool:
    try:
        _lib()
        return True
    except OpusError:
        return False


def packet_samples(packet: bytes, rate: int = SAMPLE_RATE) -> int:
    n = _lib().opus_packet_get_nb_samples(packet, len(packet), rate)
    if n < 0:
        raise OpusError(f"bad packet ({n})")
    return n


class Encoder:
    def __init__(self, bitrate: int = 24_000, complexity: int = 5, fec: bool = True, frame_ms: int = 10):
        lib = _lib()
        err = ctypes.c_int()
        self._enc = lib.opus_encoder_create(SAMPLE_RATE, 1, _APPLICATION_VOIP, ctypes.byref(err))
        if err.value or not self._enc:
            raise OpusError(f"opus_encoder_create: {err.value}")
        ctl = lib.opus_encoder_ctl
        for request, value in (
            (_SET_BITRATE, bitrate),
            (_SET_COMPLEXITY, complexity),
            (_SET_INBAND_FEC, int(fec)),
            (_SET_PACKET_LOSS_PERC, 10 if fec else 0),
            (_SET_SIGNAL, _SIGNAL_VOICE),
        ):
            ctl(ctypes.c_void_p(self._enc), ctypes.c_int(request), ctypes.c_int32(value))
        self.frame_samples = SAMPLE_RATE * frame_ms // 1000
        self._out = ctypes.create_string_buffer(1000)

    def encode(self, pcm: bytes) -> bytes:
        """`pcm`: exactly one frame of 16-bit little-endian mono samples."""
        if len(pcm) != self.frame_samples * 2:
            raise ValueError(f"need {self.frame_samples} samples")
        samples = (ctypes.c_int16 * self.frame_samples).from_buffer_copy(pcm)
        n = _lib().opus_encode(self._enc, samples, self.frame_samples, self._out, len(self._out))
        if n < 0:
            raise OpusError(f"opus_encode: {n}")
        return self._out.raw[:n]

    def __del__(self):
        if getattr(self, "_enc", None):
            _lib().opus_encoder_destroy(self._enc)
            self._enc = None


class Decoder:
    MAX_SAMPLES = SAMPLE_RATE * 120 // 1000

    def __init__(self):
        err = ctypes.c_int()
        self._dec = _lib().opus_decoder_create(SAMPLE_RATE, 1, ctypes.byref(err))
        if err.value or not self._dec:
            raise OpusError(f"opus_decoder_create: {err.value}")
        self._pcm = (ctypes.c_int16 * self.MAX_SAMPLES)()

    def _run(self, packet: bytes | None, samples: int, fec: bool) -> bytes:
        n = _lib().opus_decode(self._dec, packet, len(packet) if packet else 0, self._pcm, samples, int(fec))
        if n < 0:
            raise OpusError(f"opus_decode: {n}")
        return bytes(memoryview(self._pcm).cast("B")[: n * 2])

    def decode(self, packet: bytes) -> bytes:
        return self._run(packet, self.MAX_SAMPLES, False)

    def decode_fec(self, next_packet: bytes, samples: int) -> bytes:
        """Recover the frame *before* `next_packet` from its in-band FEC."""
        return self._run(next_packet, samples, True)

    def conceal(self, samples: int) -> bytes:
        return self._run(None, samples, False)

    def __del__(self):
        if getattr(self, "_dec", None):
            _lib().opus_decoder_destroy(self._dec)
            self._dec = None
