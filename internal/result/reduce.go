package result

import (
	"fmt"
	"os"
	"strconv"

	"github.com/neguse/rudp-bench/internal/bench"
)

const (
	timeoutStatus       = 124
	missingBinaryStatus = 127
)

// ResultFields matches Python RESULT_FIELDS (23 fields).
var ResultFields = []string{
	"run_id",
	"scenario_id",
	"library",
	"valid",
	"invalid_reason",
	"delivery_ratio",
	"forward_delivery_ratio",
	"forward_delivery_ratio_r",
	"forward_delivery_ratio_u",
	"server_echo_accept_ratio",
	"server_echo_accept_ratio_r",
	"server_echo_accept_ratio_u",
	"return_delivery_ratio",
	"return_delivery_ratio_r",
	"return_delivery_ratio_u",
	"rtt_r_p50_us",
	"rtt_r_p95_us",
	"rtt_r_p99_us",
	"rtt_u_p50_us",
	"rtt_u_p95_us",
	"rtt_u_p99_us",
	"server_cpu_pct",
	"server_cpu_pct_peak",
}

// DiagnosticFields matches Python DIAGNOSTIC_FIELDS (39 fields).
var DiagnosticFields = []string{
	"run_id",
	"scenario_id",
	"role",
	"exit_reason",
	"exit_status",
	"cpu_pct",
	"cpu_pct_peak",
	"close_ms",
	"rss_mb",
	"attempted",
	"accepted",
	"delivered",
	"accepted_r",
	"accepted_u",
	"delivered_r",
	"delivered_u",
	"accepted_ratio",
	"delivery_ratio",
	"server_received",
	"server_echo_accepted",
	"server_received_r",
	"server_received_u",
	"server_echo_accepted_r",
	"server_echo_accepted_u",
	"server_recv_drained_p99",
	"server_recv_drained_max",
	"client_tick_ok",
	"client_tick_ok_check",
	"client_tick_gap_p99_us",
	"client_pacing_lag_p99_us",
	"client_recv_drained_p99",
	"client_recv_drained_max",
	"client_outstanding_max",
	"conn_peak",
	"conn_disc_transport",
	"conn_disc_peer",
	"raw_result_path",
	"stdout_path",
	"stderr_path",
	"delivery_dedup_policy",
}

// ScenarioFields extends Python SCENARIO_FIELDS with an explicit "profile"
// column (24 fields, includes capability metadata). The profile column lets
// downstream consumers (Aggregate/report) join rows to profiles without
// parsing scenario_id with regexes.
var ScenarioFields = []string{
	"run_id",
	"scenario_id",
	"library",
	"profile",
	"rate_r",
	"rate_u",
	"size",
	"conns",
	"loss",
	"mode",
	"duration_s",
	"warmup_s",
	"ramp_up_ms",
	"tail_ms",
	"idle_policy",
	"server_cpu_pin",
	"client_cpu_pin",
	"pinning_policy",
	"flush_policy",
	"supports_reliability",
	"min_payload_bytes",
	"max_payload_bytes",
	"max_connections",
	"transport_mode",
	// 公平性メタデータ（improvements §3.2-3.3）: adapter 申告の実効 CC と
	// スレッドモデル。CC 無効群/BBR 群や single/multi スレッドを結果表で
	// 分離して読むための列。
	"cc_algo",
	"thread_model",
}

// AppendOpts holds all parameters for the Append function, matching the Python
// argparse namespace used by reduce_result.py.
type AppendOpts struct {
	Results      string
	Diagnostics  string
	Scenarios    string
	ServerCSV    string
	ClientCSV    string
	ServerStdout string
	ServerStderr string
	ClientStdout string
	ClientStderr string
	ServerStatus string
	ClientStatus string
	RunID        string
	ScenarioID   string
	Library      string
	Profile      string
	RateR        string
	RateU        string
	Size         string
	Conns        string
	Loss         string
	Mode         string
	Duration     string
	Warmup       string
	RampUpMs     string
	TailMs       string
	Idle         string
	ServerCPUPin string
	ClientCPUPin string
}

