package rig

import (
	"path/filepath"
	"testing"
)

// 同梱 rig 定義が常に Validate を通ることを保証する(fleet の boot gate で
// 初めて発覚した contract 違反の再発防止)。
func TestShippedRigDefinitionsAreValid(t *testing.T) {
	paths, err := filepath.Glob("../rigs/*.json")
	if err != nil || len(paths) == 0 {
		t.Fatalf("glob rigs: %v (found %d)", err, len(paths))
	}
	for _, path := range paths {
		if _, err := Load(path); err != nil {
			t.Errorf("%s: %v", path, err)
		}
	}
}
