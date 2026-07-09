# CLAUDE.md

## 現行方針(2026-07-10 決定・ユーザー指示)

目的: **各ライブラリの最強実装を作って横並びでバトらせる。**

- 実装ごとに「ソース徹底読み → 設計ノート → 最適化実装 → 高速確認 → ドキュメント」の
  ループを回す。1 ライブラリずつ完結させる。
- **時間のかかるベンチ(sweep / sentinel / canonical / boundary / ceiling 等)は全部あとまわし。**
  ループ内の確認は短時間 run(smoke test・低 conns・短 duration)のみで行う。
- 実時間を無駄にしない。長時間測定を回して待つ運用をしない。
  バトル(長時間ベンチ)は詰め切った実装が揃ってから一括で行う。
- この方針は specs/2026-07-08-validity-and-tuning.md の V1→V4 実行順に優先する
  (V1 ceiling sweep も後回し対象)。
- 設計ノートは `servers/<lib>/README.md` にソース行引用つきで書く。
  「この実装はライブラリが想定する最速の使い方である」を監査可能にするのが目的。
- 「最強実装」の停止条件: 追加チューニングで測定値が動かず、単一コア CPU か NIC が飽和。

## v1/v2

v1(`harness/`, `adapters/`, `cmd/rudp-benchctl`, `scripts/`)は凍結中 — 手を入れない。
新規開発は v2(`benchspec/`, `benchkit/`, `servers/`, `orchestrator/`, `calibration/`)のみ。