// Init creates fresh CSV files with headers for results, diagnostics, and scenarios.
func Init(results, diagnostics, scenarios string) error {
	for _, pair := range [][2]interface{}{
		{results, ResultFields},
		{diagnostics, DiagnosticFields},
		{scenarios, ScenarioFields},
	} {
		path := pair[0].(string)
		fields := pair[1].([]string)
		os.Remove(path)
		if err := EnsureHeader(path, fields); err != nil {
			return err
		}
	}
	return nil
}

// Append reduces raw role CSVs into canonical results, diagnostics, and scenario rows.
func Append(opts AppendOpts) error {
	server, err := ReadCSVRow(opts.ServerCSV)
	if err != nil {
		return fmt.Errorf("reading server CSV: %w", err)
	}
	client, err := ReadCSVRow(opts.ClientCSV)
	if err != nil {
		return fmt.Errorf("reading client CSV: %w", err)
	}

	reason := invalidReason(server, client, opts)
	valid := "0"
	if reason == "ok" {
		valid = "1"
	}
	channel := primaryChannel(opts)
	capMeta := bench.ScenarioMetadata(opts.Library, channel)

	// Scenario row
	scenarioRow := map[string]string{
		"run_id":      opts.RunID,
		"scenario_id": opts.ScenarioID,
		"library":     opts.Library,
		"profile":     opts.Profile,
		"rate_r":      opts.RateR,
		"rate_u":      opts.RateU,
		"size":        opts.Size,
		"conns":       opts.Conns,
		"loss":        opts.Loss,
		"mode":        opts.Mode,
		"duration_s":  opts.Duration,
		"warmup_s":    opts.Warmup,
		"ramp_up_ms":  opts.RampUpMs,
		"tail_ms":     opts.TailMs,
		"idle_policy": opts.Idle,
		"server_cpu_pin":  opts.ServerCPUPin,
		"client_cpu_pin":  opts.ClientCPUPin,
		"pinning_policy":  pinningPolicy(opts),
		"flush_policy":    scenarioFlushPolicy(opts, server, client),
		"cc_algo":         firstNonEmpty(server["cc_algo"], client["cc_algo"]),
		"thread_model":    firstNonEmpty(server["thread_model"], client["thread_model"]),
	}
	for k, v := range capMeta {
		scenarioRow[k] = v
	}
	if err := AppendRow(opts.Scenarios, ScenarioFields, scenarioRow); err != nil {
		return err
	}

	// Diagnostic rows (server + client)
	serverDiag := diagnosticRow(opts.RunID, opts.ScenarioID, "server", server,
		opts.ServerCSV, opts.ServerStdout, opts.ServerStderr, opts.ServerStatus, reason)
	if err := AppendRow(opts.Diagnostics, DiagnosticFields, serverDiag); err != nil {
		return err
	}
	clientDiag := diagnosticRow(opts.RunID, opts.ScenarioID, "client", client,
		opts.ClientCSV, opts.ClientStdout, opts.ClientStderr, opts.ClientStatus, reason)
	if err := AppendRow(opts.Diagnostics, DiagnosticFields, clientDiag); err != nil {
		return err
	}

	// Result row
	resultRow := map[string]string{
		"run_id":                    opts.RunID,
		"scenario_id":              opts.ScenarioID,
		"library":                  opts.Library,
		"valid":                    valid,
		"invalid_reason":           reason,
		"delivery_ratio":           canonicalDeliveryRatio(client),
		"forward_delivery_ratio":   forwardDeliveryRatio(server, client, opts),
		"forward_delivery_ratio_r": forwardDeliveryRatioChannel(server, opts, "r"),
		"forward_delivery_ratio_u": forwardDeliveryRatioChannel(server, opts, "u"),
		"server_echo_accept_ratio":   serverEchoAcceptRatio(server, opts),
		"server_echo_accept_ratio_r": serverEchoAcceptRatioChannel(server, opts, "r"),
		"server_echo_accept_ratio_u": serverEchoAcceptRatioChannel(server, opts, "u"),
		"return_delivery_ratio":      returnDeliveryRatio(server, client),
		"return_delivery_ratio_r":    returnDeliveryRatioChannel(server, client, "r"),
		"return_delivery_ratio_u":    returnDeliveryRatioChannel(server, client, "u"),
	}
	if client != nil {
		resultRow["rtt_r_p50_us"] = mapGet(client, "rtt_r_p50_us", "")
		resultRow["rtt_r_p95_us"] = mapGet(client, "rtt_r_p95_us", "")
		resultRow["rtt_r_p99_us"] = mapGet(client, "rtt_r_p99_us", "")
		resultRow["rtt_u_p50_us"] = mapGet(client, "rtt_u_p50_us", "")
		resultRow["rtt_u_p95_us"] = mapGet(client, "rtt_u_p95_us", "")
		resultRow["rtt_u_p99_us"] = mapGet(client, "rtt_u_p99_us", "")
	}
	if server != nil {
		resultRow["server_cpu_pct"] = mapGet(server, "cpu_pct", "")
		resultRow["server_cpu_pct_peak"] = mapGet(server, "cpu_pct_peak", "")
	}
	return AppendRow(opts.Results, ResultFields, resultRow)
}

