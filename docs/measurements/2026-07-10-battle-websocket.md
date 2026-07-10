# battle: websocket(wired-v3)— 2026-07-10

セッション 8(最終枠)。tuned 実装(atomic stats・index キャプチャバグ修正、
commit 58688d1)の wired-v3 capacity。アンカー: raw_udp r20p128 c68 PASS
(6 セッション連続一致)。

| workload | capacity | 天井比 | break 原因 |
|---|---|---|---|
| r20p128 | **251** | 330% | staleness 311ms(**全 transport 首位**) |
| r20p1000 | **141** | 188% | staleness 851ms(首位) |
| r60p200 | **127** | 276% | staleness 139ms(首位) |

**wired-v3(loss 0.1%)は素の WebSocket/TCP の 3 冠**。

## 読みどころ

- kernel TCP のストリーム集約(TSO/GSO、per-datagram syscall なし)が
  全 payload 帯で userspace UDP スタックを上回った。magiconion(同じ TCP)
  との差 = gRPC/h2 フレーミング層のコスト(251 vs ≥128、141 vs 97)
- **この結果の正しい読み方**: wired-v3 は loss 0.1% の「ほぼ綺麗な回線」。
  TCP の弱点(loss での HoL・再送遅延)はこの regime ではほぼ課金されない。
  RUDP 勢の存在意義は loss1 regime で測られる — そこが次の対戦表
  (battle.md TODO 9)。「loss が薄いなら TCP で十分」自体が本ベンチの
  重要な結論のひとつ
- c256 で一度 negative window margin(role=client — 追加した role 表示が
  初仕事)が出たが retry で通過し、251 は正直な破断。C# farm の pacing は
  ws では (256,512] 天井(#7)まで届いており censoring なし

## perf(r60p200 c127、破断点直下、duration 30s、system-wide)

system-wide 取得(SUT/farm の .NET comm 混在 caveat つき):

| % | comm / シンボル | 帰属 |
|---|---|---|
| 10.6% | .NET TP Worker / read_hpet | **時刻取得**(#21 — 全 6 transport の破断点で一貫して最大級) |
| 7.1% | .NET TP Worker / entry_SYSRETQ | syscall 復帰 |
| 3.0% | .NET TP Worker / _copy_to_user | kernel 受信 copy |

managed adapter コードは上位に出ず plateau 維持。#21(clocksource)が
全 transport 横断の最大の伸びしろであることが 6/6 で確定。

raw 結果: `results-v2/battle/websocket/`(git 管理外)
