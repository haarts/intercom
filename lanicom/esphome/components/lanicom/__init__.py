"""lanicom: serverless LAN push-to-talk intercom (see lanicom/spec/PROTOCOL.md).

Needs ESP-IDF. The protocol lives in lanicom/lanicom-core (portable C, host-tested);
this component adds the socket, Opus and the microphone/speaker.
"""

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import microphone, socket, speaker, text
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_PORT, CONF_SPEAKER

CODEOWNERS = ["@haarts"]
DEPENDENCIES = ["network", "esp32"]
AUTO_LOAD = ["audio"]

CORE_PATH = Path(__file__).resolve().parents[3] / "lanicom-core"

CONF_KEY = "key"
CONF_KEY_TEXT_ID = "key_text_id"
CONF_DEVICE_NAME = "device_name"
CONF_NAME_TEXT_ID = "name_text_id"
CONF_ZONES = "zones"
CONF_ZONES_TEXT_ID = "zones_text_id"
CONF_MULTICAST = "multicast"
CONF_STATIC_PEERS = "static_peers"
CONF_JITTER_BUFFER = "jitter_buffer"
CONF_JITTER_BUFFER_MAX = "jitter_buffer_max"
CONF_FRAME_DURATION = "frame_duration"
CONF_BITRATE = "bitrate"
CONF_COMPLEXITY = "complexity"
CONF_MIC_WARMUP = "mic_warmup"
CONF_SPEAKER_HOLD = "speaker_hold"
CONF_TASK_CORE = "task_core"
CONF_OPUS_PATH = "opus_path"
CONF_TARGET = "target"
CONF_ON_RECEIVE_START = "on_receive_start"
CONF_ON_RECEIVE_END = "on_receive_end"
CONF_ON_PEER_ADDED = "on_peer_added"
CONF_ON_PEER_REMOVED = "on_peer_removed"

lanicom_ns = cg.esphome_ns.namespace("lanicom")
LanicomComponent = lanicom_ns.class_("LanicomComponent", cg.Component)
StartTalkingAction = lanicom_ns.class_("StartTalkingAction", automation.Action)
StopTalkingAction = lanicom_ns.class_("StopTalkingAction", automation.Action)
IsReceivingCondition = lanicom_ns.class_("IsReceivingCondition", automation.Condition)
IsTalkingCondition = lanicom_ns.class_("IsTalkingCondition", automation.Condition)


def _zones(value):
    value = cv.string(value)
    zones = [z.strip().lower() for z in value.split(",") if z.strip()]
    if len(zones) > 8 or any(len(z) > 16 for z in zones):
        raise cv.Invalid("at most 8 zones of at most 16 characters")
    return ",".join(zones)


def _target(value):
    value = cv.string(value)
    if value == "all" or (value.startswith("zone:") and len(value) > 5):
        return value
    if value.startswith("device:"):
        try:
            if 0 < int(value[7:], 16) <= 0xFFFFFFFF:
                return value
        except ValueError:
            pass
    raise cv.Invalid("target is 'all', 'zone:<name>' or 'device:<8 hex digits>'")


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LanicomComponent),
            cv.Optional(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
            cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            # The network key string (`lanicom keygen`). Prefer key_text_id so it can be set
            # from the device's web page; a non-empty text value overrides this.
            cv.Optional(CONF_KEY, default=""): cv.string,
            cv.Optional(CONF_KEY_TEXT_ID): cv.use_id(text.Text),
            cv.Optional(CONF_DEVICE_NAME, default=""): cv.string,
            cv.Optional(CONF_NAME_TEXT_ID): cv.use_id(text.Text),
            cv.Optional(CONF_ZONES, default=""): _zones,
            cv.Optional(CONF_ZONES_TEXT_ID): cv.use_id(text.Text),
            cv.Optional(CONF_PORT, default=47100): cv.port,
            cv.Optional(CONF_MULTICAST, default=False): cv.boolean,
            cv.Optional(CONF_STATIC_PEERS, default=[]): cv.ensure_list(cv.ipv4address),
            cv.Optional(CONF_JITTER_BUFFER, default="20ms"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(milliseconds=10))
            ),
            cv.Optional(CONF_JITTER_BUFFER_MAX, default="60ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FRAME_DURATION, default="10ms"): cv.one_of("10ms", "20ms"),
            cv.Optional(CONF_BITRATE, default=24000): cv.int_range(min=6000, max=64000),
            cv.Optional(CONF_COMPLEXITY, default=3): cv.int_range(min=0, max=10),
            cv.Optional(CONF_MIC_WARMUP, default="50ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SPEAKER_HOLD, default="3s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TASK_CORE, default=1): cv.int_range(min=0, max=1),
            # Use a local micro-opus copy (e.g. esp32-mumble/lib/micro-opus) instead of the registry's.
            cv.Optional(CONF_OPUS_PATH): cv.directory,
            cv.Optional(CONF_ON_RECEIVE_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_RECEIVE_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PEER_ADDED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PEER_REMOVED): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.has_at_least_one_key(CONF_MICROPHONE, CONF_SPEAKER),
    socket.consume_sockets(1, "lanicom", socket.SocketType.UDP),
)


