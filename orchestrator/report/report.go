// Package report は sweep 出力(capacity.json / results.jsonl)から
// レポートの表を生成し、markdown 内のマーカー区間を置換する。
// 散文は人間が書き、数値は機械が埋める。
package report

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/run"
	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

// 表の列順(v2 レポートの既存順)。sweep に居ない transport は列ごと省く。
var transportOrder = []string{"enet", "gns", "litenetlib", "msquic", "websocket", "magiconion"}

// 行順: 平面セル 9 + synthetic 2。anchor はラベルを付ける。
var workloadRows = []struct {
	Name   string
	Anchor string
}{
	{"r10p128", ""}, {"r10p200", ""}, {"r10p1000", ""},
	{"r20p128", "br"}, {"r20p200", ""}, {"r20p1000", "video"},
	{"r60p128", ""}, {"r60p200", "vr"}, {"r60p1000", ""},
	{"echo", "synthetic"}, {"reliable_echo", "synthetic"},
}

// anchor の絶対鮮度予算(docs/profiles.md で凍結した値)。
var anchorBudgetNS = map[string]uint64{
	"r20p128":  100_000_000, // br_fanout
	"r60p200":  150_000_000, // vr_room
	"r20p1000": 150_000_000, // video_room
}

type SweepData struct {
	Regime           string
	CampaignIdentity string
	Cells            map[string]map[string]sweep.CellRecord // transport → workload → cell
	Points           map[string]sweep.PointRecord           // pointKey → record
}

