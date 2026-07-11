# 2026-07-12 KCP Fixed Class-Mapping Conformance（smoke rig、非昇格）

- Status: **instrument-validated smoke — Promotable=false（Doctor FAIL、smoke rig 方針）**
- Executed: 2026-07-11 19:39--19:45 UTC（session 5m59s）
- Host: `smolcenter` / Linux `7.0.14-arch1-1` / amd64
- Source: commit `d95853a` + servers/kcp 追加分（dirty tree での adapter 開発 run。
  probe 定義・判定経路は [6 solution record](2026-07-12-class-mapping-conformance.md) と同一）
- Local raw bundle（gitignore 対象）:
  `results-v2/class-mapping-conformance/20260711T193908Z-3012116`
- session_identity `4b276fd759d5499b…`

新規 KCP adapter（`servers/kcp` — LT=raw datagram sidechannel / MD=KCP ARQ の
2 channel treatment）の開示 class mapping を、ADR-0002 の fixed probe
（6 case、directional 1% loss）で検査した単独 session。

## Outcomes（6/6 PASS）

| case | outcome | delivered/1000 | missing | dup | corruption |
|---|---|---:|---:|---:|---:|
| clean LT | PASS | 1000 | 0 | 0 | 0 |
| clean MD | PASS | 1000 | 0 | 0 | 0 |
| LT client-egress loss | PASS | 993 | 7 | 0 | 0 |
| LT server-egress loss | PASS | 990 | 10 | 0 | 0 |
| MD client-egress loss | PASS | 1000 | 0 | 0 | 0 |
| MD server-egress loss | PASS | 1000 | 0 | 0 | 0 |

- LT（best_effort 開示）: 1% netem に対し欠落 0.7-1.0% を観測し開示と整合
- MD（reliable 開示、KCP ARQ）: 固定 drain までに欠落・重複・破損 0 で開示と整合
- loss exposure は全 loss case で Bonferroni 枠内に成立

## 適用限界

- Doctor FAIL（hpet — smoke rig で是正しない方針）のため Promotable=false。
  昇格可能な証跡は reference rig で全 8 transport の probe を再実行して取得する
- adapter 開発中の dirty tree run であり、正式 conformance 記録ではない
- 性能値・capacity・推薦には使用しない