// primaryChannel returns "r" or "u" depending on which channel drives
// capability lookups. Reliable wins when both are active.
func primaryChannel(opts AppendOpts) string {
	if IntOrZero(opts.RateR) > 0 {
		return "r"
	}
	return "u"
}

// roleExitReason determines the exit reason for a role.
func roleExitReason(role string, raw map[string]string, status string) string {
	statusCode, ok := IntOrNone(status)
	if ok && statusCode == timeoutStatus {
		return role + "_timeout"
	}
	if ok && statusCode == missingBinaryStatus {
		return role + "_missing_binary"
	}
	if ok && statusCode != 0 {
		return role + "_crash"
	}
	if raw == nil {
		return "missing_raw_result"
	}
	return "ok"
}

// acceptedCount returns the accepted message count from a client row.
func acceptedCount(client map[string]string) int {
	if client == nil {
		return 0
	}
	if v, ok := IntOrNone(client["accepted"]); ok {
		return v
	}
	if v, ok := IntOrNone(client["client_accepted"]); ok {
		return v
	}
	if v, ok := IntOrNone(client["sent"]); ok {
		return v
	}
	return 0
}

// clientAttempted returns the attempted field from a raw CSV row.
func clientAttempted(raw map[string]string) string {
	if v, ok := raw["client_attempted"]; ok && v != "" {
		return v
	}
	return raw["client_offered"]
}

// clientAccepted returns the accepted field from a raw CSV row.
func clientAccepted(raw map[string]string) string {
	if v, ok := raw["accepted"]; ok && v != "" {
		return v
	}
	if v, ok := raw["client_accepted"]; ok && v != "" {
		return v
	}
	return raw["sent"]
}

// clientAcceptedRatio computes the accepted/attempted ratio.
func clientAcceptedRatio(raw map[string]string) string {
	attempted, aOK := IntOrNone(clientAttempted(raw))
	accepted, bOK := IntOrNone(clientAccepted(raw))
	if !aOK || !bOK {
		return mapGet(raw, "client_accepted_ratio", "")
	}
	if attempted == 0 {
		return "0.0000"
	}
	return fmt.Sprintf("%.4f", float64(accepted)/float64(attempted))
}

// RecomputedTickOK recomputes the validity gate from raw CSV ratios.
func RecomputedTickOK(raw map[string]string) string {
	if raw == nil {
		return ""
	}
	accRatioStr := mapGet(raw, "client_accepted_ratio", "")
	attRatioStr := mapGet(raw, "client_attempted_ratio", "")
	accRatio, err1 := strconv.ParseFloat(accRatioStr, 64)
	attRatio, err2 := strconv.ParseFloat(attRatioStr, 64)
	if err1 != nil {
		accRatio = 0.0
	}
	if err2 != nil {
		attRatio = 0.0
	}
	combined := IntOrZero(mapGet(raw, "rate_r", "")) + IntOrZero(mapGet(raw, "rate_u", ""))
	ok := accRatio >= 0.99
	if combined > 0 {
		ok = ok && attRatio >= 0.99
	}
	if ok {
		return "1"
	}
	return "0"
}

// CanonicalDeliveryRatio computes delivered/accepted as a 4-decimal string.
func CanonicalDeliveryRatio(client map[string]string) string {
	return canonicalDeliveryRatio(client)
}

