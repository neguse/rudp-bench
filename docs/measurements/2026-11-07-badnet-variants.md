# 2026-11-07 劣悪ベンチ(既定から変えた設定)

既定の設定で不合格の理由がはっきりしたライブラリについて、ゲームで実際にそう使う設定に変えたものを、[劣悪ベンチ](../../README.md#劣悪ベンチ)の全条件で 1 回ずつ測った記録。
既定の設定の結果は [2026-11-07(ライブラリの既定の設定)](2026-11-07-badnet-local.md) と [2026-11-07(追加の 5 ライブラリ)](2026-11-07-badnet-added.md) にある。

## 測った設定

| treatment | 既定からの変更 | 理由 |
|---|---|---|
| `gns-timeout30` | `k_ESteamNetworkingConfig_TimeoutConnected` を 10 秒から 30 秒に | トンネルやエレベーター、Wi-Fi と携帯回線の切り替えで回線が十数秒切れても、接続を保ちたいゲームで使う |
| `slikenet-timeout30` | `SetTimeoutTime` を 10 秒から 30 秒に | 同上 |
| `yojimbo-timeout30` | `ClientServerConfig::timeout` を 10 秒から 30 秒に | 同上 |
| `enet-timeout30` | `enet_peer_timeout` の timeoutMinimum と timeoutMaximum を 30 秒に(既定は 5 秒と 30 秒) | 同上 |
| `litenetlib-timeout30` | `NetManager.DisconnectTimeout` を 5 秒から 30 秒に | 同上 |
| `msquic-timeout30` | `DisconnectTimeoutMs` を 16 秒から 30 秒に | 同上 |
| `quiche-drop-old-dgram` | LT の datagram を送信待ちに足す前に、まだ送っていない datagram を `quiche_conn_dgram_purge_outgoing` で捨てる | LT は最新の値が届けばよいので、送れずに残った古い値は捨ててよい(`msquic-cancel-on-blocked` と同じ) |

## 実行条件

| 項目 | 値 |
|---|---|
| commit | `6661a66`(gns・slikenet・yojimbo の timeout30 と quiche-drop-old-dgram の SUT)、`8379104`(enet・litenetlib・msquic の timeout30 の SUT)。orchestrator は、ライブラリが断った送信を判定に含めた `e60913c` 以降 |
| ライブラリ | 既定の設定の記録と同じ(GNS `a24b43d`、SLikeNet v.0.1.3、yojimbo v1.9.1、quiche 0.30.0、ENet v1.3.18、LiteNetLib 2.1.4、msquic v2.5.0-385)、Linux 6.18 |
| 実行 | `scripts/badnet/gen.py` → `sudo scripts/badnet/run.sh` → `scripts/badnet/analyze.py` |
| CPU | bench.slice(CPU 3-7,11-15) |
| 検証 | 判定に使った run はすべて `verdict=VALID` |

## 結果

| 条件 | gns-timeout30 | slikenet-timeout30 | yojimbo-timeout30 | quiche-drop-old-dgram | enet-timeout30 | litenetlib-timeout30 | msquic-timeout30 |
|---|---|---|---|---|---|---|---|
| base | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss1 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss2 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss5 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| loss10 | PASS | PASS | PASS | PASS | 境目 | FAIL C2 | PASS |
| loss20 | PASS | PASS | PASS | FAIL C3 | FAIL C2 | FAIL C2 | PASS |
| jitter30-loss2 | PASS | FAIL C3 | PASS | PASS | FAIL C3 | FAIL C3 | PASS |
| burst5x10 | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| congest1m | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C2/C3 |
| outage5 | PASS | PASS | PASS | FAIL C4 | FAIL C4 | FAIL C4 | FAIL C4 |
| outage15 | PASS | PASS | PASS | FAIL C4 | FAIL C4 | FAIL C4 | FAIL C4 |
| loss20-then-clean | PASS | PASS | PASS | PASS | PASS | PASS | PASS |

- 切断までの時間を 30 秒にした GNS・SLikeNet・yojimbo は、outage15 で切れなくなり、断が明けてから 0.2〜0.7 秒で戻った(MD 遅延の最大 15204〜15729 ms)。再送の待ち時間が倍々に伸びないので、断の後すぐに戻る。他の条件の合否は既定の設定と同じ。
- ENet・LiteNetLib・msquic は、30 秒にしても切れなくなるだけで、回線断(C4)では不合格のまま。再送の待ち時間が断の間に伸び、断が明けた後の最初の再送が遅れる(ENet は outage15 で 29 秒)。ENet と LiteNetLib は、既定では切れていたロス 20% や 5 秒の断でも切れなくなったが、MD が秒単位で遅れる(C2 / C4)。
- `quiche-drop-old-dgram` は、congest1m の下りの LT staleness p99 が 28312 ms から 590 ms に縮んだが、回線の待ち行列(約 1 秒)の分が残り不合格。ロスが多いと、送れずに捨てる LT の分だけ到達率が下がり、loss20 で不合格になった。回線断の戻りの遅れ(C4)は既定と同じ。

### 主要な数値(上り / 下り、ms)

| 条件 | 指標 | gns-timeout30 | slikenet-timeout30 | yojimbo-timeout30 | quiche-drop-old-dgram | quiche(既定) |
|---|---|---|---|---|---|---|
| outage15 | MD 遅延 max | 15204 / 15204 | 15729 / 15204 | 15204 / 15204 | 16777 / 16777 | 16777 / 16777 |
| loss20 | 下り LT 到達率 | 0.789 | 0.802 | 0.724 | 0.679 | 0.802 |
| loss20 | 下り LT staleness p99 | 197 | 205 | 213 | 311 | 786 |
| congest1m | 下り LT staleness p99 | 1376 | 655 | 819 | 590 | 28312 |
| congest1m | 下り MD 遅延 p99 | 2228 | 2359 | 1376 | 1180 | 1507 |

| 条件 | 指標 | enet-timeout30 | litenetlib-timeout30 | msquic-timeout30 |
|---|---|---|---|---|
| loss20 | MD 遅延 p99 | 5505 / 5505 | 6029 / 3408 | 492 / 492 |
| outage5 | MD 遅延 max | 7340 / 7340 | 6554 / 6554 | 5505 / 6291 |
| outage15 | MD 遅延 max | 29360 / 29360 | 16253 / 16253 | 17826 / 18874 |
| congest1m | 下り MD 遅延 p99 | 7340 | 8389 | 33554(到達率 0.875) |

### 境目の再測定

基準値の ±10% 以内で合否が変わりうるセルを測り直した。enet-timeout30 の loss10 は基準値から 11% 離れていたが、既定の ENet と合否が違ったので測り直し、合否が割れた。

| セル | 1 回目 | 2 回目以降 | 扱い |
|---|---|---|---|
| loss20 yojimbo-timeout30 | PASS(下り LT 到達率 0.724、基準 0.720) | PASS(0.726) | PASS |
| loss20 gns-timeout30 | PASS(0.789) | PASS(0.790) | PASS |
| loss20 quiche-drop-old-dgram | FAIL C3(0.679) | FAIL(0.670) | FAIL |
| outage15 gns-timeout30 / slikenet-timeout30 / yojimbo-timeout30 | PASS(最大 15204 / 15729 / 15204 ms、基準 16000 ms) | PASS(15204 / 15729 / 15204 ms) | PASS |
| outage15 quiche-drop-old-dgram | FAIL C4(16777 ms) | FAIL(16777 ms) | FAIL |
| loss5 / loss10 / loss20-then-clean の yojimbo-timeout30 と quiche-drop-old-dgram、jitter30-loss2 の yojimbo-timeout30 | PASS | PASS | PASS |
| loss10 enet-timeout30 | FAIL C2(上り MD p99 1114 ms) | PASS(590 / 950 ms) | 境目 |
| loss20 msquic-timeout30 | PASS(上り LT 到達率 0.791) | PASS(0.795) | PASS |
| outage5 litenetlib-timeout30 / msquic-timeout30 | FAIL C4(最大 6554 / 6291 ms) | FAIL(6554 / 6291 ms) | FAIL |
| outage15 litenetlib-timeout30 | FAIL C4(最大 16253 ms) | FAIL(16253 ms) | FAIL |

yojimbo-timeout30 の loss20 は 2 回とも合格したが、既定の yojimbo は 3 回とも 0.710〜0.719 で不合格だった。切断までの時間は loss20 の振る舞いに関係しない。どちらも、1024 B を超えるパケットが 2 つの断片に分かれる仕組みで、下りの LT 到達率の期待値が (1 − 0.2)(1 − 0.1) = 0.720 と基準値に一致する([追加の 5 ライブラリの記録の yojimbo の節](2026-11-07-badnet-added.md#yojimbo))。合否の差は偶然による。

enet-timeout30 の loss10 の 1 回目は、1 件の MD の再送が続けて失敗し、後続の MD が順番待ちで遅れたもの(下の ENet の節)。同じビルドで既定の ENet の loss10 も測ると、下りの MD の最大は 1769 ms で、同じ長い待ちは起きている。切断までの時間は loss10 の振る舞いに関係しない。

結果は `results-v2/badnet-20261107-{variants,near,coopfix,more,more-rerun}`(リポジトリには入れていない)。

## 不合格の仕組み

GNS・SLikeNet・yojimbo の timeout30 の不合格(congest1m、slikenet の jitter30-loss2)と、ENet・LiteNetLib・msquic の timeout30 の congest1m と jitter30-loss2 の不合格は、既定の設定と同じ仕組みによる(各ライブラリの記録の節を参照)。切断までの時間はこれらに関係しない。

### enet-timeout30

出典は `third_party/enet/`。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| outage5 / outage15 C4(最大 7340 / 29360 ms) | 再送の待ち時間は失敗ごとに 2 倍で上限がない。初期値 R(約 113 ms)から R・3R・7R・…と累積し、5 秒の断の後は 63R ≈ 7.1 秒、15 秒の断の後は 255R ≈ 28.8 秒まで次の再送が来ない。既定では 5 秒で切れていた | `protocol.c:1386`, `:1505-1506` |
| loss20 C2(MD p99 5505 ms)、loss10 境目 | 同じく再送の待ち時間の倍化。ロス 20% では 1 回の再送の失敗率は 0.36 で、6〜7 回続けて失敗すると数秒待つ。既定ではこの時点で切れていた(loss20 C1)。channel 0 は順序を守るので、後続の MD もその間待つ | `protocol.c:1386`, `:1505-1506` |

### litenetlib-timeout30

出典は LiteNetLib 2.1.4(`https://github.com/RevenantX/LiteNetLib/blob/2.1.4/LiteNetLib/`)。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| outage5 / outage15 C4(最大 6554 / 16253 ms) | 断の間は ping の返事が来ないので、1 秒ごとの経過時間が RTT の標本になる。平均 RTT は 3 秒ごとに直近の値へまとめ直されるので約 1 秒に膨らみ、再送の待ち時間(25 ms + 平均 RTT × 2.1)が約 2 秒になる。断が明けた後の最初の再送がその分遅れる。既定では 5 秒で切れていた | `LiteNetPeer.cs#L1283-L1284`, `#L1291-L1295`, `#L779-L785` |
| loss10 / loss20 C2 | 既定の LiteNetLib と同じ。ロスで ping か pong が落ちるたびに平均 RTT が跳ね、再送の待ち時間が 1〜2 秒になる | `LiteNetPeer.cs#L779-L785`, `#L1283-L1284` |

### msquic-timeout30

| 不合格 | 仕組み | 出典(`third_party/msquic/src/`) |
|---|---|---|
| outage5 / outage15 C4(最大 6291 / 18874 ms) | 既定の msquic と同じ。probe timeout は送るたびに 2 倍で上限がなく、断の後は輻輳ウィンドウを 2 パケットに落とし、断の間に溜まった datagram を先に送る。既定では 16 秒で切れていた | `core/loss_detection.c:326-331`, `core/cubic.c:307-327`, `core/send.c:1358-1378` |
| congest1m C2/C3 | 既定の msquic と同じ。datagram の送信要求が上限なく列に溜まり、MD の stream がその後ろで待つ | `core/datagram.c:350-355`, `:600-628` |

### quiche-drop-old-dgram

出典は `third_party/quiche/quiche/src/`。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| loss20 C3(下り LT 到達率 0.679 / 0.670) | `quiche_conn_dgram_purge_outgoing` は、送信待ちの datagram のうち条件に合うものを除く。ロス 20% では CUBIC の cwnd が小さくなり、送れないまま次の LT が来る。そのたびに古い LT を捨てるので、既定の quiche(送信待ちに溜めて遅れて届ける、到達率 0.802、staleness p99 786 ms)より到達率が下がり、staleness は 311 ms に縮む | `dgram.rs:95-101`, `recovery/congestion/cubic.rs:375-379` |
| congest1m C3(下り LT staleness p99 590 ms、MD p99 1180 ms) | datagram の送信待ちは伸びなくなったが、CUBIC は待ち行列が溢れて落ちるまで送る量を減らさないので、回線の待ち行列(約 1 秒)は埋まったままになる | `recovery/congestion/cubic.rs:355-399` |
| outage5 / outage15 C4 | 既定の quiche と同じ。probe timeout が倍々に伸びる | `recovery/congestion/recovery.rs:458`, `:486-488` |
