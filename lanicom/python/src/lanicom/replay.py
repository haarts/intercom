"""64-packet sliding replay window (PROTOCOL.md section 4)."""

from __future__ import annotations

WINDOW = 64
_MASK = (1 << WINDOW) - 1


class ReplayWindow:
    def __init__(self, first_seq: int):
        self.highest = first_seq
        self.bitmap = 1  # bit i set = highest - i seen

    def check_and_update(self, seq: int) -> bool:
        """True if `seq` is new (and records it); False for duplicates and too-old packets."""
        if seq > self.highest:
            shift = seq - self.highest
            self.bitmap = ((self.bitmap << shift) | 1) & _MASK if shift < WINDOW else 1
            self.highest = seq
            return True
        offset = self.highest - seq
        if offset >= WINDOW or self.bitmap >> offset & 1:
            return False
        self.bitmap |= 1 << offset
        return True