def _final_validate(config):
    if config[CONF_JITTER_BUFFER_MAX] < config[CONF_JITTER_BUFFER]:
        raise cv.Invalid("jitter_buffer_max must be at least jitter_buffer")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    add_idf_component(name="lanicom-core", path=str(CORE_PATH))
    if CONF_OPUS_PATH in config:
        add_idf_component(name="micro-opus", path=str(Path(config[CONF_OPUS_PATH]).resolve()))
    else:
        add_idf_component(name="esphome/micro-opus", ref="0.4.1")
    for option in ("CONFIG_MBEDTLS_CHACHA20_C", "CONFIG_MBEDTLS_POLY1305_C", "CONFIG_MBEDTLS_CHACHAPOLY_C"):
        add_idf_sdkconfig_option(option, True)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_MICROPHONE in config:
        cg.add(var.set_microphone(await cg.get_variable(config[CONF_MICROPHONE])))
    if CONF_SPEAKER in config:
        cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
    cg.add(var.set_key(config[CONF_KEY]))
    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_zones(config[CONF_ZONES]))
    for conf_key, setter in (
        (CONF_KEY_TEXT_ID, "set_key_text"),
        (CONF_NAME_TEXT_ID, "set_name_text"),
        (CONF_ZONES_TEXT_ID, "set_zones_text"),
    ):
        if conf_key in config:
            cg.add(getattr(var, setter)(await cg.get_variable(config[conf_key])))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_multicast(config[CONF_MULTICAST]))
    for peer in config[CONF_STATIC_PEERS]:
        cg.add(var.add_static_peer(str(peer)))
    cg.add(
        var.set_jitter(
            config[CONF_JITTER_BUFFER].total_milliseconds,
            config[CONF_JITTER_BUFFER_MAX].total_milliseconds,
        )
    )
    cg.add(var.set_frame_ms(int(config[CONF_FRAME_DURATION][:-2])))
    cg.add(var.set_bitrate(config[CONF_BITRATE]))
    cg.add(var.set_complexity(config[CONF_COMPLEXITY]))
    cg.add(var.set_mic_warmup_ms(config[CONF_MIC_WARMUP].total_milliseconds))
    cg.add(var.set_speaker_hold_ms(config[CONF_SPEAKER_HOLD].total_milliseconds))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))

    for conf_key, getter, args in (
        (CONF_ON_RECEIVE_START, "get_receive_start_trigger", [(cg.std_string, "name")]),
        (CONF_ON_RECEIVE_END, "get_receive_end_trigger", []),
        (CONF_ON_PEER_ADDED, "get_peer_added_trigger", [(cg.std_string, "name")]),
        (CONF_ON_PEER_REMOVED, "get_peer_removed_trigger", [(cg.std_string, "name")]),
    ):
        if conf_key in config:
            await automation.build_automation(getattr(var, getter)(), args, config[conf_key])


LANICOM_ID_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(LanicomComponent)})


@automation.register_action(
    "lanicom.start_talking",
    StartTalkingAction,
    LANICOM_ID_SCHEMA.extend({cv.Optional(CONF_TARGET, default="all"): cv.templatable(_target)}),
    synchronous=True,
)
async def start_talking_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_target(await cg.templatable(config[CONF_TARGET], args, cg.std_string)))
    return var


@automation.register_action("lanicom.stop_talking", StopTalkingAction, LANICOM_ID_SCHEMA, synchronous=True)
async def stop_talking_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("lanicom.is_receiving", IsReceivingCondition, LANICOM_ID_SCHEMA)
async def is_receiving_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("lanicom.is_talking", IsTalkingCondition, LANICOM_ID_SCHEMA)
async def is_talking_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
