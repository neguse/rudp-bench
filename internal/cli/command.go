package cli

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// CommandRunner executes shell commands with optional dry-run support.
type CommandRunner struct {
	Dir    string
	DryRun bool
}

// Run prints the quoted command to stdout, then executes it unless DryRun is
// set. Stdout, stderr, and stdin are passed through to the child process.
func (r *CommandRunner) Run(name string, args ...string) error {
	argv := append([]string{name}, args...)
	fmt.Println("+", QuoteCommand(argv))
	if r.DryRun {
		return nil
	}
	cmd := exec.Command(name, args...)
	cmd.Dir = r.Dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	return cmd.Run()
}

// QuoteCommand joins an argument vector into a single display string with
// shell-safe quoting.
func QuoteCommand(argv []string) string {
	parts := make([]string, 0, len(argv))
	for _, arg := range argv {
		parts = append(parts, QuoteArg(arg))
	}
	return strings.Join(parts, " ")
}

// QuoteArg returns a shell-safe representation of arg. Empty strings become
// ''. Arguments that contain no special characters are returned as-is.
// Everything else is single-quoted with embedded single-quotes escaped.
func QuoteArg(arg string) string {
	if arg == "" {
		return "''"
	}
	if strings.IndexFunc(arg, func(r rune) bool {
		return !(r >= 'A' && r <= 'Z' ||
			r >= 'a' && r <= 'z' ||
			r >= '0' && r <= '9' ||
			strings.ContainsRune("@%_+=:,./-", r))
	}) == -1 {
		return arg
	}
	return "'" + strings.ReplaceAll(arg, "'", `'\''`) + "'"
}