func canonicalDeliveryRatio(client map[string]string) string {
	if client == nil {
		return ""
	}
	delivered, ok := IntOrNone(client["delivered"])
	if !ok {
		return mapGet(client, "delivery_ratio", "")
	}
	accepted := acceptedCount(client)
	if accepted == 0 {
		return "0.0000"
	}
	return fmt.Sprintf("%.4f", float64(delivered)/float64(accepted))
}

// expectedPerSend returns how many deliveries are expected per send operation.
func expectedPerSend(opts AppendOpts) int {
	conns, _ := IntOrNone(opts.Conns)
	if opts.Mode == "broadcast" {
		return conns
	}
	return 1
}

// clientOutboundMessages returns the number of outbound messages the client sent.
func clientOutboundMessages(client map[string]string, opts AppendOpts) int {
	accepted := acceptedCount(client)
	eps := expectedPerSend(opts)
	if eps < 1 {
		eps = 1
	}
	if accepted == 0 {
		return 0
	}
	return accepted / eps
}

// plannedChannelOutboundMessages returns the planned send count for a channel.
func plannedChannelOutboundMessages(opts AppendOpts, channel string) int {
	var rate int
	if channel == "r" {
		rate = IntOrZero(opts.RateR)
	} else {
		rate = IntOrZero(opts.RateU)
	}
	conns, _ := IntOrNone(opts.Conns)
	duration, _ := IntOrNone(opts.Duration)
	return rate * conns * duration
}

// unsupportedReliable checks if the library doesn't support reliable channel
// when reliable traffic is requested.
func unsupportedReliable(opts AppendOpts) bool {
	return IntOrZero(opts.RateR) > 0 && !bench.SupportsReliability(opts.Library, "r")
}

// unsupportedUnreliable checks if the library doesn't support unreliable channel
// when unreliable traffic is requested.
func unsupportedUnreliable(opts AppendOpts) bool {
	return IntOrZero(opts.RateU) > 0 && !bench.SupportsReliability(opts.Library, "u")
}

// unsupportedPayload checks if the payload size exceeds library limits.
func unsupportedPayload(opts AppendOpts) bool {
	size, ok := IntOrNone(opts.Size)
	if !ok {
		return true
	}
	if size < bench.MinPayloadBytes {
		return true
	}
	for _, channel := range []string{"r", "u"} {
		var rate int
		if channel == "r" {
			rate = IntOrZero(opts.RateR)
		} else {
			rate = IntOrZero(opts.RateU)
		}
		if rate <= 0 {
			continue
		}
		maxPayload, hasCap := bench.MaxPayloadBytes(opts.Library, channel)
		if hasCap && size > maxPayload {
			return true
		}
	}
	return false
}

// unsupportedConns checks if the connection count exceeds library limits.
func unsupportedConns(opts AppendOpts) bool {
	conns, ok := IntOrNone(opts.Conns)
	if !ok {
		return true
	}
	maxConns, hasCap := bench.MaxConnections(opts.Library)
	return hasCap && conns > maxConns
}

// invalidReason determines why a scenario is invalid, or returns "ok".
func invalidReason(server, client map[string]string, opts AppendOpts) string {
	serverExit := roleExitReason("server", server, opts.ServerStatus)
	clientExit := roleExitReason("client", client, opts.ClientStatus)

	if unsupportedReliable(opts) {
		return "unsupported_reliable"
	}
	if unsupportedUnreliable(opts) {
		return "unsupported_unreliable"
	}
	if unsupportedPayload(opts) {
		return "unsupported_payload"
	}
	if unsupportedConns(opts) {
		return "unsupported_conns"
	}
	if serverExit == "server_timeout" {
		return "server_timeout"
	}
	if clientExit == "client_timeout" {
		return "client_timeout"
	}
	if serverExit == "server_missing_binary" || clientExit == "client_missing_binary" {
		return "missing_binary"
	}
	if serverExit == "server_crash" || serverExit == "missing_raw_result" {
		return "server_crash"
	}
	if clientExit == "client_crash" || clientExit == "missing_raw_result" {
		return "client_crash"
	}
	if client != nil && client["client_tick_ok"] == "0" {
		return "client_tick"
	}
	if acceptedCount(client) == 0 {
		return "no_accepted_messages"
	}
	return "ok"
}

