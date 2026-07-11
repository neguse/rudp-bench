package run

import "testing"

func TestValidateRecordedProcessCommandBindsNamespaceCPUAndExpandedArgv(t *testing.T) {
	cfg := RunConfig{
		Transport: "demo", TotalConns: 1, ServerCPUs: "7,15", ClientCPUs: "3-6",
		ServerCommand: CommandConfig{Path: "/srv-{transport}", Args: []string{"--total", "{total_conns}"}},
		ClientCommand: CommandConfig{Path: "/cli", Args: []string{"--index", "{proc_index}", "--range", "{origin_id_range}"}},
		Netem:         &NetemRegime{ServerNS: "srv-ns", ClientNS: "cli-ns"},
	}
	server := ProcessResult{Role: "server", ProcIndex: -1, Command: []string{
		"ip", "netns", "exec", "srv-ns", "setpriv", "--reuid", "1000", "--regid", "1000", "--init-groups",
		"taskset", "-c", "7,15", "/srv-demo", "--total", "1",
	}}
	if err := ValidateRecordedProcessCommand(cfg, server, true); err != nil {
		t.Fatalf("valid server command rejected: %v", err)
	}
	client := ProcessResult{Role: "client", ProcIndex: 0, Conns: 1, OriginIDEnd: 1, Command: []string{
		"ip", "netns", "exec", "cli-ns", "setpriv", "--reuid", "1000", "--regid", "1000", "--init-groups",
		"taskset", "-c", "3-6", "/cli", "--index", "0", "--range", "0:1",
	}}
	if err := ValidateRecordedProcessCommand(cfg, client, true); err != nil {
		t.Fatalf("valid client command rejected: %v", err)
	}

	for name, mutate := range map[string]func([]string){
		"namespace": func(command []string) { command[3] = "host" },
		"root uid":  func(command []string) { command[6] = "0" },
		"cpu set":   func(command []string) { command[12] = "0" },
		"argv":      func(command []string) { command[len(command)-1] = "2" },
	} {
		t.Run(name, func(t *testing.T) {
			bad := client
			bad.Command = append([]string(nil), client.Command...)
			mutate(bad.Command)
			if err := ValidateRecordedProcessCommand(cfg, bad, true); err == nil {
				t.Fatal("mutated command accepted")
			}
		})
	}
}
