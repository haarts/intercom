"""Control the board over the ESPHome API:
board.py mode <always_on|push_to_talk|communicator> | talk <on|off> | gain <preamp_dB> <digital_dB> | opus <bitrate> <complexity>"""
import asyncio, sys
from aioesphomeapi import APIClient, NumberInfo, SelectInfo, SwitchInfo

async def main(what, value, value2=None):
    c = APIClient("192.168.188.67", 6053, None)
    await c.connect(login=True)
    ents, _ = await c.list_entities_services()
    if what == "mode":
        e = next(e for e in ents if isinstance(e, SelectInfo) and e.object_id.endswith("mode"))
        c.select_command(e.key, value)
    elif what == "talk":
        e = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "talk")
        c.switch_command(e.key, value == "on")
    elif what == "gain":
        nums = {e.object_id: e.key for e in ents if isinstance(e, NumberInfo)}
        c.number_command(nums["mic_preamp"], float(value))
        c.number_command(nums["mic_digital_gain"], float(value2))
    elif what == "opus":
        nums = {e.object_id: e.key for e in ents if isinstance(e, NumberInfo)}
        c.number_command(nums["opus_bitrate"], float(value))
        c.number_command(nums["opus_complexity"], float(value2))
    await asyncio.sleep(0.5)
    await c.disconnect()

asyncio.run(main(*sys.argv[1:4]))