// diagnosticRow builds a diagnostic output row for a role.
func diagnosticRow(runID, scenarioID, role string, raw map[string]string,
	rawPath, stdoutPath, stderrPath, status, scenarioReason string) map[string]string {

	exitReason := roleExitReason(role, raw, status)
	if scenarioReason == "unsupported_reliable" ||
		scenarioReason == "unsupported_unreliable" ||
		scenarioReason == "unsupported_payload" ||
		scenarioReason == "unsupported_conns" ||
		scenarioReason == "missing_binary" {
		exitReason = scenarioReason
	}

	if raw == nil {
		return map[string]string{
			"run_id":          runID,
			"scenario_id":    scenarioID,
			"role":           role,
			"exit_reason":    exitReason,
			"exit_status":    status,
			"raw_result_path": rawPath,
			"stdout_path":    stdoutPath,
			"stderr_path":    stderrPath,
		}
	}

	isClient := role == "client"
	row := map[string]string{
		"run_id":       runID,
		"scenario_id": scenarioID,
		"role":        role,
		"exit_reason": exitReason,
		"exit_status": status,
		"cpu_pct":     mapGet(raw, "cpu_pct", ""),
		"cpu_pct_peak": mapGet(raw, "cpu_pct_peak", ""),
		"close_ms":    mapGet(raw, "close_ms", ""),
		"rss_mb":      mapGet(raw, "rss_mb", ""),
		"conn_peak":         mapGet(raw, "conn_peak", ""),
		"conn_disc_transport": mapGet(raw, "conn_disc_transport", ""),
		"conn_disc_peer":    mapGet(raw, "conn_disc_peer", ""),
		"raw_result_path":   rawPath,
		"stdout_path":      stdoutPath,
		"stderr_path":      stderrPath,
	}

	if isClient {
		row["attempted"] = clientAttempted(raw)
		row["accepted"] = clientAccepted(raw)
		row["delivered"] = mapGet(raw, "delivered", "")
		row["accepted_r"] = mapGet(raw, "accepted_r", "")
		row["accepted_u"] = mapGet(raw, "accepted_u", "")
		row["delivered_r"] = mapGet(raw, "delivered_r", "")
		row["delivered_u"] = mapGet(raw, "delivered_u", "")
		row["accepted_ratio"] = clientAcceptedRatio(raw)
		row["delivery_ratio"] = canonicalDeliveryRatio(raw)
		row["client_tick_ok"] = mapGet(raw, "client_tick_ok", "")
		row["client_tick_ok_check"] = RecomputedTickOK(raw)
		row["client_tick_gap_p99_us"] = mapGet(raw, "client_tick_gap_p99_us", "")
		row["client_pacing_lag_p99_us"] = mapGet(raw, "client_pacing_lag_p99_us", "")
		row["client_recv_drained_p99"] = mapGet(raw, "client_recv_drained_p99", "")
		row["client_recv_drained_max"] = mapGet(raw, "client_recv_drained_max", "")
		row["client_outstanding_max"] = mapGet(raw, "client_outstanding_max", "")
		row["delivery_dedup_policy"] = mapGet(raw, "delivery_dedup_policy", "")
	} else {
		row["server_received"] = mapGet(raw, "server_received", "")
		row["server_echo_accepted"] = mapGet(raw, "server_echo_accepted", "")
		row["server_received_r"] = mapGet(raw, "server_received_r", "")
		row["server_received_u"] = mapGet(raw, "server_received_u", "")
		row["server_echo_accepted_r"] = mapGet(raw, "server_echo_accepted_r", "")
		row["server_echo_accepted_u"] = mapGet(raw, "server_echo_accepted_u", "")
		row["server_recv_drained_p99"] = mapGet(raw, "server_recv_drained_p99", "")
		row["server_recv_drained_max"] = mapGet(raw, "server_recv_drained_max", "")
	}

	return row
}

