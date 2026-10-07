import random

import pbref
import pytest

from lanicom import control

pb = pbref.load()


def to_pb(msg):
    if isinstance(msg, control.Hello):
        links = [pb.Link(button=l.button, partner=l.partner, partner_button=l.partner_button) for l in msg.links]
        return pb.Control(hello=pb.Hello(name=msg.name, caps=msg.caps, challenge=msg.challenge, echo=msg.echo, bye=msg.bye, links=links))
    if isinstance(msg, control.PairOffer):
        return pb.Control(pair_offer=pb.PairOffer(nonce=msg.nonce, button=msg.button))
    if isinstance(msg, control.PairAccept):
        return pb.Control(pair_accept=pb.PairAccept(nonce=msg.nonce, button=msg.button))
    if isinstance(msg, control.PairConfirm):
        return pb.Control(pair_confirm=pb.PairConfirm(nonce=msg.nonce))
    if isinstance(msg, control.TalkStart):
        t = msg.target
        target = pb.Target(device=t.device) if t.device is not None else pb.Target(all=t.all)
        return pb.Control(talk_start=pb.TalkStart(target=target, stream_id=msg.stream_id))
    return pb.Control(talk_stop=pb.TalkStop(stream_id=msg.stream_id))


def random_msg(rng):
    kind = rng.randrange(6)
    word = lambda: "".join(rng.choice("abcdeé☀ ") for _ in range(rng.randrange(0, 12)))
    small = lambda: rng.choice([0, 1, 4, 200, 2**32 - 1])
    if kind == 0:
        links = [control.Link(small(), rng.choice([0, rng.getrandbits(32)]), small()) for _ in range(rng.randrange(0, 5))]
        return control.Hello(
            name=word(), caps=rng.choice([0, 1, 2, 3, 8, 2**32 - 1]),
            challenge=rng.choice([0, rng.getrandbits(64)]), echo=rng.choice([0, rng.getrandbits(64)]), bye=rng.random() < 0.2,
            links=links,
        )
    if kind == 3:
        return control.PairOffer(nonce=rng.choice([0, rng.getrandbits(64)]), button=small())
    if kind == 4:
        return control.PairAccept(nonce=rng.choice([0, rng.getrandbits(64)]), button=small())
    if kind == 5:
        return control.PairConfirm(nonce=rng.choice([0, rng.getrandbits(64)]))
    if kind == 1:
        target = rng.choice([control.Target(device=rng.getrandbits(32)), control.Target(all=True)])
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
    assert result is None or isinstance(result, control.Control.__args__)


def test_fuzz_never_crashes():
    rng = random.Random(2)
    for _ in range(5000):
        data = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 40)))
        try:
            control.decode(data)
        except control.ControlError:
            pass
