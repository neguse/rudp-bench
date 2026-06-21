package bench

import (
	"reflect"
	"testing"
)

func TestDefaultProfiles(t *testing.T) {
	profiles := DefaultProfiles()
	if len(profiles) != 4 {
		t.Fatalf("expected 4 profiles, got %d", len(profiles))
	}

	mr, ok := ProfileByName("media_relay")
	if !ok {
		t.Fatal("media_relay not found")
	}
	if mr.RateR != 0 {
		t.Errorf("media_relay RateR = %d, want 0", mr.RateR)
	}
	if mr.RateU != 30 {
		t.Errorf("media_relay RateU = %d, want 30", mr.RateU)
	}
	if mr.Size != 1000 {
		t.Errorf("media_relay Size = %d, want 1000", mr.Size)
	}
	if mr.Mode != "broadcast" {
		t.Errorf("media_relay Mode = %q, want %q", mr.Mode, "broadcast")
	}

	// Verify all names
	names := []string{"media_relay", "game_server", "reliable_echo", "echo"}
	for _, name := range names {
		if _, ok := ProfileByName(name); !ok {
			t.Errorf("profile %q not found", name)
		}
	}

	// Verify unknown profile
	if _, ok := ProfileByName("nonexistent"); ok {
		t.Error("expected nonexistent profile to return false")
	}
}

func TestParseInts(t *testing.T) {
	tests := []struct {
		input string
		want  []int
	}{
		{"1 5 50", []int{1, 5, 50}},
		{"1,5,50", []int{1, 5, 50}},
		{"", []int{}},
		{"  1  5  ", []int{1, 5}},
		{"1, 5, 50", []int{1, 5, 50}},
	}
	for _, tt := range tests {
		got := ParseInts(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("ParseInts(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}
