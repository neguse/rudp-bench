# ADR-0004: reference preset と confirmatory protocol の凍結値

- Status: **Proposed（たたき台 — 全数値が owner 合意待ち）**
- Date: 2026-07-12
- 依存: ADR-0000, ADR-0001, ADR-0002
- Decision owner: project owner

## Context

ADR-0002 は方法論を定めたが、reference preset の具体値、実用上意味のある最小差、
pilot から confirmatory protocol を凍結する手順、性能以外の capability matrix を
Open Decisions として残した。本 ADR はその 4 点の提案値を固定する。
想定 production target は GameLift の Graviton インスタンス（ルート CLAUDE.md
「rig 運用」）であり、resource budget はそこから導出する。

## 1. Reference preset（v1）

現行 plan generator の実装境界（ADR-0001「Current implementation boundary」）の
内側で開始する。非対称 pub/sub、複数 room、churn を使う preset は schema 実装後の
v2 以降とし、v1 で「近い」と装わない。

### 共通

- payload は splitmix64-v1、wire compression なし
- warmup 25 s / measurement 60 s / drain 5 s（conformance 系 run は 20 s を維持）
- network regime は 3 種:
  - `lan`: delay 1 ms、loss 0%（同一リージョン内の下限確認）
  - `wan`: delay 25 ms、random loss 1%（既存 smoke と同値。国内 WAN 相当）
  - `rough`: delay 50 ms、jitter 10 ms、random loss 3%（モバイル/劣化回線相当）
- LT SLO: delivery ratio >= 0.95、staleness p99 <= 300 ms、starvation 0
- MD SLO: eventual delivery 1.0、200 ms deadline hit >= 0.95
- SLO は用途の絶対値であり、regime によって緩めない（ADR-0002）

### `authoritative-state`（clients/server を探索）

- client input: LT 30 Hz / 64 B（既存 smoke の 13 Hz は診断用。preset は
  現代のアクションゲームの入力レートに合わせる）
- server state: LT 20 Hz / 256 B/client（per-client personalized）
- 制御系: 双方向 MD 10 Hz / 64 B
- 探索単位: clients/server。screening は 2 倍刻み、confirmatory は境界 ±10%

### `room-relay`（participants/room を探索、単一 room 対称 all-to-all）

- publish: LT 20 Hz / 128 B
- 制御系: MD 10 Hz / 64 B
- 探索単位: participants/room

### Resource budget

- SUT server プロセスは **4 vCPU 相当の cgroup quota**（GameLift の
  c8g.xlarge 相当を production の代表サイズと仮定 — **owner 確認事項**）
- 補助として 1 vCPU 条件を診断列に置く（コア当たり効率の開示用）
- client farm は測定器であり、budget 外（十分性は ADR-0002 の farm gate で検証）

## 2. 実用上意味のある最小差と precision

- capacity の最小意味差（MDE）: **10%**。それ未満の候補差は「同等群」とし、
  ADR-0002 Recommendation Rule 4 の非性能基準で選ぶ
- 境界セルは ±1 点 flap する（ledger #18）ため、capacity の報告解像度は
  探索刻み（境界近傍で 5%）を下回らない
- block 統計は median を主とし、N>=3 の IQR を不確かさとして併記（ledger #15 の教訓）

## 3. Pilot から confirmatory を凍結する手順

1. pilot は 2 treatment（raw_udp + managed 代表 1 つ）× 3 scenario × `wan` regime
2. pilot で測るもの: warmup 収束（enet 系の throttle 過渡 >= 15 s を含むか）、
   block 間分散、境界の再現幅、1 block の所要時間
3. pilot 結果から次を凍結し、preset hash に含める:
   - block 反復数 N（初期提案: 3、境界 flap が IQR を超えるセルのみ 5）
   - stopping rule: 連続 N block の median 変動が MDE/2（5%）以内で停止。
     上限 5 block で未達なら `INCONCLUSIVE`
   - 前後 baseline の drift 許容幅（pilot の観測分散から設定）
4. 凍結後の変更は本 ADR の supersede を要する。pilot data は比較表に混ぜない

## 4. 性能以外の hard requirement / capability matrix

推薦の第 1 段（除外）に使う。各 treatment の開示項目:

| 項目 | 例 |
|---|---|
| platform | **arm64/Graviton 対応（target 必須）**、Windows/コンソール client |
| license | ライセンス種別、商用利用可否 |
| security | 暗号化（DTLS/QUIC/なし）、認証、DoS 耐性の設計 |
| semantics | reliable/unreliable/ordered の提供 class、max payload、fragmentation |
| 運用 | 保守状態（最終 release）、upstream 活動、脆弱性対応窓口 |
| interop | 言語 binding、プロトコル互換、NAT traversal / IPv6 |

capability は測定と独立に記録し、capacity 表と同じ場所で開示する。

## Open Questions（owner 判断待ち）

1. resource budget の代表サイズ: 4 vCPU（c8g.xlarge 相当）でよいか
2. `authoritative-state` の state payload 256 B/client は想定 workload に近いか
   （実ゲームの delta snapshot は数百 B〜数 KB まで幅がある）
3. `rough` regime を v1 に含めるか、v2 に送るか（測定時間が 1.5 倍になる）
4. echo workload（v1 バトルで見えていた接続数スケール軸）を preset に含めるか、
   diagnostics に留めるか

## Consequences

- 本 ADR の accept をもって ADR-0002 の Open Decisions を閉じ、pilot を開始できる
- preset の正規化パラメータと hash は benchspec の実装へ落とし、結果 bundle に保存する
- v1 preset は現行 topology 実装で実行可能であり、schema 拡張を前提としない