func LoadSweep(dir string) (*SweepData, error) {
	var capFile struct {
		Seed  int64              `json:"seed"`
		Cells []sweep.CellRecord `json:"cells"`
	}
	data, err := os.ReadFile(filepath.Join(dir, "capacity.json"))
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(data, &capFile); err != nil {
		return nil, fmt.Errorf("%s/capacity.json: %w", dir, err)
	}
	if len(capFile.Cells) == 0 {
		return nil, fmt.Errorf("%s/capacity.json has no cells", dir)
	}

	sd := &SweepData{
		Regime:           capFile.Cells[0].Regime,
		CampaignIdentity: capFile.Cells[0].CampaignIdentity,
		Cells:            map[string]map[string]sweep.CellRecord{},
		Points:           map[string]sweep.PointRecord{},
	}
	for _, c := range capFile.Cells {
		if c.Regime != sd.Regime {
			return nil, fmt.Errorf("%s/capacity.json mixes regimes %q and %q", dir, sd.Regime, c.Regime)
		}
		if c.CampaignIdentity != sd.CampaignIdentity {
			return nil, fmt.Errorf("%s/capacity.json mixes campaign identities", dir)
		}
		if sd.Cells[c.Transport] == nil {
			sd.Cells[c.Transport] = map[string]sweep.CellRecord{}
		}
		if _, exists := sd.Cells[c.Transport][c.TestName()]; exists {
			return nil, fmt.Errorf("%s/capacity.json has duplicate cell %s/%s", dir, c.Transport, c.TestName())
		}
		sd.Cells[c.Transport][c.TestName()] = c
	}

	f, err := os.Open(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	lineNo := 0
	for sc.Scan() {
		lineNo++
		var rec sweep.PointRecord
		if err := json.Unmarshal(sc.Bytes(), &rec); err != nil {
			return nil, fmt.Errorf("%s/results.jsonl line %d: %w", dir, lineNo, err)
		}
		if rec.CampaignIdentity != sd.CampaignIdentity {
			continue
		}
		key := fmt.Sprintf("%s|%s|c%d", rec.Transport, rec.TestName(), rec.Conns)
		sd.Points[key] = rec
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	for transport, cells := range sd.Cells {
		for name, cell := range cells {
			for label, conns := range map[string]int{"capacity": cell.Capacity, "break": cell.BreakConns} {
				if conns == 0 {
					continue
				}
				key := fmt.Sprintf("%s|%s|c%d", transport, name, conns)
				if _, ok := sd.Points[key]; !ok {
					return nil, fmt.Errorf("%s/capacity.json %s point %s has no active-campaign result evidence", dir, label, key)
				}
			}
		}
	}
	return sd, nil
}

// causeCode は break 原因の短縮ラベル(セル内表記用)。
func causeCode(cause string) string {
	switch {
	case cause == "":
		return ""
	case strings.Contains(cause, "staleness"):
		return "st"
	case strings.Contains(cause, "delivery_lt"):
		return "dl"
	case strings.Contains(cause, "delivery_md"):
		return "md"
	case strings.Contains(cause, "farm_limited"):
		return "farm"
	case strings.Contains(cause, "payload corruption"):
		return "corrupt"
	case strings.Contains(cause, "crash") || strings.Contains(cause, "exit_code") || strings.Contains(cause, "exited"):
		return "crash"
	case strings.Contains(cause, "invalid:") || strings.Contains(cause, "measurement_invalid"):
		return "inv"
	default:
		return "fail"
	}
}

func formatCell(c sweep.CellRecord, found bool) string {
	if !found {
		return "—"
	}
	switch {
	case c.Outcome == run.OutcomeUnsupported:
		return "UNSUPPORTED"
	case c.Outcome == run.OutcomeInconclusive:
		return "INCONCLUSIVE"
	case c.BelowRange:
		return fmt.Sprintf("<%d (%s)", c.BreakConns, causeCode(c.BreakCause))
	case c.Censored:
		// censored の内訳を区別する: farm(計測器の限界)と
		// crash(その conns で transport の client 接続が死ぬ — server の
		// quality break とは別種の transport 限界)は意味が違う
		label := "farm"
		if c.MeasurementInvalid {
			label = "invalid"
		}
		return fmt.Sprintf("≥%d (%s)", c.Capacity, label)
	case c.RangeLimited:
		return fmt.Sprintf("≥%d", c.Capacity)
	case c.Capacity == 0:
		return fmt.Sprintf("0 (%s)", causeCode(c.BreakCause))
	default:
		if code := causeCode(c.BreakCause); code != "" {
			return fmt.Sprintf("%d (%s)", c.Capacity, code)
		}
		return fmt.Sprintf("%d", c.Capacity)
	}
}

// CapacityTable は workload 行 × transport 列の markdown 表を生成する。
// onlyAnchors = true で anchor 3 セルに絞る(loss 最悪点用)。
func (sd *SweepData) CapacityTable(onlyAnchors bool) string {
	transports := orderedTransports(sd.Cells)
	known := map[string]bool{}
	for _, row := range workloadRows {
		known[row.Name] = true
	}
	var scenarioRows []string
	if !onlyAnchors {
		seen := map[string]bool{}
		for _, cells := range sd.Cells {
			for name := range cells {
				if !known[name] && !seen[name] {
					scenarioRows = append(scenarioRows, name)
					seen[name] = true
				}
			}
		}
		sort.Strings(scenarioRows)
	}
	var b strings.Builder
	rowHeader := "workload"
	if len(scenarioRows) > 0 {
		rowHeader = "scenario / workload"
	}
	b.WriteString("| " + rowHeader + " | " + strings.Join(transports, " | ") + " |\n")
	b.WriteString("|---" + strings.Repeat("|---", len(transports)) + "|\n")
	for _, row := range workloadRows {
		isAnchor := row.Anchor != "" && row.Anchor != "synthetic"
		if onlyAnchors && !isAnchor {
			continue
		}
		label := row.Name
		if isAnchor {
			label += " ⚓" + row.Anchor
		} else if row.Anchor == "synthetic" {
			label += " (synthetic)"
		}
		b.WriteString("| " + label + " |")
		for _, t := range transports {
			c, ok := sd.Cells[t][row.Name]
			b.WriteString(" " + formatCell(c, ok) + " |")
		}
		b.WriteString("\n")
	}
	for _, name := range scenarioRows {
		b.WriteString("| " + name + " |")
		for _, t := range transports {
			c, ok := sd.Cells[t][name]
			b.WriteString(" " + formatCell(c, ok) + " |")
		}
		b.WriteString("\n")
	}
	b.WriteString("\n*凡例: `N (code)` = capacity Nと最初のbreak原因(st/dl/md/crash/corrupt/fail/inv)、`≥N` = 探索上限までPASS、`≥N (farm)` = farm律速、`≥N (invalid)` = 再試行後も測定不成立。`UNSUPPORTED` / `INCONCLUSIVE`は数値capacityなし。詳細はcapacity.json/results.jsonl。*\n")
	return b.String()
}

// ClassMappingTable renders the endpoint-advertised traffic-class semantics
// from active-campaign result evidence. A transport must advertise one stable
// mapping across every point represented by the sweep.
func (sd *SweepData) ClassMappingTable() (string, error) {
	records := map[string]run.ClassMappingRecord{}
	identities := map[string]string{}
	keys := make([]string, 0, len(sd.Points))
	for key := range sd.Points {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		point := sd.Points[key]
		data, err := os.ReadFile(filepath.Join(point.RunDir, "result.json"))
		if err != nil {
			return "", fmt.Errorf("%s mapping evidence: %w", point.RunDir, err)
		}
		var result run.Result
		if err := json.Unmarshal(data, &result); err != nil {
			return "", fmt.Errorf("%s/result.json mapping evidence: %w", point.RunDir, err)
		}
		if result.Transport != "" && result.Transport != point.Transport {
			return "", fmt.Errorf("%s/result.json mapping evidence: transport=%q, want %q", point.RunDir, result.Transport, point.Transport)
		}
		identity, err := validateClassMappingEvidence(result.Treatment, point.Transport)
		if err != nil {
			return "", fmt.Errorf("%s/result.json mapping evidence: %w", point.RunDir, err)
		}
		record := result.Treatment.ClassMapping
		if previous, ok := identities[point.Transport]; ok && previous != identity {
			return "", fmt.Errorf("transport %q has inconsistent class_mapping evidence across sweep points", point.Transport)
		}
		identities[point.Transport] = identity
		records[point.Transport] = record
	}

	var b strings.Builder
	b.WriteString("| transport | loss_tolerant | must_deliver | server/client |\n")
	b.WriteString("|---|---|---|---|\n")
	for _, transport := range orderedTransports(sd.Cells) {
		record, ok := records[transport]
		if !ok {
			b.WriteString(fmt.Sprintf("| %s | unavailable | unavailable | unavailable |\n", transport))
			continue
		}
		match := "mismatch"
		if record.Match {
			match = "match"
		}
		b.WriteString(fmt.Sprintf("| %s | %s | %s | %s (`%s` / `%s`) |\n",
			transport,
			formatClassMapping(record.Server[run.ClassLossTolerant]),
			formatClassMapping(record.Server[run.ClassMustDeliver]),
			match, shortHash(record.ServerSHA256), shortHash(record.ClientSHA256)))
	}
	b.WriteString("\n*class cell: `primitive; delivery; ordering; realization`. Hashes are canonical server/client class_mapping SHA-256 prefixes.*\n")
	return b.String(), nil
}

func validateClassMappingEvidence(treatment *run.TreatmentRecord, transport string) (string, error) {
	if reasons := run.ValidateTreatmentClassMappingEvidence(treatment, transport); len(reasons) > 0 {
		return "", fmt.Errorf("%s", strings.Join(reasons, "; "))
	}
	return run.HashValue(treatment.ClassMapping), nil
}

func formatClassMapping(spec run.ClassMappingSpec) string {
	primitive := strings.ReplaceAll(spec.Primitive, "|", "\\|")
	return fmt.Sprintf("`%s; %s; %s; %s`", primitive, spec.Delivery, spec.Ordering, spec.Realization)
}

func shortHash(value string) string {
	if len(value) <= 12 {
		return value
	}
	return value[:12]
}

// AnchorVerdicts は anchor セルの絶対予算判定(capacity 点の staleness p99 を
// profiles.md の予算と比較)を生成する。probed 点からの近似であることを明記。
func (sd *SweepData) AnchorVerdicts() string {
	var b strings.Builder
	b.WriteString("| anchor | transport | capacity 点の staleness p99 | 予算 | 判定 |\n|---|---|---|---|---|\n")
	for _, row := range workloadRows {
		budget, isAnchor := anchorBudgetNS[row.Name]
		if !isAnchor {
			continue
		}
		for _, t := range orderedTransports(sd.Cells) {
			c, ok := sd.Cells[t][row.Name]
			if !ok || c.Capacity == 0 {
				continue
			}
			pt, ok := sd.Points[fmt.Sprintf("%s|%s|c%d", t, row.Name, c.Capacity)]
			if !ok {
				continue
			}
			verdict := "✓"
			switch {
			case pt.Judgment.FloorStaleNS > budget:
				// 2段判定: フロアが予算を超える組は regime の性質であって
				// transport の評価ではない(design spec「フロアと2段判定」)
				verdict = fmt.Sprintf("infeasible(フロア %dms > 予算)", pt.Judgment.FloorStaleNS/1_000_000)
			case pt.Judgment.StalenessP99 > budget:
				verdict = "✗ 予算超過"
			}
			b.WriteString(fmt.Sprintf("| %s ⚓%s | %s | %dms | %dms | %s |\n",
				row.Name, row.Anchor, t, pt.Judgment.StalenessP99/1_000_000, budget/1_000_000, verdict))
		}
	}
	b.WriteString("\n*anchor 予算判定は探索済み capacity 点での近似(平面 gate で探索した点のみ使用)。*\n")
	return b.String()
}

func orderedTransports[T any](values map[string]T) []string {
	var out []string
	seen := map[string]bool{}
	for _, name := range transportOrder {
		if _, ok := values[name]; ok {
			out = append(out, name)
			seen[name] = true
		}
	}
	var extra []string
	for name := range values {
		if !seen[name] {
			extra = append(extra, name)
		}
	}
	sort.Strings(extra)
	return append(out, extra...)
}

// ReplaceSection は md 内の <!-- generated:NAME --> ... <!-- /generated:NAME -->
// 区間の中身を置換する。マーカーが無ければエラー。
func ReplaceSection(md, name, content string) (string, error) {
	re := regexp.MustCompile(`(?s)(<!-- generated:` + regexp.QuoteMeta(name) + ` -->\n).*?(<!-- /generated:` + regexp.QuoteMeta(name) + ` -->)`)
	if !re.MatchString(md) {
		return "", fmt.Errorf("marker generated:%s not found", name)
	}
	return re.ReplaceAllString(md, "${1}"+strings.TrimSuffix(content, "\n")+"\n${2}"), nil
}

// UpdateDoc は sweep / boundary dir 群を読み、doc のマーカー区間を更新する。
// 区間名: capacity-<regime>、anchors-<regime>、boundary-<anchor>-<load>。
// doc に無い区間はスキップ(まだ散文側が用意していない区間)。
func UpdateDoc(docPath string, sweepDirs, boundaryDirs []string) error {
	raw, err := os.ReadFile(docPath)
	if err != nil {
		return err
	}
	md := string(raw)
	apply := func(sections map[string]string) error {
		for name, content := range sections {
			updated, err := ReplaceSection(md, name, content)
			if err != nil {
				return err
			}
			md = updated
		}
		return nil
	}
	seenRegimes := map[string]bool{}
	for _, dir := range sweepDirs {
		sd, err := LoadSweep(dir)
		if err != nil {
			return err
		}
		if seenRegimes[sd.Regime] {
			return fmt.Errorf("multiple sweep inputs use regime %q; aggregate blocks before report generation", sd.Regime)
		}
		seenRegimes[sd.Regime] = true
		sections := map[string]string{
			"capacity-" + sd.Regime: sd.CapacityTable(sd.Regime != "wired"),
			"anchors-" + sd.Regime:  sd.AnchorVerdicts(),
		}
		mappingSection := "mapping-" + sd.Regime
		if strings.Contains(md, "<!-- generated:"+mappingSection+" -->") {
			table, err := sd.ClassMappingTable()
			if err != nil {
				return err
			}
			sections[mappingSection] = table
		}
		if err := apply(sections); err != nil {
			return err
		}
	}
	for _, dir := range boundaryDirs {
		bd, err := LoadBoundary(dir)
		if err != nil {
			return err
		}
		sections := map[string]string{}
		for _, anchor := range bd.Anchors() {
			for _, label := range bd.LoadLabels(anchor) {
				sections["boundary-"+anchor+"-"+label] = bd.BoundaryTable(anchor, label)
			}
		}
		if err := apply(sections); err != nil {
			return err
		}
	}
	return os.WriteFile(docPath, []byte(md), 0o644)
}

// UpdateSections は任意の生成済み表群をマーカー区間へ書き込む
// (aggregate 等、この package の外で生成された表の受け口)。
func UpdateSections(docPath string, sections map[string]string) error {
	raw, err := os.ReadFile(docPath)
	if err != nil {
		return err
	}
	md := string(raw)
	for name, content := range sections {
		updated, err := ReplaceSection(md, name, content)
		if err != nil {
			return err
		}
		md = updated
	}
	return os.WriteFile(docPath, []byte(md), 0o644)
}
