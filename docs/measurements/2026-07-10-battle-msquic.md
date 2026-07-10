# battle: msquic(wired-v3)— 2026-07-10

セッション 5。tuned 実装(atomic stats・共有 SendBuf・SendBuffering off・
DisconnectTimeout 60s、commit f2b00ce)の wired-v3 capacity。アンカー:
raw_udp r20p128 c68 PASS(3 セッション連続一致)。

| workload | capacity | 天井比 | break 原因 |
|---|---|---|---|
| r20p128 | **158** | 208% | **崖型**: delivery 0.000(両 class)、staleness histogram 空 |
| r20p1000 | **55** | 73% | 崖型 + staleness 5.2s、client_stall(farm CPU 2% idle) |
| r60p200 | **107** | 233% | 崖型: delivery 0.000 |

## 読みどころ

- **r60p200 で enet を逆転**(107 vs 96)— UDP GSO/sendmmsg バッチと
  partition(コア)方向スケールの効果。r20p128 では enet の MTU merge が
  勝る(191 vs 158)
- **破断が全 workload で崖型**: 直前の conns まで delivery ~1.0、+1 で 0.000。
  QUIC datagram は cwnd 逼迫で「落ちず無限にキューされる」意味論
  (`send.h:153-158`)なので、飽和を跨いだ瞬間に全 datagram が run 窓の外へ
  押し出される。enet/gns の緩やかな staleness 破断と対照的で、運用上は
  「msquic は容量内では最良品質、容量超過で即全損」という特性として読む
- r20p1000(暗号コストが payload 比例 + merge 不能)では最下位。
  msquic は「小 payload 多接続」に強く「大 payload」に弱い
- **旧 #17 の crash(exit 1)は全点で非発現** — DisconnectTimeout 60s 化の
  wired 検証完了。破断点でも全 proc exit 0 の VALID 測定

## perf(r60p200 c107、破断点直下、診断専用 run)

部分キャプチャ(server 終了で record 途中終了)のため指標として読む:

| % | シンボル | 帰属 |
|---|---|---|
| 15.6% | read_hpet(kernel) | **時刻取得**(ledger #21、3 transport 連続で最大項) |
| 7.1% | entry_SYSRETQ | syscall 復帰 |
| 6.0% | _copy_to_user | kernel 受信 copy |
| 4.9% | QuicDatagramQueueSend(library) | datagram enqueue(fanout の per-target lock + op alloc) |

adapter 自前コードは 2% cutoff 未満 — plateau 近傍。userspace 側の最大項は
msquic 自身の datagram enqueue 機構で、これはライブラリの構造コスト。

raw 結果: `results-v2/battle/msquic/`(git 管理外)
