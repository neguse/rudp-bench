# 2026-11-07 劣悪ベンチ(ローカル、ライブラリの既定の設定)

[劣悪ベンチ](../../README.md#劣悪ベンチ)の全条件を、7 ライブラリの既定の設定と、物差しの raw_udp、以前の設定の ENet(`enet-legacy`)で 1 回ずつ測った記録。

## 実行条件

| 項目 | 値 |
|---|---|
| commit | `186e0ad`(SUT・orchestrator・`scripts/badnet/`)。msquic は libnuma のリンク順だけを直した `c1722a8`。magiconion は client の送信を FireAndForget にした `36adfba` |
| ライブラリ | ENet `third_party/enet`(v1.3.18)、KCP `third_party/kcp`(`38e0c93`)、GNS `third_party/gns`(`a24b43d`)、msquic `third_party/msquic`(v2.5.0-385)、LiteNetLib 2.1.4、MagicOnion 7.10、.NET 10、Linux 6.18 |
| 実行 | `scripts/badnet/gen.py` → `sudo scripts/badnet/run.sh` → `scripts/badnet/analyze.py` |
| CPU | bench.slice(CPU 3-7,11-15)。ゲームサーバ等は 0-2,8-10 |
| 検証 | 判定に使った run はすべて `verdict=VALID`。窓内の qdisc カウンタで実ロス率は設定値どおり(loss10 で 9.2–10.3%、loss20 で 19.0–20.7%、burst5x10 で 3.0–6.9%)。`base` の MD 遅延 p99 は KCP 以外 51–70 ms、KCP は 147–156 ms |

## 結果

| 条件 | enet | gns | kcp | litenetlib | magiconion | msquic | websocket | enet-legacy | raw_udp |
|---|---|---|---|---|---|---|---|---|---|
| base | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss1 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS | FAIL C2 |
| loss2 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS | FAIL C2 |
| loss5 | PASS | PASS | 境目 | PASS | PASS | PASS | PASS | PASS | FAIL C2 |
| loss10 | PASS | PASS | FAIL C2 | FAIL C2 | FAIL C2/C3 | PASS | FAIL C2/C3 | PASS | FAIL C2 |
| loss20 | FAIL C1/C2/C3 | PASS | FAIL C2 | FAIL C2 | FAIL C2/C3 | PASS | FAIL C2/C3 | FAIL C2 | FAIL C2 |
| jitter30-loss2 | FAIL C3 | PASS | PASS | FAIL C3 | PASS | PASS | PASS | PASS | FAIL C2 |
| burst5x10 | PASS | PASS | PASS | FAIL C2 | FAIL C3 | PASS | PASS | PASS | FAIL C2 |
| congest1m | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C2/C3 | FAIL C2/C3 | FAIL C2/C3 | FAIL C3 | FAIL C2/C3 |
| outage5 | FAIL C4 | PASS | FAIL C4 | FAIL C1/C2/C4 | PASS | FAIL C4 | PASS | FAIL C4 | FAIL C2 |
| outage15 | FAIL C1/C2/C4 | FAIL C1/C2/C4 | FAIL C4 | FAIL C1/C2/C4 | FAIL C4 | FAIL C1/C2/C4 | FAIL C4 | FAIL C1/C2/C4 | FAIL C2 |
| loss20-then-clean | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS | FAIL C2 |

- GNS は混雑(congest1m)と 15 秒の回線断(既定の 10 秒で切断)以外で合格した。msquic はロスに強いが、回線断で切断か戻りの遅れが出た。
- ENet は既定の設定でロス 10% まで合格し、ロス 20% で切れた。ゆらぎがあると LT を捨てる。
- KCP は既定の設定(高速再送なし、ロスで送信窓を 1 に戻す)では、ロス 5〜10% で MD が秒単位で遅れる。
- LiteNetLib はロス 10% で MD が秒単位で遅れ、5 秒の回線断で切れる。
- TCP の上の websocket と magiconion は、ロス 10% で LT も MD も 1 秒以上止まる。
- congest1m は全 treatment が不合格だった。
- raw_udp は再送を持たないので MD のある条件はすべて C2 で落ちる(物差しとして期待どおり)。

C1 = 勝手に切らない、C2 = reliable の遅れに上限、C3 = 混雑とロスを取り違えない、C4 = 回線が戻れば品質も戻る(基準は README の「判定」)。

### 主要な数値(上り / 下り、ms)

| 条件 | 指標 | enet | gns | kcp | litenetlib | magiconion | msquic | websocket | enet-legacy |
|---|---|---|---|---|---|---|---|---|---|
| loss10 | MD 遅延 p99 | 410 / 459 | 393 / 360 | 2621 / 3408 | 1638 / 1638 | 590 / 1114 | 328 / 295 | 557 / 1704 | 475 / 410 |
| loss20 | MD 遅延 p99 | 3146 / 3670 | 590 / 688 | 18874 / 18874 | 5505 / 3277 | 5243 / 19923 | 459 / 459 | 7078 / 19923 | 6291 / 8913 |
| loss20 | LT staleness p99 | 16253 / 16253 | 147 / 197 | 147 / 197 | 156 / 205 | 5243 / 28312 | 156 / 205 | 7078 / 19923 | 156 / 197 |
| outage5 | MD 遅延 max | 7340 / 7340 | 5243 / 5243 | 7078 / 9437 | 切断 | 5505 / 5505 | 5505 / 6291 | 5505 / 5505 | 7340 / 7340 |
| outage15 | MD 遅延 max | 切断 | 切断 | 27263 / 18874 | 切断 | 20972 / 20972 | 切断 | 20972 / 20972 | 切断 |
| congest1m | 下り MD 遅延 p99 | 3408 | 2032 | 13107 | 11010 | 30409 | 33554 | 35652 | 6554 |
| congest1m | 下り LT staleness p99 | 1114 | 1245 | 28312 | 1114 | 27263 | 11534 | 29360 | 1245 |

遅延の最大値はヒストグラムの bin 上端で、分解能は約 6%。

### 境目の再測定と INVALID の測り直し

基準値の ±10% 以内で合否が変わりうるセルと、INVALID だったセルを測り直した。

| セル | 1 回目 | 2 回目 | 扱い |
|---|---|---|---|
| loss5 kcp | PASS(下り MD p99 950 ms) | FAIL(1049 / 1901 ms) | 境目 |
| outage5 msquic | FAIL(下り MD max 6291 ms) | FAIL(6291 ms) | FAIL |
| outage5 websocket | PASS(最大 5767 ms) | PASS(5505 ms) | PASS |
| outage5 magiconion | PASS(5505 ms) | PASS(5767 ms) | PASS |
| outage5 litenetlib | FAIL(切断 8) | FAIL(切断 8) | FAIL |
| burst5x10 magiconion | FAIL C3(下り LT staleness p99 508 ms) | FAIL C2/C3(1049 ms、MD p99 1016 ms) | FAIL C3(C2 は境目) |
| loss20 websocket | INVALID(10 秒以内に 8 接続中 6 つしか張れず) | FAIL C2/C3 | FAIL |
| loss20 magiconion | INVALID(8 接続中 7 つ) | FAIL C2/C3 | FAIL |

結果は `results-v2/badnet-20261107-{enet,all,rerun,recheck}`(リポジトリには入れていない)。magiconion の 1 回目は `rerun`、2 回目は `recheck`。

## 不合格の仕組み

### ENet

| 不合格 | 仕組み | 出典(`third_party/enet/`) |
|---|---|---|
| jitter30-loss2 C3(LT 到達率 上り 0.760 / 下り 0.870) | LT は unreliable sequenced。新しい番号を受けた後に届いた古いものを捨てる。ゆらぎ 30 ms で順序が入れ替わった分が消える。送信間隔(上り 33 ms、下り 50 ms)と N(0, 30 ms) から計算した追い越されずに届く割合は、上り約 0.76、下り約 0.87 で測定と合う | `peer.c:909-911` |
| outage5 C4(MD max 7340 ms) | 再送の待ち時間は失敗ごとに 2 倍で上限がない。初期値 R(RTT + 4×ゆらぎ、測定から約 113 ms)で、R・3R・7R・15R・31R の再送が断の中で落ち、次は 63R ≈ 7.1 秒。channel 0 は順序を守るため後続の MD もそこまで待つ | `protocol.c:1386`, `:1505-1506` |
| outage15 C1(切断 8) | 切断は「6 回以上送って失敗、かつ最初の失敗から timeoutMinimum 以上」か「timeoutMaximum 超え」。既定は 5 秒 / 30 秒で、63R ≈ 7.1 秒の時点で切れる。legacy(10 秒 / 60 秒)は 127R ≈ 14.3 秒で切れた(測定の送信率から逆算した切断時刻と一致) | `enet.h:230-232`, `protocol.c:1374-1377` |
| loss20 C1(切断 1)・C2・C3 | 送信 1 回の失敗率は 1 − 0.8² = 0.36。7 回続けて失敗すると経過 64R ≈ 7.7 秒で既定の 5 秒を超えて切れる。切れた 1 接続の LT が止まり、staleness p99 が 16 秒になった。legacy は 10 秒なので 8 回連続が要り、切れずに MD が数秒遅れる(C2) | `protocol.c:1374-1377`, `:914` |
| congest1m C3 | throttle が間引くのは unreliable だけで、reliable は止めない。基準 RTT は直前 5 秒の最小で、待ち行列が溜まったままだと基準ごと膨らみ、throttle がほとんど下がらない(混雑していない上りでも LT を 4% 落とした程度)。平滑化 RTT が待ち行列分に膨らみ、溢れた MD の再送が R・3R と遅れる | `protocol.c:1522-1550`, `:903-910`, `peer.c:62-90` |

### KCP

既定の設定(`ikcp_create` のまま)では、MD の遅延の大半は KCP の送信キューでの待ち時間だった。計測窓の間の `ikcp_waitsnd` の最大(セグメント数、1 セグメント = MD 1 件 = 100 ms 分)が、MD 遅延の最大とほぼ一致する(loss10 31 / 36 → 3.0 / 3.7 秒、loss20 178 / 186 → 18.9 秒、outage15 266 / 186 → 27.3 / 18.9 秒)。

- 高速再送は既定で無効(`fastresend=0`)。失ったセグメントはすべて時間切れまで待つ(`ikcp.c:286`, `:1045`)。
- 時間切れのたびに送信窓 cwnd を 1 に戻す(`ikcp.c:1126-1132`)。輻輳制御は既定で有効(`nocwnd=0`、`ikcp.c:288`, `:1017-1021`)。
- 送信と確認応答は 100 ms ごとの `ikcp_flush` でしか出ない(`interval=100`、`ikcp.c:39`, `:280`)。
- 時間切れのたびに再送の待ち時間を 2 倍以上にし、上限がない(`nodelay=0` のとき `rto += max(rto, rx_rto)`、`ikcp.c:1062-1063`)。
- `ikcp_send` は送信キューに足すだけで、長さを制限しない(`ikcp.c:507-529`)。

これらが重なり、ロスがあると 1 秒あたりに送り出せる MD が送る量(10 件/秒)を下回ってキューが伸びる。loss20 では drain の 5 秒でも送り切れず、最終到達率が 0.75 になった。回線断(outage5 / outage15 の C4)は、倍々に伸びた再送が断の後まで来ないことと、断の間に溜まった分が cwnd 1 から少しずつしか出ないことによる。congest1m の MD(C3)も同じで、混雑した下りで時間切れが続き、キューが 133 セグメントまで伸びた。

### GNS

| 不合格 | 仕組み | 出典(`third_party/gns/src/steamnetworkingsockets/`) |
|---|---|---|
| outage15 C1(切断 8) | `TimeoutConnected` の既定は 10 秒。最後に受信してから 10 秒で切る。outage5 は 10 秒より短いので切れない | `clientlib/csteamnetworkingsockets.cpp:77`, `clientlib/steamnetworkingsockets_connections.cpp:3598-3608` |
| congest1m C3(下り LT staleness p99 1245 ms、MD p99 2032 ms) | 送信レートの上限は既定で 256 KB/s(`SendRateMin` = `SendRateMax`)に固定で、帯域推定がない。1 接続あたりの送る量(約 21 KB/s)はこの上限に届かず、全部を回線の待ち行列(約 1 秒)に入れる。落ちた MD は NACK で再送され、もう一度待ち行列を通る | `clientlib/csteamnetworkingsockets.cpp:84-85`, `clientlib/steamnetworkingsockets_snp.cpp:4196-4201` |

再送の待ち時間は「平滑化 RTT × 3 + 60 ms」で、失敗ごとに伸びない(`steamnetworking_statsutils.h:1061-1069`)。outage5 で断の 0.24 秒後に MD を届け切ったのはこのため。

### msquic

| 不合格 | 仕組み | 出典(`third_party/msquic/src/`) |
|---|---|---|
| outage15 C1(切断 8) | `DisconnectTimeout` の既定は 16 秒。最も古い未確認パケットから 16 秒たつと黙って閉じる。プローブの間隔は倍々に伸びており、断が明けてから 16 秒までの約 0.9 秒の間にプローブが当たらなかったと推測している(trace は取っていない)。ログの状態コード 0x6e(ETIMEDOUT)で、IdleTimeout(30 秒)ではないことを確かめた | `core/quicdef.h:299`, `core/loss_detection.c:1850-1866` |
| outage5 C4(下り MD max 6291 ms、2 回とも) | probe timeout は送るたびに 2 倍で上限がない。断の後、持続的な輻輳と判定して輻輳ウィンドウを 2 パケットに落とし、断の間に溜まった datagram(LT)を先に送るので、MD はその後ろで待つ | `core/loss_detection.c:326-331`, `core/cubic.c:307-327`, `core/send.c:1358-1378` |
| congest1m C2/C3(下り LT staleness p99 11.5 秒、MD 到達率 0.556) | DATAGRAM の送信要求は数の上限なしに列へ足され、時間でも捨てない(捨てるのは `QUIC_SEND_FLAG_CANCEL_ON_BLOCKED` を付けたものだけ)。datagram は stream より先に詰められる。下りの LT が回線を超えているので datagram の列が伸び続け、MD の stream はその後ろで待つ | `core/datagram.c:350-355`, `:600-628`, `core/send.c:1358-1378` |

### msquic-cancel-on-blocked

LT の datagram を `QUIC_SEND_FLAG_CANCEL_ON_BLOCKED` で送る msquic(commit `3ccc1d0`、`results-v2/badnet-20261107-msquic-cob`)。送れずに列に残った datagram は捨てられる(`core/datagram.c:600-628`)。LT は最新の値が届けばよいので、ゲームでもこう使う。他の設定は既定の msquic と同じ。

| 条件 | msquic | msquic-cancel-on-blocked |
|---|---|---|
| congest1m | FAIL C2/C3(下り LT staleness p99 11534 ms、MD 到達率 0.556、MD p99 33554 ms) | FAIL C3(下り LT staleness p99 950 ms、MD 到達率 1、MD p99 1835 ms) |
| outage5 | FAIL C4(下り MD max 6291 ms、2 回とも) | PASS(MD max 5505 ms、2 回とも) |
| outage15 | FAIL C1/C2/C4(切断 8) | FAIL C1/C2/C4(切断 8) |
| 残りの 9 条件 | PASS | PASS |

datagram が溜まらなくなり、MD の stream が datagram の後ろで待たされなくなった。congest1m の残りの遅れは回線の待ち行列(約 1 秒)と、溢れた MD の再送による。outage15 の切断は `DisconnectTimeout` によるもので、送り方では変わらない。

### LiteNetLib

出典は LiteNetLib 2.1.4(`https://github.com/RevenantX/LiteNetLib/blob/2.1.4/LiteNetLib/`)。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| outage5 / outage15 C1(切断 8) | `DisconnectTimeout` の既定は 5000 ms。何も受信しない時間がこれを超えると切る。5 秒の断でも、断の前後の最後と最初の受信の間は 5 秒をわずかに超える(2 回とも切れた) | `LiteNetManager.cs#L124`, `LiteNetPeer.cs#L1222-L1233` |
| loss10 / loss20 / burst5x10 C2(MD p99 1.6 / 5.5 / 1.2 秒) | 再送の待ち時間は 25 ms + 平均 RTT × 2.1 で、倍化はない。ただし ping の返事が来ないまま次の ping(1000 ms ごと)を送ると、経過時間(約 1 秒)を RTT の標本にする。ロスで ping か pong が落ちるたびに平均 RTT が数百 ms に跳ね、再送の待ち時間が 1〜2 秒になる。MD は `ReliableOrdered` なので後続も待つ | `LiteNetPeer.cs#L779-L785`, `#L1281-L1284`, `LiteNetManager.cs#L118` |
| jitter30-loss2 C3(LT 到達率 上り 0.754 / 下り 0.870) | ENet と同じ。`Sequenced` は最後に渡したものより新しい番号だけを渡し、ゆらぎで追い越された古いものを捨てる | `SequencedChannel.cs#L85-L103` |
| congest1m C3(下り MD p99 11 秒) | 輻輳制御も送信レートの制限もない。reliable の窓 64 が埋まり、窓に入れずに待つ MD が 75 個(7.5 秒分)まで溜まった | `ReliableChannel.cs#L294-L314` |

### websocket・magiconion(TCP)

どちらも 1 接続 = 1 本の TCP で、LT も MD も同じバイト列に載る。出典の K は `https://github.com/torvalds/linux/blob/v6.18/`。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| loss10 / loss20 / burst5x10 C2/C3 | TCP の再送の待ち時間は最小約 0.3 秒(RTT 100 ms のとき)で、失敗ごとに 2 倍。受信側は欠けたバイトが埋まるまで後ろのバイトをアプリに渡さないので、1 個のロスで LT も MD も一緒に止まる(下りの MD p99 と LT staleness p99 が同じ値になる) | K`/net/ipv4/tcp_timer.c#L677-L678`, K`/net/ipv4/tcp_input.c#L5387` |
| outage15 C4(最大 21 秒) | 断の間に再送の待ち時間が倍々に伸び、断が明けた後の次の再送が約 19〜21 秒になる。切断は tcp_retries2=15(約 925 秒)まで起きず、WebSocket・HTTP/2・MagicOnion の生存確認も既定で無効なので切れない | K`/net/ipv4/tcp_timer.c#L186-L229` |
| congest1m C2/C3(下り LT staleness p99 27〜29 秒、MD 到達率 0.53〜0.57) | TCP は混雑に合わせて送る量を落とすが、送れない分は送信側の上限のないキューに溜まる。websocket は `SendAsync` を同時に 1 つしか呼べないので、SUT が上限のないキューで 1 件ずつ送る(`servers/dotnet/BenchKit.CS/BenchSut.cs:800-814`)。magiconion は MagicOnion の server が同じ形のキューを持つ。遅れは経過時間に比例して伸び、最も遅い接続は 25〜29 秒遅れた | `https://github.com/Cysharp/MagicOnion/blob/7.10.2/src/MagicOnion.Server/Internal/QueuedResponseWriter.cs#L17` |

outage5 は、4 回目の再送(約 5 秒)が断が明けた直後に来たので合格した。再送の時刻しだいで次は約 10 秒になるため、合否はこの時刻の位置に左右される。

### 混雑(congest1m)

全 treatment が不合格だった。下りの負荷(約 1.3 Mbit/s)が容量(1 Mbit/s)を超え続けると、どのライブラリも古い LT を自分から捨てずに送り続けた。

- 自分のキューを持たずに回線の待ち行列(約 1 秒)へ出す実装(ENet、GNS、LiteNetLib)は、LT staleness p99 が 1.1〜1.2 秒で止まる。
- 自分の中に上限のないキューを持つ実装(msquic の datagram、KCP の MD、TCP 系)は、遅れが経過時間に比例して 10〜35 秒まで伸びる。

送る量が容量を超え続ける状況は、ライブラリの側では解決しない。アプリが送る量を落とすには、送信待ちの量が見えること(C5)が前提になる。

## enet-legacy の確認

`enet-legacy` は 2026-11-05 の結果と同じ設定の ENet。作り直した共通部分で、11-05 と合否が違ったのは loss10(11-05 は FAIL C2、今回 PASS)、loss20(C1/C2 → C2)、congest1m(C1/C2/C3 → C3)。
同じ日に 11-05 の SUT のコード(`251761c`)をビルドして測ると、今回の `enet-legacy` と同じ合否になった。11-05 との差は作り直しによるものではない(原因は切り分けていない)。
11-05 の未解明だった loss10 の上下差(上り MD p99 が下りの数倍)は、今回は再現しなかった(上り / 下り 475 / 410 ms)。

## C5 送信待ちの量が見えるか(API の確認)

| treatment | 判定 | 根拠 |
|---|---|---|
| GNS | 見える | `GetConnectionRealTimeStatus` が送信待ちバイト数(`m_cbPendingReliable` / `m_cbPendingUnreliable`)と待ち時間の見込み(`m_usecQueueTime`)を返す(`include/steam/steamnetworkingtypes.h:761-803`)。推定送信レートの欄もあるが、既定では固定の 256 KB/s を返す。いずれも GNS の中の待ちで、回線の待ち行列は含まない |
| msquic | 一部見える | 輻輳ウィンドウ(`QUIC_STATISTICS_V2.SendCongestionWindow`、`src/inc/msquic.h:630`)と stream の推奨送信バッファ量(`QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`、`:1574`)は取れる。datagram の送信待ちの量を返す API はなく、アプリが `QUIC_DATAGRAM_SEND_SENT` の通知を数える必要がある |
| KCP | 見える | `ikcp_waitsnd` が送信待ちのセグメント数を返す(`ikcp.h:390-391`) |
| LiteNetLib | 一部見える | `NetPeer.GetPacketsCountInReliableQueue` は reliable の窓に入っていない数だけを返す(`NetPeer.cs#L191-L204`)。窓が埋まるまでは 0〜1 のまま。推定帯域はない |
| ENet | 見えない | 送信待ちの量を返す関数がない。`ENetPeer` の構造体は公開されており、内部のリスト(`outgoingCommands` など、`enet.h:317-318`)を数えれば件数は出る |
| websocket | 見えない | `WebSocket` の公開メンバーに送信待ちの量も帯域もない。下のソケットを自分で用意すれば Linux の `TCP_INFO` でカーネル内の分だけは読める |
| magiconion | 見えない | MagicOnion と gRPC の公開 API にない。キューは `internal` / `private` |

## 未解明

- loss20 で websocket と magiconion が 10 秒以内に全接続を張れなかった(8 本中 6 本と 7 本)。TCP の SYN の再送間隔(Linux 6.18 では最初の 4 回は 1 秒おき)と upgrade の往復を、ロスが独立に起きるとして数えた見積もりでは、8 本中 2 本が 10 秒を超える確率は 0.2% 以下で、観測と合わない。
- congest1m で KCP と raw_udp だけ、下りの LT の 8 つの流れのうち 1 つが計測窓の間に 1 件も届かなかった(LT staleness p99 28 秒)。どちらも LT を server が接続順に続けて `sendto` している。満杯の待ち行列で毎回後ろの接続が落ちると推測しているが、どの接続かは記録しておらず確かめていない。
- ENet の loss20 で、ロスが独立に起きるとした切断の期待値(既定 8 接続中約 4.6 本、legacy 約 2 本)より、実測(1 本と 0 本)が少ない。
