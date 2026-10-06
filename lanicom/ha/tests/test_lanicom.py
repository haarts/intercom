"""Home Assistant joins a lanicom network as a peer (real UDP on 127.0.0.x)."""

import asyncio
import shutil
import subprocess
import time

import pytest
from homeassistant import config_entries
from homeassistant.const import CONF_NAME
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.lanicom.const import CONF_KEY, CONF_SENDER_ID, CONF_ZONES, DOMAIN
from custom_components.lanicom.lanicom_lib import CAP_PLAYBACK, NetworkKey, Node, NodeConfig, Target, opus

KEY = "ha-test-key-0123"


async def test_config_flow(hass: HomeAssistant) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.FORM
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_KEY: "short", CONF_NAME: "HA"})
    assert result["errors"] == {CONF_KEY: "key_too_short"}
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_KEY: f"  {KEY} ", CONF_NAME: "HA", CONF_ZONES: "Kitchen, downstairs"}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    key_id = (await hass.async_add_executor_job(NetworkKey, KEY)).key_id
    assert result["result"].unique_id == f"{key_id:04x}"
    assert result["data"][CONF_KEY] == KEY
    assert result["data"][CONF_ZONES] == "kitchen,downstairs"
    assert 0 < result["data"][CONF_SENDER_ID] <= 0xFFFFFFFF

    # Same network again: aborted.
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_KEY: KEY, CONF_NAME: "HA2"})
    assert result["type"] is FlowResultType.ABORT


async def wait_for(cond, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return
        await asyncio.sleep(0.05)
    raise AssertionError("timed out")


@pytest.fixture
async def peer_factory(socket_enabled):
    nodes = []

    async def make(name="Kitchen", zones=("kitchen",)):
        node = Node(
            NetworkKey(KEY),
            NodeConfig(name=name, zones=list(zones), caps=CAP_PLAYBACK, bind="127.0.0.2", broadcast=None,
                       static_peers=["127.0.0.1"]),
        )
        await node.start()
        nodes.append(node)
        return node

    yield make
    for node in nodes:
        await node.stop()


async def test_peer_entities_events_and_announce(hass: HomeAssistant, peer_factory, tmp_path) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN, unique_id="test",
        data={CONF_KEY: KEY, CONF_NAME: "Home Assistant", CONF_ZONES: "", CONF_SENDER_ID: 0x0A0B0C0D},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    hub = entry.runtime_data

    peer = await peer_factory()
    events = []
    hass.bus.async_listen("lanicom_talk_start", lambda e: events.append(("start", e.data)))
    hass.bus.async_listen("lanicom_talk_stop", lambda e: events.append(("stop", e.data)))
    try:
        await wait_for(lambda: peer.sender_id in hub.peers)
        await hass.async_block_till_done()
        state = hass.states.get("binary_sensor.kitchen_talking")
        assert state is not None and state.state == "off"
        assert hass.states.get("sensor.kitchen_round_trip") is not None

        # The peer talks to a zone HA isn't in: HA (a monitor) still sees it, without audio.
        talk = peer.start_talk(Target(zone="upstairs"))
        talk.send_frame(b"\x78\x01", 160)
        await wait_for(lambda: events)
        await hass.async_block_till_done()
        assert events[0][0] == "start" and events[0][1]["name"] == "Kitchen" and events[0][1]["target"] == "zone:upstairs"
        assert hass.states.get("binary_sensor.kitchen_talking").state == "on"
        talk.stop()
        await wait_for(lambda: len(events) == 2)
        await hass.async_block_till_done()
        assert hass.states.get("binary_sensor.kitchen_talking").state == "off"
        assert hub.node.stats["rx_audio"] == 0

        # Announce a file to the peer by name.
        if not (shutil.which("ffmpeg") and opus.available()):
            pytest.skip("needs ffmpeg and libopus")
        wav = tmp_path / "beep.wav"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=800:duration=0.3", str(wav)], check=True)
        received = []
        peer.on_audio = lambda p, sid, ts, pkt: received.append(p.name)
        await hass.services.async_call(DOMAIN, "announce", {"target": "kitchen", "media": f"file://{wav}"}, blocking=True)
        await wait_for(lambda: len(received) >= 25)
        assert set(received) == {"Home Assistant"}
    finally:
        assert await hass.config_entries.async_unload(entry.entry_id)
