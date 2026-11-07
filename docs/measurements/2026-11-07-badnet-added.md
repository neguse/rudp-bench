# 2026-11-07 劣悪ベンチ(追加の 5 ライブラリ、ライブラリの既定の設定)

[劣悪ベンチ](../../README.md#劣悪ベンチ)の全条件を、yojimbo・SLikeNet・quiche・lsquic・coop_rudp の既定の設定で 1 回ずつ測った記録。
先に測った 7 ライブラリの結果は [2026-11-07(ライブラリの既定の設定)](2026-11-07-badnet-local.md) にある。

## 実行条件

| 項目 | 値 |
|---|---|
| commit | `2ea5b12`(SUT・orchestrator・`scripts/badnet/`)。yojimbo の outage5 / outage15 と loss20 の 3 回目は、切れた client の扱いを直した `963cfc2`。coop_rudp の loss20 / congest1m の 3 回目は、ライブラリが断った送信を判定に含めた `e60913c` |
| ライブラリ | yojimbo `third_party/yojimbo`(v1.9.1)、SLikeNet `third_party/slikenet`(v.0.1.3)、quiche `third_party/quiche`(0.30.0)、lsquic `third_party/lsquic`(4.11.1、BoringSSL 0.20250807.0)、coop_rudp(このリポジトリの `coop_rudp/`)、Linux 6.18 |
| 実行 | `scripts/badnet/gen.py` → `sudo scripts/badnet/run.sh` → `scripts/badnet/analyze.py` |
| CPU | bench.slice(CPU 3-7,11-15)。ゲームサーバ等は 0-2,8-10 |
| 検証 | 判定に使った run はすべて `verdict=VALID`。窓内の qdisc カウンタで実ロス率は設定値どおり(loss10 で 9.1–10.6%、loss20 で 19.3–20.6%、burst5x10 で 4.2–5.9%)。`base` の MD 遅延 p99 は 51–61 ms |

## 結果

| 条件 | yojimbo | slikenet | quiche | lsquic | coop_rudp |
|---|---|---|---|---|---|
| base | PASS | PASS | PASS | PASS | PASS |
| loss1 | PASS | PASS | PASS | PASS | PASS |
| loss2 | PASS | PASS | PASS | PASS | PASS |
| loss5 | PASS | PASS | PASS | PASS | FAIL C3 |
| loss10 | PASS | PASS | PASS | PASS | FAIL C3 |
| loss20 | FAIL C3 | PASS | FAIL C3 | PASS | FAIL C2/C3 |
| jitter30-loss2 | PASS | FAIL C3 | PASS | PASS | FAIL C3 |
| burst5x10 | PASS | PASS | PASS | PASS | PASS |
| congest1m | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C2/C3 |
| outage5 | PASS | PASS | FAIL C4 | FAIL C4 | INVALID |
| outage15 | FAIL C1/C2/C4 | FAIL C1/C2/C4 | FAIL C4 | FAIL C4 | PASS |
| loss20-then-clean | PASS | PASS | PASS | PASS | PASS |

- ロスにはどれも強く、MD 遅延 p99 は loss20 でも 0.3〜0.7 秒に収まった。
- yojimbo と SLikeNet は再送の待ち時間が倍々に伸びないので、5 秒の断から断の長さ + 0.25 秒以内に戻る。どちらも 10 秒受信がないと切るので、15 秒の断で切れる。
- quiche と lsquic は切れないが、再送の待ち時間が倍々に伸びるので、断が明けてから戻るまでに約 2〜11 秒かかる。
- coop_rudp は、ロスで送信率を下げ、古い LT を捨てずに待たせるので、下りの LT が数秒〜20 秒古くなる。送信待ちが溢れると送信を断るので、loss20 と congest1m では下りの MD も届き切らない(C2)。
- congest1m は全 treatment が不合格だった。

C1 = 勝手に切らない、C2 = reliable の遅れに上限、C3 = 混雑とロスを取り違えない、C4 = 回線が戻れば品質も戻る(基準は README の「判定」)。

### 主要な数値(上り / 下り、ms)

| 条件 | 指標 | yojimbo | slikenet | quiche | lsquic | coop_rudp |
|---|---|---|---|---|---|---|
| loss10 | MD 遅延 p99 | 254 / 262 | 328 / 328 | 254 / 360 | 393 / 426 | 254 / 442 |
| loss10 | LT staleness p99 | 115 / 180 | 123 / 156 | 119 / 295 | 115 / 147 | 127 / 19923 |
| loss20 | MD 遅延 p99 | 279 / 393 | 590 / 590 | 426 / 557 | 688 / 721 | 459 / 688(到達率 1 / 0.840) |
| loss20 | LT staleness p99 | 147 / 238 | 156 / 197 | 147 / 786 | 156 / 197 | 156 / 29360 |
| outage5 | MD 遅延 max | 5243 / 5243 | 5243 / 5243 | 6816 / 7078 | 6816 / 6816 | INVALID |
| outage15 | MD 遅延 max | 切断 | 切断 | 16777 / 16777 | 26214 / 26214 | 15729 / 15729 |
| congest1m | 下り MD 遅延 p99 | 1442 | 2753 | 1507 | 32506 | 344(到達率 0.402) |
| congest1m | 下り LT staleness p99 | 786 | 655 | 28312 | 918 | 30409(到達率 0) |

遅延の最大値はヒストグラムの bin 上端で、分解能は約 6%。

### 境目の再測定と INVALID の測り直し

基準値の ±10% 以内で合否が変わりうるセルと、INVALID だったセルを測り直した。

| セル | 1 回目 | 2 回目以降 | 扱い |
|---|---|---|---|
| loss20 yojimbo | FAIL C3(下り LT 到達率 0.715、基準 0.720) | FAIL(0.710)、FAIL(0.719) | FAIL |
| outage15 quiche | FAIL C4(最大 16777 ms、基準 16000 ms) | FAIL(16777 ms) | FAIL |
| outage15 yojimbo | INVALID(client が segfault) | INVALID(同じ)。SUT を直して FAIL C1/C2/C4(切断 8) | FAIL |
| outage5 yojimbo | PASS(5243 ms) | SUT を直して PASS(5243 ms) | PASS |
| congest1m coop_rudp | INVALID(送れた割合 0.824) | INVALID(0.778)。断った送信を判定に含めて FAIL C2/C3(断った 3777 件) | FAIL |
| loss20 coop_rudp | INVALID(送れた割合 0.968) | INVALID(0.951)。断った送信を判定に含めて FAIL C2/C3(断った 808 件) | FAIL |
| outage5 coop_rudp | INVALID(client の UDP 受信バッファあふれ 3 件) | INVALID(11 件) | INVALID |
| loss5 / loss10 / loss20-then-clean yojimbo | PASS(下り LT 到達率 0.930 / 0.856 / 0.862、基準 0.855 / 0.810 / 0.810) | PASS(0.924 / 0.855 / 0.857) | PASS |
| jitter30-loss2 lsquic | PASS(上り LT 到達率 0.969、基準 0.882) | PASS(0.972) | PASS |
| outage15 coop_rudp | PASS(最大 15729 ms、基準 16000 ms) | PASS(15729 ms) | PASS |

jitter30-loss2 の slikenet と coop_rudp は、下りの LT 到達率(0.872 / 0.875)が基準 0.882 に近いが、上り(0.757 / 0.758)が基準から 14% 離れて不合格なので、測り直していない。

yojimbo の client の SUT は、切れた `Client` に `ReceiveMessage` を呼んでいた。yojimbo は切れたときに `Connection` を解放するので(`source/yojimbo_client.cpp:136-148`)、そこで segfault した。直した `963cfc2` で、切断を挟まない loss20 の合否は変わらない。

coop_rudp の loss20 と congest1m の INVALID は、ライブラリが送信を断った(`rudp_send` が `RUDP_SEND_QUEUE_FULL` を返した)分が 1% を超えたことによる。断った分を届かなかったものとして判定に含めるよう README を改め(`e60913c`)、3 回目で判定した。仕組みは下の coop_rudp の節に書く。

結果は `results-v2/badnet-20261107-added{,-rerun,-yojimbo,-yojimbo2}`、`results-v2/badnet-20261107-refused`、`results-v2/badnet-20261107-near`(リポジトリには入れていない)。

## 不合格の仕組み

### yojimbo

出典は `third_party/yojimbo/`。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| loss20 C3(下り LT 到達率 0.715 / 0.710 / 0.719、基準 0.720) | 1024 B を超えるパケットは 1024 B ごとの断片に分けて送り、断片が 1 つでも落ちるとパケットごと失う。下りの LT(1000 B)だけのパケットは 1024 B に収まるが、MD(64 B)と同じパケットに入ると 1024 B を超えて 2 つに分かれる。MD は 100 ms ごと、LT は 50 ms ごとに同じ時刻から送るので、LT の半分が MD と同じパケットに入る。下りの LT が届く割合は (1 − p)(1 − p/2) になり、loss1〜loss20 で 0.985 / 0.970 / 0.926 / 0.855 / 0.720 と測定(0.983 / 0.971 / 0.930 / 0.856 / 0.715)と合う。loss20 の期待値が基準値と同じなので、実際のロス率のわずかな差で合否が決まる | `include/yojimbo_config.h:204`, `:251-252`, `reliable/reliable.c:792-805` |
| outage15 C1(切断 8) | 既定の timeout は 10 秒。最後に受信してから 10 秒で server も client も切る | `include/yojimbo_config.h:49`, `netcode/netcode.c:3356-3361`, `:4915-4918` |
| congest1m C3(下り LT staleness p99 786 ms、MD p99 1442 ms) | 輻輳制御がなく、毎回作ったパケットをそのままソケットへ出す。送信の速さと確認された速さは測っているが、送る判断には使わない。回線の待ち行列(約 1 秒)が溢れて LT が落ち、MD は 100 ms ごとの再送で待ち行列を何度も通る | `source/yojimbo_server.cpp:128-147`, `include/yojimbo_config.h:164`, `source/yojimbo_reliable_ordered_channel.cpp:307` |

reliable の再送の間隔は 100 ms に固定で、失敗ごとに伸びない(`include/yojimbo_config.h:164`、`source/yojimbo_reliable_ordered_channel.cpp:307`)。outage5 では断が明けてから 0.25 秒以内に戻った。

### SLikeNet

出典は `third_party/slikenet/Source/`。輻輳制御は既定の sliding window(`include/slikenet/defines.h:152-154`)。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| jitter30-loss2 C3(LT 到達率 上り 0.757 / 下り 0.872) | ENet と同じ。`UNRELIABLE_SEQUENCED` は、それまでに受けた最大の番号より古いものを捨てる。ゆらぎ 30 ms で順序が入れ替わった分が消える | `src/ReliabilityLayer.cpp:1306`, `:1350-1356` |
| outage15 C1(切断 8) | 既定の timeout は 10 秒。再送待ちがあり、最後に受信してから 10 秒たつと切る | `src/RakPeer.cpp:246-250`, `src/ReliabilityLayer.cpp:1812-1820`, `:3441-3446` |
| congest1m C3(下り LT staleness p99 655 ms、MD p99 2753 ms) | 送信の窓が数えるのは reliable だけで、LT は窓に関係なく出る。下りの LT だけで約 1.34 Mbit/s あり、回線の待ち行列が溢れ続ける。SLikeNet の中には LT が溜まっていない(計測窓の送信待ちの最大は 1170 B)。落ちた MD は再送のたびに待ち行列を通り直し、`RELIABLE_ORDERED` なので後続の MD も待つ | `src/CCRakNetSlidingWindow.cpp:87-90`, `src/ReliabilityLayer.cpp:2099` |

再送の待ち時間は「2 × RTT + 4 × ゆらぎ + 30 ms」で、上限 2 秒、失敗の回数に関係しない(`src/CCRakNetSlidingWindow.cpp:293-318`)。outage5 では断が明けてから 0.25 秒以内に戻った。

### quiche

出典は `third_party/quiche/quiche/src/`。輻輳制御は既定の CUBIC(`lib.rs:654`)。SUT は datagram の送信待ちを 1000 個にしている(`servers/quiche/quiche.c`)。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| outage5 / outage15 C4(最大 7078 / 16777 ms) | probe timeout は送るたびに 2 倍で、上限がない。0 に戻るのは ACK を受けたときだけ。断が明けても、次の probe が出るまで両方向とも止まる。断の長さを超える遅れは、断が明けたのが倍々の間隔のどこかで決まる | `recovery/congestion/recovery.rs:458`, `:486-488`, `:745`, `:912-914` |
| congest1m C3(下り LT staleness p99 28312 ms、到達率 0.440) | datagram の送信待ちは古いものから送り、古さで捨てない。CUBIC で 8 接続に分けた取り分(1 接続あたり約 14〜15 個/秒)が生成(20 個/秒)を下回り、送信待ちが伸び続ける | `dgram.rs:81-89` |
| loss20 C3(下り LT staleness p99 786 ms) | ロスのたびに cwnd を 0.7 倍にし、下限は 2 パケット。下りの LT(1 接続あたり約 21 KB/s)は 20% のロスで cwnd に押さえられる時間ができ、その間 datagram の送信待ちが伸びる(cwnd の推移は記録していない)。上りは 64 B なので押さえられない | `recovery/congestion/cubic.rs:63`, `:375-379`, `recovery/mod.rs:91` |

congest1m の MD(p99 1507 ms)は、datagram と交互にパケットへ詰められるので送信待ちの後ろには並ばず、回線の待ち行列(約 1 秒)と、溢れた分の再送で遅れる。

### lsquic

出典は `third_party/lsquic/`。輻輳制御は既定の BBRv1(`include/lsquic.h:443-444`)。lsquic は送るものを溜めず、送れるときに datagram を 1 つずつ取りに来るので、SUT が LT と MD を上限のない列で持つ(`servers/lsquic/lsquic.c` の先頭)。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| outage5 / outage15 C4(最大 6816 / 26214 ms) | ACK が途絶えると TLP を 2 回送ったあと RTO になる。RTO は max(srtt + 4 × ゆらぎ, 200 ms) で、続くたびに 2 倍(指数の上限 10、間隔の上限 60 秒)。srtt 約 100 ms で再送の時刻は断の開始から 0.2, 0.4, 0.6, 1.0, 1.8, 3.4, 6.6, 13.0, 25.8 秒。5 秒の断の後は 6.6 秒、15 秒の断の後は 25.8 秒で、測定と合う。LT は断の間 SUT の列で待ち、断の後にまとめて届く(到達率 0.984) | `src/liblsquic/lsquic_send_ctl.c:66-68`, `:345-360`, `:541-555` |
| congest1m C3(下り MD p99 32506 ms) | MD は計測窓の間ほぼ届かず、負荷が止まった約 1.8 秒後にまとめて届いた(到着の間隔の中央値 2 µs)。1 本の stream の先頭が欠けたまま、後ろが止まっていた。BBR は 10 秒間 RTT がそれより低い値にならないと PROBE_RTT に入り、cwnd を 4 パケット(5840 B)にする。抜けるには送信中の量がそれ以下に下がる必要がある。ところが ACK のパケットは cwnd を見ずに作られ、datagram はその空きに入るので、LT が出続けて送信中の量が下がらない。失った packet の再送は cwnd が許すときしか出ないので止まる。100 ms ごとに接続の状態を記録するビルドで測ると、server の接続 0 は cwnd 5840 のまま 17 秒間、再送の数が 31 から増えず、その間に新しいパケットを約 500 個送り、失ったパケットは 124 から 302 に増えた。SUT の MD の列は 0〜1 件で、MD は SUT では待っていない(結果は `results-v2/dbg-20261107b/lsquic-congest`) | `src/liblsquic/lsquic_bbr.c:54`, `:83`, `:786-833`, `src/liblsquic/lsquic_full_conn_ietf.c:1932-1940`, `src/liblsquic/lsquic_send_ctl.c:2513-2518`, `:1669-1670` |
| congest1m C3(下り LT staleness p99 918 ms、到達率 0.671) | 下りの LT の遅延は 524〜741 ms で時間とともに伸びず、SUT の列には溜まっていない。回線の待ち行列が満杯のまま、溢れた分が落ちた | 同上 |

### coop_rudp

出典はこのリポジトリの `coop_rudp/rudp.c`(`2ea5b12` の時点)。
ここに書いた不合格は直した。直した後の結果は [2026-11-07(直した coop_rudp)](2026-11-07-badnet-coop-rudp.md) にある。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| loss5 / loss10 C3(下り LT staleness p99 6291 / 19923 ms、loss10 の到達率 0.475) | 送信率の上限は初めは制限なしで、reliable を載せたパケットを失うたびに半分にする(1 RTT に 1 回)。増やすのは ACK された reliable パケット 1 個につき 9600 bit/s だけ。増減を決めるのは MD(10 個/秒)だけで、増やす速さは最大で約 96 kbit/s 毎秒。下りの需要(1 接続あたり約 170 kbit/s)を下回る時間が続き、LT は古いものから送られ、期限を付けないと捨てられないので古いまま届く。計測窓の server の送信待ちの最大を LT の生成の速さ(20 KB/s)で割ると、loss5 で 6.3 秒、loss10 で 20.8 秒になり、staleness と合う。上りの需要(約 27 kbit/s)は率の下限(32〜64 kbit/s)より小さいので絞られない | `rudp.c:1179`, `:1752-1767`, `:791-794`, `:1667-1673` |
| loss20 / congest1m C2/C3(下りの MD 到達率 0.840 / 0.402、LT staleness p99 29 / 30 秒) | 上と同じ仕組みで server の送信待ちが伸び、全接続で共有するメッセージの枠(既定 4096 個)を使い切ると、`rudp_send` が `RUDP_SEND_QUEUE_FULL` を返して LT も MD も断る | `rudp.c:11`, `:1552-1555` |
| jitter30-loss2 C3(LT 到達率 上り 0.758 / 下り 0.875) | ENet と同じ。`RUDP_UNRELIABLE_SEQUENCED` は最後に渡したものより新しくない番号を捨てる | `rudp.c:2357-2363` |

outage5 の INVALID(2 回とも)は client の UDP 受信バッファのあふれによる。断の間に server が溜めた下りの LT を、断が明けた直後にまとめて送り出す。送信率の上限は初めは制限なしで(`rudp.c:1179`)、断の間の時間切れで数回半分にしても制限にならない。10 ms ごとに送った量を記録するビルドで測ると、断が明けた直後の 30 ms に約 600 KB を送り、client の受信バッファ(既定の約 208 KB)が溢れた(結果は `results-v2/dbg-20261107b/coop-outage5`)。
outage15 は合格した。再送の待ち時間は倍々に伸びる(上限 64 倍)が、パケットを 1 つでも受信すると 0 に戻り、新しい MD を送るときに次の再送の時刻を前倒しするので、断が明けて約 1 RTT で戻る(`rudp.c:756-780`、`:2612-2614`、`:1852-1853`)。

### 混雑(congest1m)

追加の 5 つも全部が不合格だった。先の 7 ライブラリと同じく、古い LT を自分から捨てる実装はなかった。

- 自分のキューを持たずに回線の待ち行列(約 1 秒)へ出す実装(yojimbo、SLikeNet、lsquic の LT)は、下りの LT staleness p99 が 0.7〜0.9 秒で止まる。
- 古さで捨てない datagram の送信待ちを持つ quiche は、遅れが経過時間に比例して伸びる(28 秒)。
- coop_rudp は送信待ちが溢れて送信を断り、下りの LT はほぼ届かず、MD も 4 割しか届かない。

## C5 送信待ちの量が見えるか(API の確認)

| treatment | 判定 | 根拠 |
|---|---|---|
| SLikeNet | 見える | `GetStatistics` の `RakNetStatistics` に、優先度ごとの送信待ちのバイト数(`bytesInSendBuffer[]`)と再送待ちのバイト数(`bytesInResendBuffer`)がある(`Source/include/slikenet/statistics.h:87-99`)。推定帯域の欄(`BPSLimitByCongestionControl`)は、既定の sliding window では常に 0 を返す(`Source/src/CCRakNetSlidingWindow.cpp:359-362`) |
| quiche | 見える(stream を除く) | datagram の送信待ちの数とバイト数(`quiche_conn_dgram_send_queue_len` / `_byte_size`、`include/quiche.h:816-819`)と、`quiche_conn_path_stats` の `cwnd`・`delivery_rate`(`:765`, `:783`)が取れる。stream の送信待ちの量を返す関数はない |
| lsquic | 見える(推定帯域) | `lsquic_conn_get_info` の `lci_cwnd`・`lci_bw_estimate`・`lci_pacing_rate`(`include/lsquic.h:2241-2260`)。送信待ちの量を返す API はないが、lsquic は送るものを溜めないので、送信待ちはアプリが持つ |
| coop_rudp | 見える | `rudp_get_status` が送信待ち・送信中・再送待ちのバイト数と送信率の上限を返す(`coop_rudp/rudp.h:99-118`)。送信率の上限は測った帯域ではなく、ロスで半分・ACK で加算するだけの値 |
| yojimbo | 一部見える | 送信待ちは送れるかどうかの真偽(`CanSendMessage`)だけ。`GetNetworkInfo` の `NetworkInfo` に送った速さと確認された速さ(kbit/s)、RTT、ロス率がある(`include/yojimbo_network_info.h:37-53`)。どれも測った値で、経路の容量の推定ではない |
