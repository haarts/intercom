import pytest

from lanicom import control
from lanicom.keys import NetworkKey
from lanicom.packet import Header, PacketError, open_packet, seal
from lanicom.replay import ReplayWindow

_keys = {}


def key(ks):
    if ks not in _keys:
        _keys[ks] = NetworkKey(ks)
    return _keys[ks]


def test_key_derivation(vectors):
    for v in vectors["keys"]:
        k = key(v["key_string"])
        assert k.master.hex() == v["master"]
        assert k.key_id == v["key_id"]
        for sid, expected in v["send_keys"].items():
            assert k.send_key(int(sid, 16)).hex() == expected


def test_seal_matches_vectors(vectors):
    for v in vectors["packets"]:
        header = Header(v["type"], key(v["key_string"]).key_id, v["sender_id"], v["epoch"], v["seq"])
        assert seal(key(v["key_string"]), header, bytes.fromhex(v["plaintext"])).hex() == v["packet"]


def test_open_vectors(vectors):
    for v in vectors["packets"]:
        header, plain = open_packet(key(v["key_string"]), bytes.fromhex(v["packet"]))
        assert (header.type, header.sender_id, header.epoch, header.seq) == (v["type"], v["sender_id"], v["epoch"], v["seq"])
        assert plain.hex() == v["plaintext"]


def test_invalid_packets(vectors):
    for v in vectors["invalid_packets"]:
        with pytest.raises(PacketError) as err:
            open_packet(key(v["key_string"]), bytes.fromhex(v["packet"]))
        assert err.value.reason == v["reason"], v["name"]


def _from_fields(fields):
    if "hello" in fields:
        return control.Hello(**fields["hello"])
    if "talk_start" in fields:
        f = fields["talk_start"]
        return control.TalkStart(target=control.Target(**f["target"]), stream_id=f.get("stream_id", 0))
    return control.TalkStop(**fields["talk_stop"])


def test_control_vectors(vectors):
    for v in vectors["control"]:
        expected = _from_fields(v["message"])
        assert control.decode(bytes.fromhex(v["bytes"])) == expected, v["name"]
        if not v.get("decode_only"):
            assert control.encode(expected).hex() == v["bytes"], v["name"]


def test_replay_vectors(vectors):
    for v in vectors["replay"]:
        w = ReplayWindow(v["first"])
        for seq, ok in v["steps"]:
            assert w.check_and_update(seq) == ok, (v["first"], seq)
