package bench

import "strconv"

// MinPayloadBytes is the minimum application payload the harness embeds
// (sequence number + timestamp header).
const MinPayloadBytes = 17

// ModeCapability describes adapter capabilities for a single channel mode.
type ModeCapability struct {
	MaxPayload  int
	FlushPolicy string
	Transport   string
}

// LibCapability describes the overall capabilities of a library adapter.
type LibCapability struct {
	Encryption string
	MaxConns   int                    // -1 = unbounded
	Modes      map[string]ModeCapability // "r", "u"
}

// Capabilities maps library names to their adapter capabilities.
// All 21 entries match scripts/capabilities.py.
var Capabilities map[string]LibCapability

func init() {
	Capabilities = map[string]LibCapability{
		"raw_udp": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"u": {MaxPayload: 65507, FlushPolicy: "immediate", Transport: "udp_datagram"},
			},
		},
		"mini_rudp": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65497, FlushPolicy: "immediate_retransmit_poll", Transport: "udp_datagram_ack"},
				"u": {MaxPayload: 65497, FlushPolicy: "immediate", Transport: "udp_datagram"},
			},
		},
		"coop_rudp": {
			Encryption: "off",
			MaxConns:   4096,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 1148, FlushPolicy: "coop_flush_sack_retransmit", Transport: "udp_datagram_sack"},
				"u": {MaxPayload: 1148, FlushPolicy: "coop_flush_unreliable", Transport: "udp_datagram"},
			},
		},
		"apex_rudp": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65486, FlushPolicy: "piggyback_sack_retransmit", Transport: "udp_datagram_sack"},
				"u": {MaxPayload: 65486, FlushPolicy: "server_async_unreliable_piggyback_ack", Transport: "udp_datagram"},
			},
		},
		"enet": {
			Encryption: "off",
			MaxConns:   4095,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "poll_flush", Transport: "enet_packet"},
				"u": {MaxPayload: 65536, FlushPolicy: "poll_flush", Transport: "enet_packet"},
			},
		},
		"kcp": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "poll_update", Transport: "kcp_arq"},
				"u": {MaxPayload: 65502, FlushPolicy: "immediate", Transport: "udp_datagram"},
			},
		},
		"slikenet": {
			Encryption: "off",
			MaxConns:   4096,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "library_internal", Transport: "slikenet_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "library_internal", Transport: "slikenet_message"},
			},
		},
		"raknet": {
			Encryption: "off",
			MaxConns:   4096,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "library_internal", Transport: "raknet_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "library_internal", Transport: "raknet_message"},
			},
		},
		"udt4": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "blocking_stream", Transport: "stream"},
			},
		},
		"yojimbo": {
			Encryption: "on",
			MaxConns:   64,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 4096, FlushPolicy: "poll_send_packets", Transport: "yojimbo_message"},
				"u": {MaxPayload: 4096, FlushPolicy: "poll_send_packets", Transport: "yojimbo_message"},
			},
		},
		"gns": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "nagle", Transport: "gns_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "nagle", Transport: "gns_message"},
			},
		},
		"gns_encrypted": {
			Encryption: "on",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "nagle", Transport: "gns_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "nagle", Transport: "gns_message"},
			},
		},
		"gns_no_nagle": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "no_nagle", Transport: "gns_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "no_nagle", Transport: "gns_message"},
			},
		},
		"gns_smallbuf": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "nagle", Transport: "gns_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "nagle", Transport: "gns_message"},
			},
		},
		"gns_split_lanes": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "nagle_split_lanes", Transport: "gns_message"},
				"u": {MaxPayload: 65536, FlushPolicy: "nagle_split_lanes", Transport: "gns_message"},
			},
		},
		"msquic": {
			Encryption: "on",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "async_internal", Transport: "quic_stream"},
				"u": {MaxPayload: 1000, FlushPolicy: "async_internal", Transport: "quic_datagram"},
			},
		},
		"quiche": {
			Encryption: "on",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "poll_send", Transport: "quic_stream"},
				"u": {MaxPayload: 1200, FlushPolicy: "poll_send", Transport: "quic_datagram"},
			},
		},
		"lsquic": {
			Encryption: "on",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 65536, FlushPolicy: "poll_process_conns", Transport: "quic_stream"},
				"u": {MaxPayload: 1200, FlushPolicy: "poll_process_conns", Transport: "quic_datagram"},
			},
		},
		"litenetlib": {
			Encryption: "off",
			MaxConns:   -1,
			Modes: map[string]ModeCapability{
				"r": {MaxPayload: 1000, FlushPolicy: "library_internal", Transport: "litenetlib_message"},
				"u": {MaxPayload: 1000, FlushPolicy: "library_internal", Transport: "litenetlib_message"},
			},
		},
	}
}

// modeCapability returns the ModeCapability for a library/channel pair, or nil.
func modeCapability(library, channel string) *ModeCapability {
	cap, ok := Capabilities[library]
	if !ok {
		return nil
	}
	mode, ok := cap.Modes[channel]
	if !ok {
		return nil
	}
	return &mode
}

// KnownLibrary reports whether a library name exists in the capabilities table.
func KnownLibrary(library string) bool {
	_, ok := Capabilities[library]
	return ok
}

// SupportsReliability reports whether a library supports a given channel.
// Unknown libraries return true (matching Python behavior).
func SupportsReliability(library, channel string) bool {
	if !KnownLibrary(library) {
		return true
	}
	return modeCapability(library, channel) != nil
}

// MaxPayloadBytes returns the maximum application payload for a library/channel
// pair. The boolean is false if the library or channel is not found.
func MaxPayloadBytes(library, channel string) (int, bool) {
	mode := modeCapability(library, channel)
	if mode == nil {
		return 0, false
	}
	return mode.MaxPayload, true
}

// MaxConnections returns the connection limit for a library.
// Returns (value, true) when bounded; (0, false) when unbounded or unknown.
func MaxConnections(library string) (int, bool) {
	cap, ok := Capabilities[library]
	if !ok {
		return 0, false
	}
	if cap.MaxConns < 0 {
		return 0, false
	}
	return cap.MaxConns, true
}

// FlushPolicy returns the flush policy string for a library/channel pair.
func FlushPolicy(library, channel string) string {
	mode := modeCapability(library, channel)
	if mode == nil {
		if KnownLibrary(library) {
			return "unsupported"
		}
		return "unknown"
	}
	return mode.FlushPolicy
}

// TransportMode returns the transport mode string for a library/channel pair.
func TransportMode(library, channel string) string {
	mode := modeCapability(library, channel)
	if mode == nil {
		if KnownLibrary(library) {
			return "unsupported"
		}
		return "unknown"
	}
	return mode.Transport
}

// ScenarioMetadata returns metadata key-value pairs matching the Python
// scenario_metadata function output.
func ScenarioMetadata(library, channel string) map[string]string {
	maxPayload, hasPayload := MaxPayloadBytes(library, channel)
	maxConns, hasCap := MaxConnections(library)

	supRel := "0"
	if SupportsReliability(library, channel) {
		supRel = "1"
	}

	payloadStr := ""
	if hasPayload {
		payloadStr = itoa(maxPayload)
	}

	connsStr := "unbounded"
	if hasCap {
		connsStr = itoa(maxConns)
	}

	return map[string]string{
		"supports_reliability": supRel,
		"min_payload_bytes":    itoa(MinPayloadBytes),
		"max_payload_bytes":    payloadStr,
		"max_connections":      connsStr,
		"transport_mode":       TransportMode(library, channel),
	}
}

func itoa(v int) string {
	return strconv.Itoa(v)
}
