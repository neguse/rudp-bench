package doctor

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"unsafe"
)

type Result struct {
	Name    string
	OK      bool
	Message string
	Fatal   bool
}

func isTerminal() bool {
	var termios syscall.Termios
	_, _, err := syscall.Syscall6(syscall.SYS_IOCTL, os.Stdout.Fd(), syscall.TCGETS, uintptr(unsafe.Pointer(&termios)), 0, 0, 0)
	return err == 0
}

func Check(buildDir, serverCPU, clientCPU string) []Result {
	var results []Result
	color := isTerminal()

	check := func(name string, fatal bool, fn func() (bool, string)) {
		ok, msg := fn()
		results = append(results, Result{Name: name, OK: ok, Message: msg, Fatal: fatal})
		mark := "✓"
		if !ok {
			mark = "✗"
		}
		if color {
			if ok {
				fmt.Printf("\033[32m%s\033[0m %s: %s\n", mark, name, msg)
			} else if fatal {
				fmt.Printf("\033[31m%s\033[0m %s: %s\n", mark, name, msg)
			} else {
				fmt.Printf("\033[33m%s\033[0m %s: %s\n", mark, name, msg)
			}
		} else {
			fmt.Printf("%s %s: %s\n", mark, name, msg)
		}
	}

	check("cmake", true, func() (bool, string) {
		out, err := exec.Command("cmake", "--version").Output()
		if err != nil {
			return false, "not found"
		}
		line := strings.SplitN(string(out), "\n", 2)[0]
		return true, line
	})

	check("go", true, func() (bool, string) {
		out, err := exec.Command("go", "version").Output()
		if err != nil {
			return false, "not found"
		}
		return true, strings.TrimSpace(string(out))
	})

	check("systemd-run", false, func() (bool, string) {
		out, err := exec.Command("systemd-run", "--version").Output()
		if err != nil {
			return false, "not found (needed for isolate=systemd)"
		}
		line := strings.SplitN(string(out), "\n", 2)[0]
		return true, line
	})

	check("tc", false, func() (bool, string) {
		out, err := exec.Command("tc", "-Version").Output()
		if err != nil {
			return false, "not found (needed for netem)"
		}
		return true, strings.TrimSpace(string(out))
	})

	check("taskset", false, func() (bool, string) {
		_, err := exec.LookPath("taskset")
		if err != nil {
			return false, "not found (needed for isolate=taskset)"
		}
		return true, "found"
	})

	check("sudo", false, func() (bool, string) {
		err := exec.Command("sudo", "-n", "true").Run()
		if err != nil {
			return false, "passwordless sudo not available"
		}
		return true, "passwordless sudo OK"
	})

	check("harness", true, func() (bool, string) {
		bin := buildDir + "/harness/rudp-bench"
		if _, err := os.Stat(bin); err != nil {
			return false, fmt.Sprintf("not found: %s", bin)
		}
		return true, bin
	})

	check("litenetlib", false, func() (bool, string) {
		bin := "adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
		if _, err := os.Stat(bin); err != nil {
			return false, fmt.Sprintf("not found: %s (optional)", bin)
		}
		return true, bin
	})

	check("cpu", false, func() (bool, string) {
		data, err := os.ReadFile("/sys/devices/system/cpu/online")
		if err != nil {
			return false, "cannot read /sys/devices/system/cpu/online"
		}
		online := strings.TrimSpace(string(data))
		msg := "online=" + online
		if serverCPU != "" {
			msg += " server=" + serverCPU
		}
		if clientCPU != "" {
			msg += " client=" + clientCPU
		}
		return true, msg
	})

	return results
}

func HasFatal(results []Result) bool {
	for _, r := range results {
		if !r.OK && r.Fatal {
			return true
		}
	}
	return false
}
