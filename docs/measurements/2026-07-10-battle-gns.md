# battle: gns(wired-v3)— 2026-07-10

セッション 4。tuned 実装(SendRate 256KB/s→256MB/s 解放・共有バッファ
broadcast・drain budget、commit f4039d9)+ 本セッションで追加した
UDP socket バッファ 4MB の wired-v3 capacity。アンカー: raw_udp r20p128
c68 PASS — [天井基準線](2026-07-10-ceiling-wired-v3.md)と整合。

| workload | capacity | 天井比 | break 原因 |
|---|---|---|---|
| r20p128 | **88** | 116% | staleness_p99 139ms > floor(正直な品質破断) |
| r20p1000 | **65** | 87% | staleness + client_stall(farm CPU 8% idle)— 下限気味に読む |
| r60p200 | **49** | 107% | staleness + client_stall 帰属併記 |

vs enet(191 / ≥75 / 96): 全 workload で enet に劣後。GNS は暗号必須
(AES-256-GCM)+ 全接続を単一 service スレッドで直列処理する構造
(servers/gns/README.md)なので想定どおりの位置。旧計測の
「broadcast ≥64 farm 打ち切り(ledger #13)」は解消され、全行が正直な
品質破断で確定した。

## セッション中の修正

- **UDP socket バッファ 4MB**(`SteamNetworkingSocketsLib::
  g_cbUDPSocketBufferSize`、既定 256KB): 初回 sweep で r60p200 が
  「farm_limited: client netns RcvbufErrors=116」で censored(capacity 32)。
  ledger #5 のシグナル発火 → 同手順で farm/server 両側を 4MB 化
  (公開 config が無く extern グローバルが唯一のノブ、describe に開示)。
  再走で censored 32 → **正直な 49**。
- 256KB 時代の結果は `results-v2/battle/gns.sockbuf256k/` に退避
  (r20p128=87 / r20p1000=65 — 4MB 版と ±1 で一致し、sockbuf は
  r60p200 の farm 側だけに効いていたことも確認できた)

## perf(r20p128 c88、破断点、診断専用 run)

部分キャプチャ(server 終了で record 途中終了)のため指標として読む:

| % | シンボル | 帰属 |
|---|---|---|
| 19.3% | read_hpet(kernel) | **時刻取得**(ledger #21 と一致) |
| 12.9% | _copy_to_user | kernel 受信 copy |
| 8.6% | entry_SYSRETQ | syscall 復帰 |
| 4.4% | qdisc_watchdog | pacing/netem timer |
| 3.4% | srso_alias_safe_ret | CPU 脆弱性緩和 |

adapter 自前コードは 2% cutoff に現れず — enet 同様 plateau 近傍。残りの
伸びしろは rig の clocksource(#21)と kernel I/O 経路側。GNS 固有の暗号
コスト(AES-GCM)も上位に見えないのは、単一 service スレッドが I/O と
timer に律速されて暗号が回りきる前に飽和している形。

raw 結果: `results-v2/battle/gns/`(git 管理外)
