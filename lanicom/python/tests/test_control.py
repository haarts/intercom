import random

import pbref
import pytest

from lanicom import control

pb = pbref.load()


def to_pb(msg):
    if isinstance(msg, control.Hello):
        return pb.Control(hello=pb.Hello(name=msg.name, zones=msg.zones, caps=msg.caps, challenge=msg.challenge, echo=msg.echo, bye=msg.bye))
    if isinstance(msg, control.TalkStart):
        t = msg.target
        target = pb.Target(device=t.device) if t.device is not None else pb.Target(zone=t.zone) if t.zone is not None else pb.Target(all=t.all)
        return pb.Control(talk_start=pb.TalkStart(target=target, stream_id=msg.stream_id))
    return pb.Control(talk_stop=pb.TalkStop(stream_id=msg.stream_id))


def random_msg(rng):
    kind = rng.randrange(3)
    word = lambda: "".join(rng.choice("abcdeé☀ ") for _ in range(rng.randrange(0, 12)))
    if kind == 0:
        return control.Hello(
            name=word(), zones=[word() for _ in range(rng.randrange(4))], caps=rng.choice([0, 1, 2, 3, 2**32 - 1]),
            challenge=rng.choice([0, rng.getrandbits(64)]), echo=rng.choice([0, rng.getrandbits(64)]), bye=rng.random() < 0.2,
        )
    if kind == 1:
        target = rng.choice([control.Target(device=rng.getrandbits(32)), control.Target(zone=word()), control.Target(all=True)])
        return control.TalkStart(target=target, stream_id=rng.getrandbits(32))
    return control.TalkStop(stream_id=rng.getrandbits(32))


def test_matches_official_protobuf():
    rng = random.Random(1)
    for _ in range(2000):
        msg = random_msg(rng)
        ours = control.encode(msg)
        assert ours == to_pb(msg).SerializeToString()
        assert control.decode(ours) == msg


@pytest.mark.parametrize("data", [b"\x0a", b"\x0a\x05\x0a", b"\xff" * 11, b"\x0b", b"\x00\x00", b"\x0a\x02\x0a\xff", b"\x08\x01"])
def test_malformed(data):
    try:
        result = control.decode(data)
    except control.ControlError:
        return
    assert result is None or isinstance(result, (control.Hello, control.TalkStart, control.TalkStop))


def test_fuzz_never_crashes():
    rng = random.Random(2)
    for _ in range(5000):
        data = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 40)))
        try:
            control.decode(data)
        except control.ControlError:
            pass
