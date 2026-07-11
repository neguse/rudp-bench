package conformance

import (
	"fmt"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/doctor"
	"github.com/neguse/rudp-bench/orchestrator/rig"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

func evaluateSessionDoctor(path, expectedSHA256, serverCPUs, clientCPUs string) (bool, string, error) {
	if path == "" {
		if expectedSHA256 != "" {
			return false, "", fmt.Errorf("session plan binds doctor contents without a doctor_report locator")
		}
		return false, "doctor report is not bound", nil
	}
	if !isSHA256Digest(expectedSHA256) {
		return false, "", fmt.Errorf("session plan has invalid doctor sha256")
	}
	data, err := readBoundedRegular(path, maxSessionJSONBytes)
	if err != nil {
		return false, "", fmt.Errorf("read doctor report: %w", err)
	}
	if actual := run.HashBytes(data); actual != expectedSHA256 {
		return false, "", fmt.Errorf("doctor report changed after session planning")
	}
	var report doctor.Report
	if err := strictDecode(data, &report); err != nil {
		return false, "", fmt.Errorf("decode doctor report: %w", err)
	}
	if err := doctor.ValidateReferenceReportStructure(report); err != nil {
		return false, "", err
	}
	currentHost := run.HostEnvironmentSnapshot()
	currentOrchestrator := run.OrchestratorFingerprint()
	for name, values := range map[string][2]string{
		"orchestrator_sha256": {report.OrchestratorSHA256, currentOrchestrator},
		"hostname":            {report.Hostname, currentHost.Hostname},
		"architecture":        {report.Architecture, currentHost.Architecture},
		"kernel_release":      {report.KernelRelease, currentHost.KernelRelease},
		"clocksource":         {report.Clocksource, currentHost.Clocksource},
	} {
		if values[0] == "" || values[0] != values[1] {
			return false, "", fmt.Errorf("doctor report %s=%q does not match current host value %q", name, values[0], values[1])
		}
	}
	if !report.OK {
		return false, "doctor report outcome is FAIL", nil
	}
	if err := doctor.ValidateReferenceEnvironment(report); err != nil {
		return false, "", fmt.Errorf("doctor reference environment: %w", err)
	}
	if !sameCPUSet(serverCPUs, report.Rig.ServerCPUs) {
		return false, fmt.Sprintf("session server_cpus=%q does not match doctor rig server_cpus=%q", serverCPUs, report.Rig.ServerCPUs), nil
	}
	if !sameCPUSet(clientCPUs, report.Rig.ClientCPUs) {
		return false, fmt.Sprintf("session client_cpus=%q does not match doctor rig client_cpus=%q", clientCPUs, report.Rig.ClientCPUs), nil
	}
	if err := doctor.ValidateReferenceReportFreshness(report, time.Now().UTC()); err != nil {
		return false, err.Error(), nil
	}
	return true, "", nil
}

func sameCPUSet(left, right string) bool {
	leftCPUs, leftErr := rig.ParseCPUSet(left)
	rightCPUs, rightErr := rig.ParseCPUSet(right)
	if leftErr != nil || rightErr != nil || len(leftCPUs) != len(rightCPUs) {
		return false
	}
	for index := range leftCPUs {
		if leftCPUs[index] != rightCPUs[index] {
			return false
		}
	}
	return len(leftCPUs) > 0
}
