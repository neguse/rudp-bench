package sweep

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/neguse/rudp-bench/internal/result"
)

// TestCompletedPoints は resume の完了判定が sweep ループの参照キー
// (runID + "/" + lib) と一致する形で、res CSV 内の (run_id, library) 行の
// 存在に基づいて作られることを確認する。
func TestCompletedPoints(t *testing.T) {
	dir := t.TempDir()

	// 2 lib 分の行を持つ res ファイル。
	res1 := filepath.Join(dir, "res_media_relay_c50_r1.csv")
	if err := result.EnsureHeader(res1, result.ResultFields); err != nil {
		t.Fatal(err)
	}
	rows := []map[string]string{
		{"run_id": "media_relay_c50_r1", "scenario_id": "enet_r0_u30_1000_50_broadcast_0_adaptive", "library": "enet", "valid": "1"},
		{"run_id": "media_relay_c50_r1", "scenario_id": "kcp_r0_u30_1000_50_broadcast_0_adaptive", "library": "kcp", "valid": "0"},
	}
	for _, r := range rows {
		if err := result.AppendRow(res1, result.ResultFields, r); err != nil {
			t.Fatal(err)
		}
	}

	// ヘッダのみ（run 開始直後に落ちた）ファイルは完了扱いにならない。
	res2 := filepath.Join(dir, "res_media_relay_c50_r2.csv")
	if err := result.EnsureHeader(res2, result.ResultFields); err != nil {
		t.Fatal(err)
	}

	// 空ファイルも完了扱いにならない。
	res3 := filepath.Join(dir, "res_media_relay_c50_r3.csv")
	if err := os.WriteFile(res3, nil, 0o644); err != nil {
		t.Fatal(err)
	}

	completed := CompletedPoints(dir)

	// sweep ループが参照するキー形式と一致していること。
	for _, want := range []string{
		CompletedKey("media_relay_c50_r1", "enet"),
		CompletedKey("media_relay_c50_r1", "kcp"),
	} {
		if !completed[want] {
			t.Errorf("completed[%q] = false, want true (map: %v)", want, completed)
		}
	}

	// 未実行 lib・空ファイルの run は完了扱いにしない。
	for _, notWant := range []string{
		CompletedKey("media_relay_c50_r1", "quiche"),
		CompletedKey("media_relay_c50_r2", "enet"),
		CompletedKey("media_relay_c50_r3", "enet"),
		// 旧実装のキー（lib なし）が生成されないこと。
		"media_relay_c50_r1",
	} {
		if completed[notWant] {
			t.Errorf("completed[%q] = true, want false", notWant)
		}
	}
}
