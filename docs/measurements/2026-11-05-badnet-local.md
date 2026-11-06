# 2026-11-05 劣悪ベンチ(ローカル)

[劣悪ベンチ](../../README.md#劣悪ベンチ)の全条件 × 7 treatment を smolcenter で 1 回ずつ測った記録。
SUT は以前の目的に合わせた性能寄りの設定のまま測っており、ライブラリの既定の設定での結果ではない(例: GNS は送信速度の上限を既定の 256 KB/s から 256 MB/s に、msquic は切断までの時間を既定の 16 秒から 60 秒にしている。各 `servers/<lib>/README.md` 参照)。

## 実行条件

| 項目 | 値 |
|---|---|
| commit | `824848e`(orchestrator・`scripts/badnet/` はこの版。SUT は master `b9655df` と同じ) |
| 実行 | `scripts/badnet/gen.py` → `sudo scripts/badnet/run.sh` → `scripts/badnet/analyze.py` |
| CPU | bench.slice(CPU 3-7,11-15)。server は 7,15、client は残り。ゲームサーバ等は 0-2,8-10 |
| 検証 | 全 84 run が `verdict=VALID`。窓内の qdisc カウンタで実ロス率は設定値どおり(例: loss10 で 9.3–10.3%、burst5x10 で 3.9–6.6%)。`base` の MD 遅延 p50 は 51–57 ms(片道 50 ms 設定) |

## 結果

| 条件 | enet | gns | kcp | litenetlib | magiconion | msquic | websocket |
|---|---|---|---|---|---|---|---|
| base | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss1 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss2 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss5 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss10 | FAIL C2 | PASS | PASS | FAIL C2 | FAIL C2/C3 | PASS | FAIL C3 |
| loss20 | FAIL C1/C2 | PASS | PASS | FAIL C2 | FAIL C2/C3 | PASS | FAIL C2/C3 |
| jitter30-loss2 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| burst5x10 | PASS | PASS | PASS | FAIL C2 | FAIL C3 | PASS | FAIL C2/C3 |
| congest1m | FAIL C1/C2/C3 | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C2/C3 | FAIL C3 | FAIL C2/C3 |
| outage5 | FAIL C4 | PASS | 境目 | FAIL C4 | 境目 | PASS | 境目 |
| outage15 | FAIL C1/C2/C4 | PASS | FAIL C4 | FAIL C4 | FAIL C4 | FAIL C4 | FAIL C4 |
| loss20-then-clean | PASS | PASS | PASS | PASS | FAIL C4 | PASS | PASS |

GNS は混雑(congest1m)以外のすべての条件で合格した。msquic と KCP は、ロスには強いが長い回線断からの戻りが遅い。ENet・LiteNetLib・TCP 系(websocket・magiconion)は、ロス 10% 以上で reliable が秒単位で遅れる。

C1 = 勝手に切らない、C2 = reliable の遅れに上限、C3 = 混雑とロスを取り違えない、C4 = 回線が戻れば品質も戻る(基準は README の「判定」)。

### 主要な数値(上り / 下り、ms)

| 条件 | 指標 | enet | gns | kcp | litenetlib | magiconion | msquic | websocket |
|---|---|---|---|---|---|---|---|---|
| loss10 | MD 遅延 p99 | 3146 / 410 | 393 / 410 | 238 / 344 | 2228 / 1769 | 688 / 1016 | 295 / 311 | 623 / 721 |
| loss20 | MD 遅延 p99 | 6029 / 7864 | 590 / 623 | 377 / 377 | 3801 / 3015 | 2884 / 13631 | 459 / 426 | 3539 / 22020 |
| loss20 | LT staleness p99 | 205 / 279 | 147 / 205 | 156 / 205 | 147 / 205 | 2884 / 28312 | 147 / 205 | 12059 / 22020 |
| outage5 | MD 遅延 max | 7340 / 7340 | 5243 / 5243 | 6029 / 5505 | 6291 / 6291 | 5505 / 5505 | 5505 / 5767 | 5767 / 5767 |
| outage15 | MD 遅延 max | 切断 | 15204 / 15204 | 20972 / 18874 | 16253 / 16253 | 22020 / 22020 | 17826 / 17826 | 22020 / 22020 |
| congest1m | 下り MD 遅延 p99 | 29360 | 6291 | 5505 | 6554 | 28312 | 7340 | 23069 |
| congest1m | 下り LT staleness p99 | 2228 | 1114 | 983 | 1114 | 26214 | 1245 | 29360 |

遅延の最大値はヒストグラムの bin 上端で、分解能は約 6%。

### 境目の再測定

基準値の ±10% 以内だった 8 セルを測り直した(`results-v2/badnet-20261105b-rerun`)。

| セル | 1 回目 | 2 回目 | 扱い |
|---|---|---|---|
| outage5 kcp | FAIL(MD max 6029 ms) | PASS(5505 ms) | 境目 |
| outage5 magiconion | PASS(5505 ms) | FAIL(6029 ms) | 境目 |
| outage5 websocket | PASS(5767 ms) | FAIL(LT staleness max 6029 ms) | 境目 |
| outage5 litenetlib | FAIL(6291 ms) | FAIL(6291 ms) | FAIL |
| outage5 msquic | PASS(5767 ms) | PASS(5767 ms) | PASS |
| outage15 litenetlib | FAIL(16253 ms) | FAIL(16253 ms) | FAIL |
| outage15 gns | PASS(15204 ms) | PASS(15204 ms) | PASS |
| loss10 magiconion | FAIL(MD p99 1016 ms) | FAIL(1442 ms) | FAIL |

割れた 3 セルは、基準(6000 ms)をまたぐ隣り合った bin(上端 5767 ms と 6029 ms)の間で揺れている。測定の分解能の内側なので「境目」とした。

## 不合格の仕組み

### 回線断・ロスで reliable が秒単位で遅れる(C2・C4)

再送の待ち時間が失敗のたびに伸びるかどうかで分かれた。

| treatment | 再送の待ち時間 | 出典 |
|---|---|---|
| ENet | 失敗ごとに 2 倍(初期値は RTT + 4×ゆらぎ) | `third_party/enet/protocol.c:1386`, `:1506` |
| KCP | 失敗ごとに 1.5 倍(nodelay=1) | `third_party/kcp/ikcp.c:1063-1067` |
| msquic | probe timeout が 2 倍ずつ(RFC 9002) | `third_party/msquic/src/core/loss_detection.c:324` |
| websocket / magiconion | TCP の再送タイマ(RFC 6298 §5.5 の 2 倍ずつ)。1 本の TCP の上で LT も MD も順番待ちになる | — |
| LiteNetLib | 固定(25 ms + 平均 RTT × 2.1)。ただし平均 RTT が膨らむ(下記) | LiteNetLib 2.1.4 `LiteNetPeer.cs:784` |
| GNS | 固定(平滑化 RTT × 3 + 最大 ACK 遅延 + 10 ms)。倍々に伸びない | `third_party/gns/src/steamnetworkingsockets/steamnetworking_statsutils.h:1068` |

GNS だけが outage15 で断の終了後 0.2 秒以内に MD を届け切った。倍々に伸びる実装は、断の長さに比例して戻りが遅れる(outage15 で KCP +6 秒、TCP 系 +7 秒、msquic +2.8 秒)。

LiteNetLib は、次の ping を送る時点で pong が戻っていないと、ping の間隔(既定 1000 ms、`LiteNetManager.cs:118`)をそのまま RTT のサンプルにする(`LiteNetPeer.cs:1281-1283`)。
ロスで ping か pong が落ちるたびに平均 RTT が 1 秒近くに引っ張られ、再送の待ち時間が数百 ms〜2 秒に伸びる。
MD は `ReliableOrdered` なので、1 個が待たされると後続も待たされる。loss10 の MD p99 2.2 秒はこの組み合わせで説明できる(待ち時間の実測による定量確認は未実施)。

### ENet の切断(C1)

切断条件は「同じ reliable の送信が 6 回続けて失敗し、かつ最初の送信から timeoutMinimum 以上たった」か「timeoutMaximum を超えた」(`protocol.c:1374-1377`)。この実装は timeoutMinimum を 10 秒、timeoutMaximum を 60 秒にしている(`servers/enet/client.c:425`)。

- outage15: 15 秒の断が 10 秒を超えるため 8 接続すべてが切れ、断の後も戻らなかった(LT 到達率 0.25)。
- loss20: 往復の失敗率は約 36% で、6 回続けて失敗する確率は約 0.2%。待ち時間が倍々に伸びるため、6 回目は 10 秒を超えて切断に至る。
- congest1m: 1 接続が切れた。下りの待ち行列が溢れ続けて reliable の失敗が重なったためと推定しているが、確かめていない。

### 混雑で全 treatment が遅延を膨らませる(C3)

congest1m では 7 treatment すべてが不合格だった。下りの負荷(約 1.4 Mbit/s)が容量(1 Mbit/s)を超え続けると、どのライブラリも古い LT を捨てずに送り続け、待ち行列(1 秒分)を埋めた。
UDP 系(GNS・KCP・LiteNetLib・msquic)は LT の staleness p99 が 1.0〜1.2 秒で、待ち行列の 1 秒分にほぼ等しい。MD の p99 は 5.5〜7.3 秒だった。
待ち行列で RTT が 1 秒超に膨らみ、溢れて落ちた MD の再送待ちも RTT に比例して伸びる(GNS なら RTT × 3)ことで説明できるが、送信側のライブラリ内部でも溜まっている可能性は切り分けていない。TCP 系と ENet は 23〜29 秒まで膨らんだ。
送る量が容量を超え続ける状況は、ライブラリの側では解決しない。アプリが送る量を落とす必要があり、そのためには送信待ちの量がアプリから見えること(C5)が前提になる。

なお ENet は既定の throttle(混雑で unreliable を間引く)を、この実装では無効にしている(`enet_peer_throttle_configure(accel=32, decel=0)`、`servers/enet/README.md`)。既定設定の変種は測っていない。

## C5 送信待ちの量が見えるか(API の確認)

| treatment | 判定 | 根拠 |
|---|---|---|
| GNS | 見える | `GetConnectionRealTimeStatus` が送信待ちバイト数(`m_cbPendingReliable` / `m_cbPendingUnreliable`)、推定送信レート、待ち時間の見込み(`m_usecQueueTime`)を返す(`steamnetworkingtypes.h:759-803`) |
| msquic | 見える | 接続統計に輻輳ウィンドウ(`QUIC_STATISTICS_V2.SendCongestionWindow`)、stream に推奨送信バッファ量のイベント(`QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`)、datagram に送信状態のイベントがある(`msquic.h:630`, `:1332`, `:1574`) |
| KCP | 見える | `ikcp_waitsnd` が送信待ちのセグメント数を返す(`ikcp.h:391`) |
| LiteNetLib | 見える | `NetPeer.GetPacketsCountInReliableQueue` が reliable の待ち数を返す(LiteNetLib 2.1.4 の API ドキュメント) |
| ENet | 見えない | 送信待ちの量を返す API がない。`ENetPeer` の構造体は公開されており RTT やロス率は読めるが、待ち行列の量は内部のリストを数えるしかない(`enet.h` の `ENetPeer`) |
| websocket | 見えない | `SendAsync` の完了待ちで押し戻されるだけで、量は取れない |
| magiconion | 見えない | gRPC の書き込みの完了待ちで押し戻されるだけで、量は取れない |

## 未解明

- ENet の loss10 で、上りの MD p99 が下りの数倍あった。2 回測って 2 回とも再現した(上り / 下り = 3146 / 410 ms、2621 / 459 ms)。throttle と timeout の設定は client・server とも同じ(`client.c:423-425`)。server 側の ACK 送出が遅れて client の RTT ゆらぎ推定が膨らむ、という仮説は未検証。

## 測り直した経緯

最初のセッション(`results-v2/badnet-20261105`、29 run で中止)は採用していない。ARP も netem のロスを受けるため、ロス 10% で netem gate の ping 平均が 376 ms になり、SUT のパケットも ARP 待ちで止まりうる状態だった。
veth の MAC を固定して近隣エントリを恒久登録し、全条件を測り直した。
