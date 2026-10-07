"""lanicom: serverless LAN push-to-talk intercom (see lanicom/spec/PROTOCOL.md).

Needs ESP-IDF. The protocol lives in lanicom/lanicom-core (portable C, host-tested);
this component adds the socket, Opus and the microphone/speaker.
"""

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, microphone, output, socket, speaker, text, time
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
CONF_MAX_STREAMS = "max_streams"
CONF_AUDIO_STACK = "audio_stack"
CONF_OPUS_PATH = "opus_path"
CONF_TARGET = "target"
CONF_BUTTONS = "buttons"
CONF_BUTTON = "button"
CONF_RING = "ring"
CONF_RING_BRIGHTNESS = "ring_brightness"
CONF_NIGHT_BRIGHTNESS = "night_brightness"
CONF_NIGHT_START = "night_start"
CONF_NIGHT_END = "night_end"
CONF_TIME_ID = "time_id"
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


def _target(value):
    value = cv.string(value)
    if value == "all":
        return value
    if value.startswith("device:"):
        try:
            if 0 < int(value[7:], 16) <= 0xFFFFFFFF:
                return value
        except ValueError:
            pass
    raise cv.Invalid("target is 'all' or 'device:<8 hex digits>'")


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
            # Streams played at once, one Opus decoder (~20 KB) each. Lower it without PSRAM.
            cv.Optional(CONF_MAX_STREAMS, default=4): cv.int_range(min=1, max=4),
            # Stack of the audio task (Opus runs there).
            cv.Optional(CONF_AUDIO_STACK, default=16384): cv.int_range(min=8192, max=65536),
            # Use a local micro-opus copy (e.g. esp32-mumble/lib/micro-opus) instead of the registry's.
            cv.Optional(CONF_OPUS_PATH): cv.directory,
            # Wall-box buttons, in order (1-4). Each one is paired with one button on another box
            # and talks to it; see docs/buttons.md. With buttons, only partners and announcers
            # (Home Assistant) are played.
            cv.Optional(CONF_BUTTONS): cv.All(
                cv.ensure_list(
                    cv.Schema(
                        {
                            cv.Required(CONF_BUTTON): cv.use_id(binary_sensor.BinarySensor),
                            cv.Optional(CONF_RING): cv.use_id(output.FloatOutput),
                        }
                    )
                ),
                cv.Length(min=1, max=4),
            ),
            cv.Optional(CONF_RING_BRIGHTNESS, default="10%"): cv.percentage,
            # Dimmer rings at night, from night_start to night_end (hours, local time). Needs a
            # clock (time_id); without one it's always day.
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_NIGHT_BRIGHTNESS, default="2%"): cv.percentage,
            cv.Optional(CONF_NIGHT_START, default=22): cv.int_range(min=0, max=23),
            cv.Optional(CONF_NIGHT_END, default=7): cv.int_range(min=0, max=23),
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
    for conf_key, setter in (
        (CONF_KEY_TEXT_ID, "set_key_text"),
        (CONF_NAME_TEXT_ID, "set_name_text"),
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
    cg.add(var.set_max_streams(config[CONF_MAX_STREAMS]))
    cg.add(var.set_audio_stack(config[CONF_AUDIO_STACK]))
    for button in config.get(CONF_BUTTONS, []):
        sensor = await cg.get_variable(button[CONF_BUTTON])
        ring = await cg.get_variable(button[CONF_RING]) if CONF_RING in button else cg.nullptr
        cg.add(var.add_button(sensor, ring))
    cg.add(var.set_ring_brightness(config[CONF_RING_BRIGHTNESS]))
    cg.add(var.set_night_brightness(config[CONF_NIGHT_BRIGHTNESS]))
    cg.add(var.set_night_hours(config[CONF_NIGHT_START], config[CONF_NIGHT_END]))
    if CONF_TIME_ID in config:
        cg.add(var.set_clock(await cg.get_variable(config[CONF_TIME_ID])))

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
