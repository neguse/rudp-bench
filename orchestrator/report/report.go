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
	"strings"

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
	Regime string
	Cells  map[string]map[string]sweep.CellRecord // transport → workload → cell
	Points map[string]sweep.PointRecord           // pointKey → record
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
		Regime: capFile.Cells[0].Regime,
		Cells:  map[string]map[string]sweep.CellRecord{},
		Points: map[string]sweep.PointRecord{},
	}
	for _, c := range capFile.Cells {
		if sd.Cells[c.Transport] == nil {
			sd.Cells[c.Transport] = map[string]sweep.CellRecord{}
		}
		sd.Cells[c.Transport][c.Workload] = c
	}

	f, err := os.Open(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		var rec sweep.PointRecord
		if json.Unmarshal(sc.Bytes(), &rec) != nil {
			continue
		}
		key := fmt.Sprintf("%s|%s|c%d", rec.Transport, rec.Workload, rec.Conns)
		sd.Points[key] = rec
	}
	return sd, sc.Err()
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
	default:
		return "inv"
	}
}

func formatCell(c sweep.CellRecord, found bool) string {
	if !found {
		return "—"
	}
	switch {
	case c.Censored:
		return fmt.Sprintf("≥%d (farm)", c.Capacity)
	case c.RangeLimited:
		return fmt.Sprintf("≥%d", c.Capacity)
	case c.Capacity == 0:
		return fmt.Sprintf("0 (%s)", causeCode(c.BreakCause))
	default:
		return fmt.Sprintf("%d (%s)", c.Capacity, causeCode(c.BreakCause))
	}
}

// CapacityTable は workload 行 × transport 列の markdown 表を生成する。
// onlyAnchors = true で anchor 3 セルに絞る(loss 最悪点用)。
func (sd *SweepData) CapacityTable(onlyAnchors bool) string {
	var transports []string
	for _, t := range transportOrder {
		if _, ok := sd.Cells[t]; ok {
			transports = append(transports, t)
		}
	}
	var b strings.Builder
	b.WriteString("| workload | " + strings.Join(transports, " | ") + " |\n")
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
	b.WriteString("\n*凡例: `N (code)` = capacity N・break 原因(st=staleness / dl=delivery_lt / md=delivery_md / inv=validity)、`≥N` = 探索上限まで OK、`≥N (farm)` = farm 律速で打ち切り(server の break ではない)。詳細は sweep 出力の capacity.json / results.jsonl。*\n")
	return b.String()
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
		for _, t := range transportOrder {
			c, ok := sd.Cells[t][row.Name]
			if !ok || c.Capacity == 0 {
				continue
			}
			pt, ok := sd.Points[fmt.Sprintf("%s|%s|c%d", t, row.Name, c.Capacity)]
			if !ok {
				continue
			}
			verdict := "✓"
			if pt.Judgment.StalenessP99 > budget {
				verdict = "✗ 予算超過"
			}
			b.WriteString(fmt.Sprintf("| %s ⚓%s | %s | %dms | %dms | %s |\n",
				row.Name, row.Anchor, t, pt.Judgment.StalenessP99/1_000_000, budget/1_000_000, verdict))
		}
	}
	b.WriteString("\n*anchor 予算判定は探索済み capacity 点での近似(平面 gate で探索した点のみ使用)。*\n")
	return b.String()
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

// UpdateDoc は sweep dir 群を読み、doc のマーカー区間を更新する。
// 区間名は regime から決まる: capacity-<regime>、anchors-<regime>。
// doc に無い区間はスキップ(まだ散文側が用意していない regime)。
func UpdateDoc(docPath string, sweepDirs []string) error {
	raw, err := os.ReadFile(docPath)
	if err != nil {
		return err
	}
	md := string(raw)
	for _, dir := range sweepDirs {
		sd, err := LoadSweep(dir)
		if err != nil {
			return err
		}
		sections := map[string]string{
			"capacity-" + sd.Regime: sd.CapacityTable(sd.Regime != "wired"),
			"anchors-" + sd.Regime:  sd.AnchorVerdicts(),
		}
		for name, content := range sections {
			updated, err := ReplaceSection(md, name, content)
			if err != nil {
				fmt.Fprintf(os.Stderr, "[report] skip %s: %v\n", name, err)
				continue
			}
			md = updated
		}
	}
	return os.WriteFile(docPath, []byte(md), 0o644)
}
