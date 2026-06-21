package bench

import "testing"

func TestYojimboMaxConns(t *testing.T) {
	v, ok := MaxConnections("yojimbo")
	if !ok {
		t.Fatal("expected yojimbo to have bounded MaxConnections")
	}
	if v != 64 {
		t.Errorf("yojimbo MaxConnections = %d, want 64", v)
	}
}

func TestMsquicEncryption(t *testing.T) {
	cap, ok := Capabilities["msquic"]
	if !ok {
		t.Fatal("msquic not found in Capabilities")
	}
	if cap.Encryption != "on" {
		t.Errorf("msquic Encryption = %q, want %q", cap.Encryption, "on")
	}
}

func TestRawUdpNoReliable(t *testing.T) {
	if SupportsReliability("raw_udp", "r") {
		t.Error("expected raw_udp to NOT support reliable channel")
	}
}

func TestCoop_rudpReliable(t *testing.T) {
	if !SupportsReliability("coop_rudp", "r") {
		t.Error("expected coop_rudp to support reliable channel")
	}
}

func TestUnknownLibrary(t *testing.T) {
	// Unknown libraries return true, matching Python behavior.
	if !SupportsReliability("nonexistent", "r") {
		t.Error("expected unknown library to return true for SupportsReliability")
	}
}

func TestKnownLibrary(t *testing.T) {
	if !KnownLibrary("enet") {
		t.Error("expected enet to be known")
	}
	if KnownLibrary("nonexistent") {
		t.Error("expected nonexistent to be unknown")
	}
}

func TestFlushPolicy(t *testing.T) {
	if got := FlushPolicy("enet", "r"); got != "poll_flush" {
		t.Errorf("FlushPolicy(enet, r) = %q, want %q", got, "poll_flush")
	}
	if got := FlushPolicy("raw_udp", "r"); got != "unsupported" {
		t.Errorf("FlushPolicy(raw_udp, r) = %q, want %q", got, "unsupported")
	}
	if got := FlushPolicy("nonexistent", "r"); got != "unknown" {
		t.Errorf("FlushPolicy(nonexistent, r) = %q, want %q", got, "unknown")
	}
}

func TestTransportMode(t *testing.T) {
	if got := TransportMode("quiche", "u"); got != "quic_datagram" {
		t.Errorf("TransportMode(quiche, u) = %q, want %q", got, "quic_datagram")
	}
	if got := TransportMode("udt4", "u"); got != "unsupported" {
		t.Errorf("TransportMode(udt4, u) = %q, want %q", got, "unsupported")
	}
}

func TestScenarioMetadata(t *testing.T) {
	m := ScenarioMetadata("yojimbo", "r")
	if m["supports_reliability"] != "1" {
		t.Error("expected yojimbo r supports_reliability = 1")
	}
	if m["max_connections"] != "64" {
		t.Errorf("max_connections = %q, want %q", m["max_connections"], "64")
	}
	if m["transport_mode"] != "yojimbo_message" {
		t.Errorf("transport_mode = %q, want %q", m["transport_mode"], "yojimbo_message")
	}

	// Unbounded library
	m2 := ScenarioMetadata("gns", "u")
	if m2["max_connections"] != "unbounded" {
		t.Errorf("gns max_connections = %q, want %q", m2["max_connections"], "unbounded")
	}
}

func TestCapabilitiesCount(t *testing.T) {
	if got := len(Capabilities); got != 19 {
		t.Errorf("expected 19 library entries (was told 21 but Python has 19 unique keys), got %d", got)
	}
}
