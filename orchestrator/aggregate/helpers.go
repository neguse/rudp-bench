package aggregate

import (
	"fmt"
	"sort"
)

// AggregateCapacityWithRegime は集約に加えて regime ラベルを返す。
// ブロック横断集約は同一 regime の sweep dir 群が前提(混在はエラー)。
func AggregateCapacityWithRegime(dirs []string) (map[CapacityKey]CapacityAgg, string, error) {
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		return nil, "", err
	}
	regime := ""
	for k := range aggs {
		if regime == "" {
			regime = k.Regime
		} else if k.Regime != regime {
			return nil, "", fmt.Errorf("mixed regimes in aggregate inputs: %q and %q", regime, k.Regime)
		}
	}
	return aggs, regime, nil
}

// BoundaryAnchors は出現 anchor の一覧(安定順)。
func BoundaryAnchors(aggs map[BoundaryKey]BoundaryAgg) []string {
	seen := map[string]bool{}
	var out []string
	for k := range aggs {
		if !seen[k.Anchor] {
			seen[k.Anchor] = true
			out = append(out, k.Anchor)
		}
	}
	sort.Strings(out)
	return out
}

// BoundaryLoadLabels は anchor 内の負荷ラベル一覧(floor 先頭、以降辞書順)。
func BoundaryLoadLabels(aggs map[BoundaryKey]BoundaryAgg, anchor string) []string {
	seen := map[string]bool{}
	var out []string
	for k := range aggs {
		if k.Anchor == anchor && !seen[k.LoadLabel] {
			seen[k.LoadLabel] = true
			out = append(out, k.LoadLabel)
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		if out[i] == "floor" {
			return true
		}
		if out[j] == "floor" {
			return false
		}
		return out[i] < out[j]
	})
	return out
}
