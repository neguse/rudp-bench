package cli

import (
	"bytes"
	"os"
	"strings"
	"testing"
)

func TestQuoteArg(t *testing.T) {
	tests := []struct {
		name string
		arg  string
		want string
	}{
		{name: "empty", arg: "", want: "''"},
		{name: "no special chars", arg: "hello", want: "hello"},
		{name: "path with slashes", arg: "/usr/bin/cmake", want: "/usr/bin/cmake"},
		{name: "alphanumeric with safe chars", arg: "foo@bar:1.0-2", want: "foo@bar:1.0-2"},
		{name: "spaces", arg: "hello world", want: "'hello world'"},
		{name: "single quotes", arg: "it's", want: `'it'\''s'`},
		{name: "multiple single quotes", arg: "it's a 'test'", want: `'it'\''s a '\''test'\'''`},
		{name: "special shell chars", arg: "foo;bar", want: "'foo;bar'"},
		{name: "dollar sign", arg: "$HOME", want: "'$HOME'"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := QuoteArg(tt.arg)
			if got != tt.want {
				t.Errorf("QuoteArg(%q) = %q, want %q", tt.arg, got, tt.want)
			}
		})
	}
}

func TestQuoteCommand(t *testing.T) {
	got := QuoteCommand([]string{"cmake", "--build", "build dir", "-j", "4"})
	want := "cmake --build 'build dir' -j 4"
	if got != want {
		t.Errorf("QuoteCommand() = %q, want %q", got, want)
	}
}

func TestCommandRunnerDryRun(t *testing.T) {
	// Capture stdout to verify the "+ <command>" output.
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w

	runner := &CommandRunner{Dir: ".", DryRun: true}
	runErr := runner.Run("echo", "hello", "world")

	w.Close()
	os.Stdout = old

	if runErr != nil {
		t.Fatalf("dry-run returned error: %v", runErr)
	}

	var buf bytes.Buffer
	if _, err := buf.ReadFrom(r); err != nil {
		t.Fatal(err)
	}
	output := strings.TrimSpace(buf.String())
	want := "+ echo hello world"
	if output != want {
		t.Errorf("dry-run output = %q, want %q", output, want)
	}
}

func TestCommandRunnerDryRunDoesNotExecute(t *testing.T) {
	// A command that would fail if actually executed should succeed in
	// dry-run mode.
	runner := &CommandRunner{Dir: ".", DryRun: true}
	// Redirect stdout so the test output stays clean.
	old := os.Stdout
	w, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	defer func() { os.Stdout = old; w.Close() }()

	err = runner.Run("false")
	if err != nil {
		t.Fatalf("dry-run should not execute command, got error: %v", err)
	}
}
