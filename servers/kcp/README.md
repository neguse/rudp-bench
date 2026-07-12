# kcp — KCP(skywind3000)server/client(tuned)

benchspec/README.md 準拠の実装対。「ライブラリが想定する最速の使い方」を
ソース読解と v1 tuning sweep(2026-05-31、`adapters/kcp` の設計メモ)に
基づいて設計した tune-to-plateau 版。

## class mapping

KCP は reliable-ordered な ARQ プロトコル**のみ**を提供し、unreliable channel
も UDP socket も持たない(`third_party/kcp/README.md`)。実運用の KCP 採用
(kcp2k / Mirror 等)は「同一 UDP socket 上で reliable=KCP + unreliable=素
datagram」の 2 channel 構成が通例で、本 treatment もそれに合わせる。

| class | channel | wire |
|---|---|---|
| loss-tolerant | raw datagram(KCP バイパス) | `0x00` + benchspec payload |
| must-deliver | KCP ARQ(conv = origin_id) | `0x01` + KCP frame |

- 先頭 1 byte が channel discriminator。LT の最大 payload は 65506B
  (datagram 上限 − prefix)
- KCP frame は自前の conv(先頭 4B)を持ち、server は送信元アドレスで
  接続を引いた上で conv 照合する(`ikcp_getconv`)
- class と channel の対応は wire 契約: raw channel に MD、KCP channel に LT が
  来たら `invalid_payload` に数える

## tuning(全て upstream 公式 API、`--describe` に開示)

v1 sweep の結論を引き継ぐ:

- `ikcp_nodelay(1, 5, 2, 1)` — nodelay(min RTO 30ms)、fastresend=2、
  **nocwnd=1**(TCP Reno AIMD の cwnd を無効化。ベンチは自前 rate 制御)
- **interval 5ms**: `ikcp_nodelay` は interval<10ms を 10ms に clamp する
  (`third_party/kcp/ikcp.c:1258-1259`)ため、公開 field `kcp->interval` を
  直接上書きして vendored source を変えずに 5ms を通す。sweep では 5ms が
  高 conn 側の HoL stall 回復で勝った
- `ikcp_wndsize(256, 256)`: 既定 snd_wnd=32(`ikcp.c:35`)は小さすぎる。
  384 以上は ARQ の buffering latency を足すだけで悪化(sweep)
- `ikcp_setmtu(1400)` = upstream 既定(`ikcp.c:37`)
- socket buffer 256KB: 1MB は bufferbloat で delivery 0.78→0.52 に悪化
  (v1 の A/B 実測)
- **送信 backpressure**: `ikcp_waitsnd() >= 1024`(wnd の 4 倍)で MD 送信を
  受け付けない。KCP の送信 queue は無制限で、溢れさせると RTO spurious
  retransmit が雪崩れる。弾いた slot は未送信として metrics の分母に残る
  (eventual delivery の会計は正直なまま)

## イベントループ設計

- server は raw_udp server と同一の single-socket / single-thread 構成。
  peer ごとに ikcp instance を持ち、`ikcp_check` が期限切れの分だけ
  `ikcp_update` を回す(service slice = 5ms = interval)
- 受信は drain budget 512/呼び出しで bound(battle.md「運用の学び」の
  server 版 drain budget)。KCP frame は `ikcp_input` 後に完成 message を
  `ikcp_recv` で引き切る
- ikcp の output callback user pointer は peer 配列の realloc を跨いで安定で
  ある必要があるため、server は fd+addr を持つ heap ctx を peer と別に持つ

## 制約・特性

- MD は KCP の ordered stream に載るため、loss 時は message 単位の
  HoL blocking が deadline hit に響く(reliable-ordered の宿命)
- KCP 内部クロックは 32-bit ms(`kcp_common.h:37-39` の `kcp_now_ms`、
  差分は `_itimediff` の signed cast)で、half-space 2^31ms ≈ 24.8 日で
  折り返す。sn の折り返しは 2^32 packets でベンチの rate では実質届かない。
  いずれも本ベンチの run 長では問題にならない
- dead_link=20(`ikcp.c:41`): 同一 segment の再送 20 回で state=-1。
  本実装は切断扱いにせず送信継続する(ベンチ中の削除はロスターを壊すため)

## ramp モード

orchestrator の ramp(単一 run 内の接続数段階増加。契約は
`benchspec/README.md`「ramp mode」)に対応済み(`../ramp.h` を使用)。
`BENCH_RAMP_*` が揃うと phase ごとに接続を追加して per-phase snapshot
(`$BENCH_METRICS_OUT.ramp-*.json`)を書き、最終の cumulative metrics は
書かない。
