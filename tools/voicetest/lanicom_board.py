"""Control a lanicom board over its encrypted ESPHome API: lanicom_board.py talk <on|off> [HOST]

The API key comes from lanicom/esphome/secrets.yaml.
"""
import asyncio
import sys
from pathlib import Path

import yaml
from aioesphomeapi import APIClient, SwitchInfo

SECRETS = Path(__file__).resolve().parents[2] / "lanicom" / "esphome" / "secrets.yaml"


async def main(what, value, host="lanicom-p4-e80332.local"):
    key = yaml.safe_load(SECRETS.read_text())["api_encryption_key"]
    c = APIClient(host, 6053, None, noise_psk=key)
    await c.connect(login=True)
    ents, _ = await c.list_entities_services()
    if what == "talk":
        e = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "talk")
        c.switch_command(e.key, value == "on")
    await asyncio.sleep(0.3)
    await c.disconnect()


asyncio.run(main(*sys.argv[1:4]))
