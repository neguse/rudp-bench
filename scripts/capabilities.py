"""Static adapter capability metadata used by result reduction/reporting."""

from __future__ import annotations

from typing import Dict, Optional


MIN_PAYLOAD_BYTES = 17

# Values are application payload bytes, not wire bytes. max_conns=None means the
# harness has no adapter-level cap beyond OS/library resource limits.
CAPABILITIES: Dict[str, Dict[str, object]] = {
    "raw_udp": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "u": {"max_payload": 65507, "flush_policy": "immediate", "transport": "udp_datagram"},
        },
    },
    "mini_rudp": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {
                "max_payload": 65497,
                "flush_policy": "immediate_retransmit_poll",
                "transport": "udp_datagram_ack",
            },
            "u": {"max_payload": 65497, "flush_policy": "immediate", "transport": "udp_datagram"},
        },
    },
    "coop_rudp": {
        "encryption": "off",
        "max_conns": 4096,
        "modes": {
            "r": {
                "max_payload": 1148,
                "flush_policy": "coop_flush_sack_retransmit",
                "transport": "udp_datagram_sack",
            },
            "u": {
                "max_payload": 1148,
                "flush_policy": "coop_flush_unreliable",
                "transport": "udp_datagram",
            },
        },
    },
    "apex_rudp": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {
                "max_payload": 65486,
                "flush_policy": "piggyback_sack_retransmit",
                "transport": "udp_datagram_sack",
            },
            "u": {
                "max_payload": 65486,
                "flush_policy": "server_async_unreliable_piggyback_ack",
                "transport": "udp_datagram",
            },
        },
    },
    "enet": {
        "encryption": "off",
        "max_conns": 4095,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "poll_flush", "transport": "enet_packet"},
            "u": {"max_payload": 65536, "flush_policy": "poll_flush", "transport": "enet_packet"},
        },
    },
    "kcp": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "poll_update", "transport": "kcp_arq"},
            "u": {"max_payload": 65502, "flush_policy": "immediate", "transport": "udp_datagram"},
        },
    },
    "slikenet": {
        "encryption": "off",
        "max_conns": 4096,
        "modes": {
            "r": {
                "max_payload": 65536,
                "flush_policy": "library_internal",
                "transport": "slikenet_message",
            },
            "u": {
                "max_payload": 65536,
                "flush_policy": "library_internal",
                "transport": "slikenet_message",
            },
        },
    },
    "raknet": {
        "encryption": "off",
        "max_conns": 4096,
        "modes": {
            "r": {
                "max_payload": 65536,
                "flush_policy": "library_internal",
                "transport": "raknet_message",
            },
            "u": {
                "max_payload": 65536,
                "flush_policy": "library_internal",
                "transport": "raknet_message",
            },
        },
    },
    "udt4": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "blocking_stream", "transport": "stream"},
        },
    },
    "yojimbo": {
        "encryption": "on",
        "max_conns": 64,
        "modes": {
            "r": {
                "max_payload": 4096,
                "flush_policy": "poll_send_packets",
                "transport": "yojimbo_message",
            },
            "u": {
                "max_payload": 4096,
                "flush_policy": "poll_send_packets",
                "transport": "yojimbo_message",
            },
        },
    },
    "gns": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "nagle", "transport": "gns_message"},
            "u": {"max_payload": 65536, "flush_policy": "nagle", "transport": "gns_message"},
        },
    },
    "gns_encrypted": {
        "encryption": "on",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "nagle", "transport": "gns_message"},
            "u": {"max_payload": 65536, "flush_policy": "nagle", "transport": "gns_message"},
        },
    },
    "gns_no_nagle": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "no_nagle", "transport": "gns_message"},
            "u": {"max_payload": 65536, "flush_policy": "no_nagle", "transport": "gns_message"},
        },
    },
    "gns_smallbuf": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "nagle", "transport": "gns_message"},
            "u": {"max_payload": 65536, "flush_policy": "nagle", "transport": "gns_message"},
        },
    },
    "gns_split_lanes": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "nagle_split_lanes", "transport": "gns_message"},
            "u": {"max_payload": 65536, "flush_policy": "nagle_split_lanes", "transport": "gns_message"},
        },
    },
    "msquic": {
        "encryption": "on",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "async_internal", "transport": "quic_stream"},
            "u": {"max_payload": 1000, "flush_policy": "async_internal", "transport": "quic_datagram"},
        },
    },
    "quiche": {
        "encryption": "on",
        "max_conns": None,
        "modes": {
            "r": {"max_payload": 65536, "flush_policy": "poll_send", "transport": "quic_stream"},
            "u": {"max_payload": 1200, "flush_policy": "poll_send", "transport": "quic_datagram"},
        },
    },
    "lsquic": {
        "encryption": "on",
        "max_conns": None,
        "modes": {
            "r": {
                "max_payload": 65536,
                "flush_policy": "poll_process_conns",
                "transport": "quic_stream",
            },
            "u": {
                "max_payload": 1200,
                "flush_policy": "poll_process_conns",
                "transport": "quic_datagram",
            },
        },
    },
    "litenetlib": {
        "encryption": "off",
        "max_conns": None,
        "modes": {
            "r": {
                "max_payload": 1000,
                "flush_policy": "library_internal",
                "transport": "litenetlib_message",
            },
            "u": {
                "max_payload": 1000,
                "flush_policy": "library_internal",
                "transport": "litenetlib_message",
            },
        },
    },
}


def mode_capability(library: str, reliable: str) -> Optional[Dict[str, object]]:
    cap = CAPABILITIES.get(library)
    if cap is None:
        return None
    modes = cap.get("modes", {})
    if not isinstance(modes, dict):
        return None
    mode = modes.get(reliable)
    return mode if isinstance(mode, dict) else None


def known_library(library: str) -> bool:
    return library in CAPABILITIES


def supports_reliability(library: str, reliable: str) -> bool:
    if not known_library(library):
        return True
    return mode_capability(library, reliable) is not None


def max_payload_bytes(library: str, reliable: str) -> Optional[int]:
    mode = mode_capability(library, reliable)
    if mode is None:
        return None
    value = mode.get("max_payload")
    return int(value) if value is not None else None


def max_connections(library: str) -> Optional[int]:
    value = CAPABILITIES.get(library, {}).get("max_conns")
    return int(value) if value is not None else None


def flush_policy(library: str, reliable: str) -> str:
    mode = mode_capability(library, reliable)
    if mode is None:
        return "unsupported" if known_library(library) else "unknown"
    return str(mode.get("flush_policy", "unknown"))


def transport_mode(library: str, reliable: str) -> str:
    mode = mode_capability(library, reliable)
    if mode is None:
        return "unsupported" if known_library(library) else "unknown"
    return str(mode.get("transport", "unknown"))


def scenario_metadata(library: str, reliable: str) -> Dict[str, str]:
    max_payload = max_payload_bytes(library, reliable)
    max_conns = max_connections(library)
    return {
        "supports_reliability": "1" if supports_reliability(library, reliable) else "0",
        "min_payload_bytes": str(MIN_PAYLOAD_BYTES),
        "max_payload_bytes": "" if max_payload is None else str(max_payload),
        "max_connections": "unbounded" if max_conns is None else str(max_conns),
        "transport_mode": transport_mode(library, reliable),
    }
