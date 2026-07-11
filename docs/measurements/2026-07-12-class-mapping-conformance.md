# 2026-07-12 Fixed Class-Mapping Conformance（smoke rig、非昇格）

- Status: **instrument-validated smoke — Promotable=false（Doctor FAIL）**
- Executed: 2026-07-11 16:55--17:35 UTC（session 39m35s）
- Host: `smolcenter` / Linux `7.0.14-arch1-1` / amd64
- Source: clean commit `e313a33`
- Probe 定義: ADR-0002「Fixed Class-Mapping Probe」（6 case × 7 transport、
  1 conn class-exclusive echo、50 Hz / 1000 B / 1000 slot、warmup 25 s /
  measurement 20 s / drain 5 s、directional 1% random netem、
  family alpha 0.01 を 4 loss case へ Bonferroni 分割）
- Local raw bundle（gitignore 対象）:
  `results-v2/class-mapping-conformance/20260711T165558Z-2820184`
- session_identity `7251d140eb38…5901d` / plan sha256 `4a4de515dfcf…d9ce` /
  attempt ledger sha256 `6459d176f898…66d0`

このsessionが確認するのは、各transportが開示したclass mapping（LT/MD）のdelivery
semanticsがdirectional loss下で開示どおりに振る舞い、その証拠（qdisc counter、
payload検証、process record、attempt ledger）が束縛されて保存されることである。
DoctorがFAIL（clocksource=hpet、IRQ affinity交差 — home rigでは是正しない方針、
ルートCLAUDE.md「rig運用」）のためPromotable=falseであり、reference用のconformance
証跡としては**reference rig（c8g）での再実行が必要**。性能比較・capacity・推薦には
一切使用しない。

## Outcomes

session outcome **PASS**（非昇格理由は doctor FAIL のみ）。
候補6 solutionは**全36 caseがPASS**。raw_udpは環境診断transportとして
LT 3 case PASS、MD 3 caseは開示どおり`UNSUPPORTED`（候補の不合格に数えない）。

| transport | 開示 LT mapping | clean LT/MD | LT loss (missing/1000) | MD loss (missing/dup/corr) |
|---|---|---|---:|---:|
| enet | unreliable-unsequenced / best_effort | PASS / PASS | 15, 10 | 0/0/0 ×2 |
| gns | unreliable-no-nagle / best_effort | PASS / PASS | 13, 13 | 0/0/0 ×2 |
| litenetlib | unreliable / best_effort | PASS / PASS | 14, 10 | 0/0/0 ×2 |
| msquic | quic-datagram / best_effort | PASS / PASS | 8, 9 | 0/0/0 ×2 |
| magiconion | grpc-stream / reliable_fallback | PASS / PASS | 0, 0 | 0/0/0 ×2 |
| websocket | reliable-stream / reliable_fallback | PASS / PASS | 0, 0 | 0/0/0 ×2 |
| raw_udp | unreliable-udp / best_effort | PASS / UNSUPPORTED | 13, 10 | UNSUPPORTED |

LT loss 列は client-egress, server-egress の順。読み方:

- UDP 系 best_effort の LT は 1% netem に対し欠落 0.8--1.5% を観測し、開示と整合
- TCP 系 reliable_fallback（magiconion / websocket）の LT 欠落 0 は開示どおり
  （best_effort 宣言なら欠落 0 は `INCONCLUSIVE` になるが、これらは reliable を開示）
- 全 MD case は固定 drain までの欠落・重複・破損 0 かつ loss exposure 成立で PASS
- loss exposure: 全 loss case で保守的 packet trial 下限 653--662 に対し
  `p_zero_upper_bound` 0.00129--0.00140 ≤ 0.0025（Bonferroni 枠内）

## 実行上の記録

- attempt 1 で全 case 有効。再試行・dependency skip なし
- 前 2 session は無効: 1 回目（`20260711T161704Z`）は acquisition lock の
  `fs.protected_regular` バグ（`0ab30f2` で修正）、2 回目（`20260711T161958Z`）は
  **session 実行中の orchestrator 再ビルドが run_identity 束縛で検出され、
  再ビルド(01:29:59 JST)以降に開始した全 attempt が invalid**（計測器は設計どおり
  作動。運用教訓は battle.md「運用の学び」に記録）
- 2 回目 session の有効分（enet 6/6、gns 5/6 PASS）は本 session の結果と整合

## 適用限界

- 単発 acquisition（case あたり 1 有効取得、ADR-0002 の固定 protocol どおり）
- Doctor FAIL 環境のため reference conformance 証跡ではない。ADR-0002 gate の
  「loss 注入 conformance」は計測器・判定経路の検証として完了、**昇格可能な証跡は
  reference rig 受入後に同一 probe を再実行して取得する**
- ordering / primitive / realization は開示 metadata であり、この delivery probe は
  識別しない（report の limitations に明記）
