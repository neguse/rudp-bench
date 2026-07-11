package run

import (
	"fmt"
	"reflect"
	"strconv"
)

// ValidateRecordedProcessCommand binds a process record to the command that
// Run constructs from its immutable config. Conformance acquisition also
// requires an explicit privilege drop after entering the network namespace.
func ValidateRecordedProcessCommand(cfg RunConfig, process ProcessResult, requirePrivilegeDrop bool) error {
	var command CommandConfig
	var netns, cpus string
	switch process.Role {
	case "server":
		command = expandCommandTemplate(cfg.ServerCommand, templateVars{
			Transport: cfg.Transport, ProcIndex: -1, TotalConns: cfg.TotalConns,
		})
		cpus = cfg.ServerCPUs
		if cfg.Netem != nil {
			netns = cfg.Netem.pairSpec().ServerNS
		}
	case "client":
		command = expandCommandTemplate(cfg.ClientCommand, templateVars{
			Transport: cfg.Transport, ProcIndex: process.ProcIndex, Conns: process.Conns,
			OriginIDStart: process.OriginIDStart, OriginIDEnd: process.OriginIDEnd,
			TotalConns: cfg.TotalConns,
		})
		cpus = cfg.ClientCPUs
		if cfg.Netem != nil {
			netns = cfg.Netem.pairSpec().ClientNS
		}
	default:
		return fmt.Errorf("unknown process role %q", process.Role)
	}

	actual := process.Command
	index := 0
	if netns != "" {
		prefix := []string{"ip", "netns", "exec", netns}
		if len(actual) < len(prefix) || !reflect.DeepEqual(actual[:len(prefix)], prefix) {
			return fmt.Errorf("command does not enter planned network namespace %q", netns)
		}
		index = len(prefix)
	}
	if requirePrivilegeDrop {
		if netns == "" {
			return fmt.Errorf("privilege-drop validation requires a network namespace")
		}
		if len(actual) < index+7 || actual[index] != "setpriv" ||
			actual[index+1] != "--reuid" || actual[index+3] != "--regid" || actual[index+5] != "--init-groups" {
			return fmt.Errorf("command does not drop privileges inside the network namespace")
		}
		uid, uidErr := strconv.ParseUint(actual[index+2], 10, 32)
		gid, gidErr := strconv.ParseUint(actual[index+4], 10, 32)
		if uidErr != nil || gidErr != nil || uid == 0 || gid == 0 {
			return fmt.Errorf("command privilege-drop uid/gid must be positive numeric IDs")
		}
		index += 6
	}
	if cpus != "" {
		prefix := []string{"taskset", "-c", cpus}
		if len(actual) < index+len(prefix) || !reflect.DeepEqual(actual[index:index+len(prefix)], prefix) {
			return fmt.Errorf("command does not use planned CPU set %q", cpus)
		}
		index += len(prefix)
	}
	want := append([]string{command.Path}, command.Args...)
	if !reflect.DeepEqual(actual[index:], want) {
		return fmt.Errorf("launched argv does not match expanded command plan")
	}
	return nil
}