// forwardDeliveryRatio computes server_received / client_outbound_messages.
func forwardDeliveryRatio(server, client map[string]string, opts AppendOpts) string {
	if server == nil || client == nil {
		return ""
	}
	if !mapHas(server, "server_received") {
		return ""
	}
	return RatioStr(IntOrZero(server["server_received"]),
		clientOutboundMessages(client, opts))
}

// serverEchoAcceptRatio computes echo_accepted / (received * expected_per_send).
func serverEchoAcceptRatio(server map[string]string, opts AppendOpts) string {
	if server == nil {
		return ""
	}
	if !mapHas(server, "server_received") || !mapHas(server, "server_echo_accepted") {
		return ""
	}
	received := IntOrZero(server["server_received"])
	echoAccepted := IntOrZero(server["server_echo_accepted"])
	eps := expectedPerSend(opts)
	if eps < 1 {
		eps = 1
	}
	return RatioStr(echoAccepted, received*eps)
}

// forwardDeliveryRatioChannel computes the per-channel forward delivery ratio.
func forwardDeliveryRatioChannel(server map[string]string, opts AppendOpts, channel string) string {
	if server == nil {
		return ""
	}
	col := "server_received_" + channel
	if !mapHas(server, col) {
		return ""
	}
	return RatioStr(IntOrZero(server[col]),
		plannedChannelOutboundMessages(opts, channel))
}

// serverEchoAcceptRatioChannel computes the per-channel server echo accept ratio.
func serverEchoAcceptRatioChannel(server map[string]string, opts AppendOpts, channel string) string {
	if server == nil {
		return ""
	}
	receivedCol := "server_received_" + channel
	acceptedCol := "server_echo_accepted_" + channel
	if !mapHas(server, receivedCol) || !mapHas(server, acceptedCol) {
		return ""
	}
	received := IntOrZero(server[receivedCol])
	echoAccepted := IntOrZero(server[acceptedCol])
	eps := expectedPerSend(opts)
	if eps < 1 {
		eps = 1
	}
	return RatioStr(echoAccepted, received*eps)
}

// returnDeliveryRatio computes client_delivered / server_echo_accepted.
func returnDeliveryRatio(server, client map[string]string) string {
	if server == nil || client == nil {
		return ""
	}
	if !mapHas(server, "server_echo_accepted") {
		return ""
	}
	delivered, _ := IntOrNone(client["delivered"])
	return RatioStr(delivered, IntOrZero(server["server_echo_accepted"]))
}

// returnDeliveryRatioChannel computes the per-channel return delivery ratio.
func returnDeliveryRatioChannel(server, client map[string]string, channel string) string {
	if server == nil || client == nil {
		return ""
	}
	serverCol := "server_echo_accepted_" + channel
	clientCol := "delivered_" + channel
	if !mapHas(server, serverCol) || !mapHas(client, clientCol) {
		return ""
	}
	return RatioStr(IntOrZero(client[clientCol]), IntOrZero(server[serverCol]))
}

// scenarioFlushPolicy determines the flush policy for a scenario.
func scenarioFlushPolicy(opts AppendOpts, server, client map[string]string) string {
	channel := primaryChannel(opts)
	if !bench.SupportsReliability(opts.Library, channel) {
		return bench.FlushPolicy(opts.Library, channel)
	}
	for _, raw := range []map[string]string{client, server} {
		if raw != nil {
			if v := raw["flush_policy"]; v != "" {
				return v
			}
		}
	}
	return bench.FlushPolicy(opts.Library, channel)
}

// firstNonEmpty returns the first non-empty string among the arguments
// ("unknown" if all are empty). raw CSV に列が無い旧データでは空になる。
func firstNonEmpty(values ...string) string {
	for _, v := range values {
		if v != "" {
			return v
		}
	}
	return "unknown"
}

// pinningPolicy formats the CPU pinning description.
func pinningPolicy(opts AppendOpts) string {
	serverPin := opts.ServerCPUPin
	clientPin := opts.ClientCPUPin
	if serverPin == "" && clientPin == "" {
		return "none"
	}
	sp := serverPin
	if sp == "" {
		sp = "none"
	}
	cp := clientPin
	if cp == "" {
		cp = "none"
	}
	return "server=" + sp + ";client=" + cp
}
