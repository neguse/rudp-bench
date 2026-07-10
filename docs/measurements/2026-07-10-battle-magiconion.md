# battle: magiconion(wired-v3)— 2026-07-10

セッション 7。tuned 実装(atomic stats・Kestrel HTTP/2 window 拡大、
commit 58688d1)の wired-v3 capacity。アンカー: raw_udp r20p128 c68 PASS
(5 セッション連続一致)。

| workload | capacity | 天井比 | break 原因 |
|---|---|---|---|
| r20p128 | **≥128**(censored) | ≥168% | farm pacing gate(attempted 0.949 < 0.99、#7 の凍結時測定「mo 天井 (128,256]」どおり) |
| r20p1000 | **97** | 129% | staleness 1441ms(**p1000 の首位**) |
| r60p200 | **81** | 176% | staleness 475ms |

## 読みどころ

- **p1000 で全 transport の首位**(97 vs enet ≥75 / lnl 72 / gns 65 /
  msquic 55)。TCP ストリームは kernel の TSO/GSO で 1000B 帯のバルクに
  強く、per-datagram syscall の UDP 勢を逆転する。「重量級 RPC フレーム
  ワークが大 payload では最速」は対戦表のハイライト
- r60p200 も 81(176%)で msquic(107)・enet(96)に次ぐ 3 位。
  旧計測の「wired ~60-68 集中(farm 律速、#7)」から大幅に伸びており、
  凍結 farm 構成 + 今日の server 側修正(stats atomic 化・h2 window)が
  効いている
- 破断は staleness の大幅超過(1.4s / 0.5s)= TCP の再送+HoL 遅延が
  quality gate を踏み抜く形。loss 0.1% でこれなので loss1 regime では
  さらに落ちるはず(TCP 系の宿題、loss1 バトルで)
- r20p128 の真値は farm 増強待ち(litenetlib と同じ TODO 11 系)

## perf(r20p1000 c97、破断点直下、duration 30s 診断 run)

.NET は `-p` attach でサンプルが取れず(2 回とも空 capture — litenetlib
セッションと同症状)、システムワイド(`-a`)で取得。**SUT と farm の
.NET プロセスが混在した表**として読む:

| % | comm / シンボル | 帰属 |
|---|---|---|
| 7.1% | .NET TP Worker / entry_SYSRETQ | syscall 復帰 |
| 5.9% | .NET TP Worker / read_hpet | **時刻取得**(#21、全 transport 共通で最大級) |
| 4.0% | .NET TP Worker / libcoreclr | .NET ランタイム |

kernel 側の構図(hpet + syscall オーバーヘッド支配)は C/C++ 勢と同じ。
adapter の managed コードは上位に出ず plateau 判定は維持。
手順ノート: **C# の perf は `-a`(system-wide)で取り、comm 混在の
caveat を付けること**(battle.md に反映)。

raw 結果: `results-v2/battle/magiconion/`(git 管理外)
