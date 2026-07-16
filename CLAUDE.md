# CLAUDE.md

## 現行方針(2026-07-10 決定・ユーザー指示)

目的: **各ライブラリの最強実装を作って横並びでバトらせる。**

- 実装ごとに「ソース徹底読み → 設計ノート → 最適化実装 → 高速確認 → ドキュメント」の
  ループを回す。1 ライブラリずつ完結させる。
- **時間のかかるベンチ(sweep / sentinel / canonical / boundary / ceiling 等)は全部あとまわし。**
  ループ内の確認は短時間 run(smoke test・低 conns・短 duration)のみで行う。
- 実時間を無駄にしない。長時間測定を回して待つ運用をしない。
  バトル(長時間ベンチ)は詰め切った実装が揃ってから一括で行う。
- この方針は docs/log/2026-07-08-validity-and-tuning.md の V1→V4 実行順に優先する
  (V1 ceiling sweep も後回し対象)。
- 設計ノートは `servers/<lib>/README.md` にソース行引用つきで書く。
  「この実装はライブラリが想定する最速の使い方である」を監査可能にするのが目的。
- 「最強実装」の停止条件: 追加チューニングで測定値が動かず、単一コア CPU か NIC が飽和。

## バトル(wired 測定)の運用ルール(2026-07-10 ユーザー指示)

- 1 セッション = 「raw_udp アンカー + 1 ライブラリ」の隔離チャンク。一括で 1 日級を回さない。
- **開始前に「何時間で何をアウトプットするか」を合意する。**
- **想定より伸びたら打ち切って原因を調査する**(延長して回し続けない)。
- セッション冒頭の raw_udp アンカーが前セッション比で乖離したら停止して調査。
- 対戦表・手順・学び・TODO のハブは `docs/battle.md`。セッション終了ごとに更新する。
- **意思決定は「目的 → 方法論 → 具体手順」の順に ADR(`docs/adr/`)で合意してから動く。**
  accepted な ADR は巻き返さない(変更は supersede する新 ADR)。マトリクス外の測定・
  枠外の実験を場当たりで始めない。

## rig 運用(2026-07-12 決定・ユーザー指示)

- smolcenter(現 PC)は 24 時間ゲームサーバ同居のため **smoke・計測器開発専用**。
  doctor FAIL(clocksource=hpet 等)は容認するが、その値は reference へ昇格させない。
- **信頼できる数値(reference campaign)は専用の外部環境で取る。**
- 想定 production ターゲットは **Amazon GameLift の ARM(Graviton)インスタンス、
  ap-northeast-1 で使える最新世代 = c8g(Graviton4)**(2026-07-12 ユーザー表明。
  GameLift の第8世代対応は 2026-03 発表、EC2 c8g の東京提供は 2025-03 から)。
- 予算: 外部環境に月 5 万円まで(2026-07-12 ユーザー表明)。
- reference rig は **仮想化 c8g の spot fleet を campaign 単位で起動**
  (2026-07-16 決定、[ADR-0005](docs/adr/0005-reference-fleet.md))。サイズは
  A/A 実験で凍結。全 run spot、on-demand fallback なし。中断 cell は打ち切り
  時刻まで requeue し、残った穴は原因調査の対象。metal は perf 診断専用に降格。
  fleet fingerprint・1h campaign protocol の詳細は ADR-0005 が正。
- rig のリージョンは target(ap-northeast-1)に合わせず、**安価な US リージョン
  (us-west-2 等)でよい**(2026-07-12 ユーザー承認。ベンチは veth/netns 完結で
  リージョンは測定に影響しない。silicon 一致が要件)。
- clocksource 是正(ledger #21)は smolcenter では行わない。x86 の tsc/hpet 問題は
  Graviton(virt 含め `arch_sys_counter`)には存在せず、reference fleet の受入 gate
  で担保する(ADR-0005)。
- host fingerprint が異なる rig の値は同じ比較に集約しない(ADR-0002 のとおり)。

## v1/v2

v1(`harness/`, `adapters/`, `cmd/rudp-benchctl`, `scripts/`)は凍結中 — 手を入れない。
新規開発は v2(`benchspec/`, `benchkit/`, `servers/`, `orchestrator/`, `calibration/`)のみ。
