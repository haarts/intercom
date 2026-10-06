"""Constants for the lanicom integration."""

DOMAIN = "lanicom"

CONF_KEY = "key"
CONF_SENDER_ID = "sender_id"

DEFAULT_NAME = "Home Assistant"

EVENT_TALK_START = "lanicom_talk_start"
EVENT_TALK_STOP = "lanicom_talk_stop"

SERVICE_ANNOUNCE = "announce"
ATTR_TARGET = "target"
ATTR_MEDIA = "media"
ATTR_BITRATE = "bitrate"

SIGNAL_PEER_ADDED = "lanicom_peer_added_{}"
SIGNAL_UPDATE = "lanicom_update_{}"

# A peer that never sends TalkStop (all three copies lost) stops "talking" after this.
TALK_TIMEOUT_S = 120
