# 高性能通信ライブラリの実装・ベンチマークチェックリスト

対象読者: **通信ライブラリの実装者**と**ベンチマークアダプタの実装者**。
14ライブラリのソースコード（adapter + third_party 内部）を精読した調査結果に基づく。
`§X.Y` は [`dev-notes.md`](dev-notes.md) の事例。

---

## 1. ソケットと OS

### 1.1 バッファサイズ

- [ ] `SO_RCVBUF` / `SO_SNDBUF` を**明示設定**しているか。OS 既定値（Linux ~208KB）では高スループットに不足し、カーネルがパケットをサイレントにドロップする
- [ ] 設定後に `getsockopt` で**実値を確認**しているか。`net.core.rmem_max` / `wmem_max` の sysctl 上限を超えた値はサイレントにクランプされる。**14ライブラリ中、設定後に確認しているものは 0**
- [ ] **送受信で非対称**になっていないか。raknet/slikenet は SO_RCVBUF=256KB だが SO_SNDBUF=**16KB** のみ（`RakNetSocket2_Berkley.cpp:39,54`）。高スループット送信のボトルネック
- [ ] バッファを大きくしすぎて**バッファブロート**を起こしていないか。kcp は 256KB→1MB で偽 RTO 再送が増加し delivery が 0.78→0.52 に悪化
- [ ] ライブラリが内部で**極端な値を要求**していないか。msquic は SO_RCVBUF に INT32_MAX（2.1GB）を要求するが SO_SNDBUF は未設定（`datapath_epoll.c:702-709`）

<details><summary>14ライブラリのソケットバッファ設定一覧</summary>

| lib | SO_RCVBUF | SO_SNDBUF | 設定箇所 | 設定後確認 |
|-----|-----------|-----------|---------|-----------|
| raw_udp | 256KB | 256KB | adapter:55-58 | なし |
| mini_rudp | 256KB | 256KB | adapter:95-96 | なし |
| coop_rudp | 4MB | 4MB | adapter:85-86 | なし |
| apex_rudp | 4MB | 4MB | adapter:146-147（`APEX_RCVBUF_KB` で変更可） | なし |
| enet | 256KB | 256KB | host.c:65-66 + adapter:208-209（`ENET_RCVBUF_KB` で変更可） | なし |
| kcp | 256KB | 256KB | adapter:124-125（`KCP_RCVBUF_KB` で変更可） | なし |
| raknet | 256KB | **16KB** | RakNetSocket2_Berkley.cpp:39,54 | assert のみ |
| slikenet | 256KB | **16KB** | RakNetSocket2_Berkley.cpp:39,54 | assert のみ |
| udt4 | 65KB(UDP層) | 65KB(UDP層) | channel.cpp:152-153。UDT 層: recv 12MB | なし |
| yojimbo | 4MB | 4MB | netcode.c:55-58（`#define` 固定） | なし |
| gns | 4MB(adapter) / 256KB(lib既定) | 同左 | lowlevel.cpp:2065-2073 + adapter:75 | なし |
| msquic | **INT32_MAX** | **未設定** | datapath_epoll.c:702-709 | なし |
| quiche | 256KB | 256KB | adapter:66-67 | なし |
| lsquic | 256KB | 256KB | adapter:228-230 | なし |
| litenetlib | 1MB | 1MB | LiteNetManager.Socket.cs:405-406 | なし |

</details>

### 1.2 I/O システムコール

- [ ] 高 PPS で `sendto`/`recvfrom` の **1 パケット 1 syscall が律速**になっていないか
- [ ] mini_rudp は reliable パケットごとに**個別 10byte ACK を sendto** する。高 PPS で syscall 数が 2 倍になる。ACK のピギーバック（データパケットに相乗り）を検討すべき
- [ ] バッチ I/O（`sendmmsg`/`recvmmsg`）の導入効果は大きい。coop_rudp/apex_rudp が最高スループットを出せる理由の一つ

<details><summary>14ライブラリの syscall パス一覧</summary>

| lib | send syscall | recv syscall | batch size | 備考 |
|-----|-------------|-------------|-----------|------|
| raw_udp | sendto | recvfrom | 1 | — |
| mini_rudp | sendto | recvfrom | 1 | ACK も個別 sendto（10byte/pkt） |
| coop_rudp | **sendmmsg** | **recvmmsg** | send:256 / recv:64 | 同一 peer 宛を物理バッチ化 |
| apex_rudp | **sendmmsg**/sendto | **recvmmsg** | send:256 / recv:64 | 3 経路: inline / batch / async TX |
| enet | sendmsg | recvmsg | 1（iovec で ACK+data を 1 peer 分結合） | peer 単位で coalesce |
| kcp | sendto | recvfrom | 1 | output callback 経由 |
| raknet | sendto | recvfrom | 1 | recv は blocking recv thread |
| slikenet | sendto | recvfrom | 1 | 同上 |
| udt4 | sendmsg（iovec） | recvmsg | 1 | UDT epoll 経由 |
| yojimbo | sendto | recvfrom | 1 | netcode 層で直接 syscall |
| gns | sendto/sendmsg | recvmsg/recvfrom | 1 | epoll_wait + DrainSocket |
| msquic | **sendmmsg**/sendmsg(GSO) | **recvmmsg**(GRO) | 可変 | GSO/GRO 対応 |
| quiche | **sendmmsg** | **recvmmsg** | 64 | adapter 側で batch |
| lsquic | **sendmmsg** | **recvmmsg** | send:64(×最大1024pkt/tick) / recv:64 | engine がパケット収集→batch 送信 |
| litenetlib | .NET SendTo→sendto | .NET ReceiveFrom→recvfrom | 1 | packet merging で小パケット結合 |

</details>

### 1.3 fd とリソース上限

- [ ] conn ごとに socket を作る設計で **fd 上限**に引っかからないか
  - per-conn socket: msquic, raknet/slikenet（per-conn RakPeerInterface）, litenetlib（per-conn NetManager）, raw_udp(client)
  - 単一 fd 多重化: mini_rudp, coop_rudp, apex_rudp, enet, kcp, gns, quiche, lsquic, yojimbo
- [ ] サンドボックスやコンテナ環境では**対話シェルと fd 上限が異なる**（§1.7: systemd-run 既定 1024）

### 1.4 MTU とフラグメンテーション

- [ ] ペイロード + ヘッダが **Path MTU** 内に収まるか
- [ ] フラグメンテーション実装がある場合、**ロスした fragment の扱い**を確認しているか
  - kcp は out-of-order fragment 再組立を**サポートしない**（rcv_buf→rcv_queue は in-order のみ）。multi-fragment message で HoL blocking が発生する
  - raknet/slikenet は fragmented unreliable message を**自動的に reliable に昇格**する。大きな unreliable メッセージを送れない設計

<details><summary>14ライブラリの MTU とフラグメンテーション一覧</summary>

| lib | MTU/payload | 最大 fragment 数 | OoO 再組立 | 備考 |
|-----|-----------|----------------|-----------|------|
| raw_udp | 65507B | なし | — | UDP 生パケット |
| mini_rudp | 65497B | **なし** | — | 分割なし |
| coop_rudp | 1200B | 256 | あり（256-bit bitmap） | — |
| apex_rudp | 65486B | **なし** | — | 分割なし |
| enet | 1392B | 1,048,576 | あり（bitmap） | ~32MB まで |
| kcp | 1400B(MSS=1376) | 127 | **なし（in-order のみ）** | multi-frag で HoL |
| raknet | ~576B(min MTU) | 2^32(理論) / 65536(同時) | あり | **unreliable→reliable 自動昇格** |
| slikenet | ~576B(min MTU) | 同上 | あり | 同上 |
| udt4 | ~1472B(MSS) | 8192+(バッファ) | あり(DGRAM) | STREAM mode は透過 |
| yojimbo | 1024B(fragment) | packet:16 / block:256 | あり | block=250ms resend |
| gns | ~1232B | ~14(unreliable) / 無制限(reliable stream) | あり | unreliable は segment 超過で破棄 |
| msquic | QUIC MTU | N/A(stream) | あり | QUIC stream segmentation |
| quiche | QUIC MTU | N/A(stream) | あり（BTreeMap by offset） | datagram: ~1200B 上限 |
| lsquic | QUIC MTU | N/A(stream) | あり | datagram: ~1200B 上限 |
| litenetlib | MTU−header | 65535 | あり（Fragments[] array） | — |

</details>

---

## 2. 再送と信頼性

### 2.1 RTO 計算

- [ ] RTO が**適応的**（RTT 測定に基づく）か**固定**か把握しているか
  - 固定 RTO は実装が簡単だが、RTT が RTO より長い経路で偽再送が頻発し、RTT が短い経路で不必要に遅延する
  - coop_rudp/apex_rudp/yojimbo は SRTT を測定しているが **RTO 計算には使わない**（固定 100ms）。coop_rudp の SRTT は status 表示のみ
  - mini_rudp は RTT を**一切測定しない**。固定 50ms RTO で、RTT>50ms の経路では偽再送が爆発する
- [ ] RTO の**バックオフ**（再送ごとに RTO を増加）を実装しているか。バックオフなしの固定 RTO は輻輳時に再送洪水を起こす
  - kcp: nodelay=0 で 2×、nodelay=1 で 1.5×、nodelay≥2 で +RTO/2。coop_rudp/apex_rudp/yojimbo/mini_rudp: **バックオフなし**

<details><summary>14ライブラリの RTO 詳細</summary>

| lib | RTO 方式 | 初期値 | 最小 | 最大 | バックオフ |
|-----|---------|-------|------|------|----------|
| enet | Jacobson/Karels（SRTT+4×RTTVAR） | SRTT=0 時 peer default | なし | なし | 2× exponential |
| kcp | Jacobson/Karels | 200ms | 100ms(normal) / 30ms(nodelay) | 60s | 2×/1.5×/+RTO/2 |
| coop_rudp | **固定** | 100ms | — | — | **なし** |
| apex_rudp | **固定** | 100ms | — | — | **なし** |
| mini_rudp | **固定（RTT 未測定）** | 50ms | — | — | **なし** |
| yojimbo | **固定** | 100ms | — | — | **なし** |
| gns | 3×SRTT+60ms | 1s(ping 未知時) | — | — | NACK ベースで RTO 依存小 |
| msquic | SRTT+4×RTTVAR+MaxAckDelay（QUIC PTO） | ~250ms | 1ms粒度 | — | 2^n exponential |
| quiche | SRTT+max(4×RTTVAR,1ms)（RFC 9002 PTO） | ~333ms | 1ms粒度 | 2^20×PTO | 2^n exponential |
| lsquic | (SRTT+4×RTTVAR)×2^min(n,10) | 500ms | 200ms | 60s | 2×、最大 10 回 |
| udt4 | EXP_COUNT×(RTT+4×RTTVar)+SYN | 100ms | 300ms | — | EXP_COUNT 乗算 |
| raknet | 2×SRTT+4×RTTVAR+30ms | 2s | — | 2s | なし |
| slikenet | 同上 | 同上 | — | 同上 | なし |
| litenetlib | **25ms+AvgRTT×2.1**（Jacobson 非準拠） | 27ms | — | — | **なし** |

</details>

### 2.2 Fast Retransmit と ACK

- [ ] **Fast Retransmit** を実装しているか。RTO 待ちだけだとレイテンシ tail が跳ねる
- [ ] ACK が**ピギーバック**（データパケットに相乗り）されるか、**専用パケット**か
  - lsquic は QUIC 実装で唯一 **ACK を別データグラムで送る**。他の QUIC 実装はデータにピギーバック
  - mini_rudp: reliable パケットごとに 10byte の ACK 専用パケット。ピギーバックなし
- [ ] ACK の**遅延**はどの程度か。遅延 ACK はスループットを上げるがレイテンシを増やす
  - apex_rudp: 2ms delayed ACK。msquic: 25ms（2 パケット受信で即時）。lsquic: max(1ms, min(max_ack_delay, srtt/4))

<details><summary>14ライブラリの Fast Retransmit と ACK 形式</summary>

| lib | Fast Retransmit | ACK 形式 | ACK サイズ | 遅延 | ピギーバック |
|-----|----------------|---------|----------|------|------------|
| enet | なし | 個別/selective per command | 8byte/ACK | なし（即時） | あり |
| kcp | あり（dup ACK=2、`fastlimit=5`） | 個別+cumulative UNA | 24byte/segment | interval(5ms) まで | あり（UNA 全パケット） |
| coop_rudp | あり（3pkt gap、SACK bitmap） | cumulative+64-bit SACK bitmap | 12byte ACK 部 | なし | あり（全パケット） |
| apex_rudp | なし | 64-bit SACK bitmap | 12byte(ack4+bits8) | 2ms delayed | あり |
| mini_rudp | なし | **個別 per-packet** | **10byte 専用パケット** | なし | **なし** |
| yojimbo | なし | 32-bit bitmap | 4-9byte(compressed) | なし | あり |
| gns | あり（NACK、3ms delay） | range-based NACK+ACK blocks | 5+byte variable | 50ms max | あり |
| msquic | あり（3pkt+RACK） | range-based QUIC ACK frame | 5-30byte variable | 25ms / 2pkt 即時 | あり |
| quiche | あり（3pkt→最大20、+time-based） | range-based（SmallVec<4>） | 5+byte variable | 25ms | あり |
| lsquic | あり（3pkt + early retransmit srtt/4） | range-based（最大256 ranges） | 4-7byte/range | max(1ms,min(ack_delay,srtt/4)) | **なし（別 datagram）** |
| udt4 | なし（NAK ベース） | cumulative+別 NAK | 32-40byte(full ACK+RTT/BW) | 10ms timer or 64pkt lite | なし（別 control packet） |
| raknet | なし（NAK ベース） | range-based selective | 4-7byte/range | ~10ms(SYN) | なし（別 datagram） |
| slikenet | 同上 | 同上 | 同上 | 同上 | 同上 |
| litenetlib | なし | bitmap/bitmask（64pkt window） | 14byte | 15ms(update interval) | なし（別送信） |

</details>

### 2.3 再送上限とコネクション死活

- [ ] 再送の**上限**（回数 or 時間）を設けているか。無制限再送は障害時にリソースを消費し続ける
  - **coop_rudp, apex_rudp, mini_rudp は再送上限なし・死活検知なし**。crashed peer に対して無限に再送し続け、リソースがリークする
  - kcp は `dead_link=20` でコネクション異常を設定するが、**adapter がその状態フラグを一切確認していない**。ライブラリの安全装置が無視されている
  - yojimbo は netcode 層の keepalive（10Hz、timeout 10s）で死活検知する。enet は reliable PING（500ms間隔）+ timeout chain（5-30s）

<details><summary>14ライブラリの再送上限と死活検知</summary>

| lib | 再送上限 | 死活検知 | keepalive | timeout |
|-----|---------|---------|-----------|---------|
| enet | ~5-6 回（指数 backoff） | reliable command timeout chain | 500ms PING | 5-30s |
| kcp | dead_link=20/segment | `kcp->state=-1`（**adapter 未参照**） | **なし** | **なし** |
| coop_rudp | **無制限** | **なし** | **なし** | **なし** |
| apex_rudp | **無制限** | **なし** | **なし** | **なし** |
| mini_rudp | **無制限** | **なし（`is_connected()`常にtrue）** | **なし** | **なし** |
| yojimbo | 無制限（message level） | netcode keepalive timeout | 100ms(10Hz) | 10s |
| gns | 無制限 | TimeoutConnected | 10s(active)/60s(idle) | 10s |
| msquic | 無制限(PTO backoff) | DisconnectTimeout | 無効(既定) | idle:30s, disconnect:16s |
| quiche | 無制限(PTO backoff 2^20 まで) | idle timeout | **なし** | 30s |
| lsquic | 無制限(backoff 2^10 まで) | idle + no-progress | 15s PING | idle:30s, handshake:10s |
| udt4 | 無制限 | EXP_COUNT>16 AND elapsed>5s | EXP timer 時 | ~5s+ |
| raknet | 無制限 | AckTimeout | timeoutTime/2 PING | 10s(release)/30s(debug) |
| slikenet | 同上 | 同上 | 同上 | 同上 |
| litenetlib | 無制限 | DisconnectTimeout | 1000ms Ping/Pong | 5s |

</details>

---

## 3. 輻輳制御

- [ ] 輻輳制御の**アルゴリズムと設定**を把握しているか。ベンチマーク用に adapter がライブラリ既定を上書きしている場合、その影響を理解しているか
- [ ] **ランダムロス環境**で CC が過度にレート抑制しないか（§1.9: UDT4 CUDTCC→BenchCCC、msquic Cubic→BBR）
- [ ] CC を無効化している場合、**ネットワークを溢れさせるリスク**を認識しているか
  - kcp adapter: `nocwnd=1` で CC 完全無効化。snd_wnd=256 segments まで無制限に送信
  - udt4 adapter: BenchCCC で全 CC コールバックを空実装に置換。固定 period=1μs / window=64
- [ ] **loss 時の応答**が適切か
  - raknet/slikenet: loss で cwnd を **1 MTU にリセット**（Tahoe 動作）。min MTU=576 なので初期 window=548byte。回復が極めて遅い
  - quiche BBRv2: beta=0.3（**70% 削減**）。標準の 50% より攻撃的

<details><summary>14ライブラリの輻輳制御詳細</summary>

| lib | アルゴリズム | 初期 window | loss 応答 | ACK 応答 | 備考 |
|-----|-----------|-----------|----------|---------|------|
| enet | custom bandwidth throttle | 65536byte（max、slow start なし） | throttle−=2/32 per RTT | throttle+=2/32 per good RTT | window ベースではなく確率的スロットル |
| kcp | TCP Reno AIMD | cwnd=0→1, ssthresh=**2** | ssthresh=cwnd/2, cwnd=1 | SS: +1/ACK, CA: +1MSS/RTT | **adapter で無効化**（nocwnd=1） |
| coop_rudp | custom rate-based AIMD | **UINT32_MAX**（~4.3Gbps、slow start なし） | safe_bps/=2（floor 64Kbps） | safe_bps+=mtu×8/ACK | 初期レート実質無制限 |
| apex_rudp | **なし** | — | — | — | — |
| mini_rudp | **なし** | — | — | — | pending cap=65536 のみ |
| yojimbo | **なし** | — | — | — | — |
| gns | TFRC(RFC 3448風)+token bucket | 4380byte/assumed_RTT | TFRC formula で rate 更新 | BW/loss 推定から rate 更新 | Nagle 5ms 結合あり |
| msquic | **BBR**（adapter 選択、lib 既定は Cubic） | 10×MSS≈12KB | Recovery: window=BytesInFlight | BDP-based target cwnd | preview feature gate |
| quiche | **BBRv2**（gcongestion, Chromium 派生） | 10×MSS=12KB | beta=**0.3**（70%削減） | BDP×cwnd_gain | 標準より攻撃的 |
| lsquic | **BBR**（adapter 選択、lib 既定は Adaptive） | 32×1460=46720byte | Recovery window; Cubic fallback: ×0.8 | Startup: 2.885×; ProbeBW cycle | — |
| udt4 | **BenchCCC（adapter 独自、CC なし）** | cwnd=64pkt, period=1μs | **なし（空実装）** | **なし** | lib 既定 CUDTCC(DAIMD) を完全置換 |
| raknet | TCP Reno 風 sliding window | 1 MTU（~548byte） | ssthresh=cwnd/2, **cwnd=1MTU（Tahoe）** | SS: +1MTU/ACK, CA: +MTU²/cwnd | 回復が極めて遅い |
| slikenet | 同上 | 同上 | 同上 | 同上 | 同上 |
| litenetlib | **なし** | 固定 64pkt window | — | — | — |

</details>

---

## 4. シーケンス番号

- [ ] シーケンス番号の**ビット幅**がユースケースに対して十分か
  - 16-bit: enet（per-channel）、yojimbo（packet）、litenetlib（**15-bit、MaxSequence=32768**）
  - 100K PPS で 16-bit は **0.65 秒**、15-bit は **0.33 秒**で一周する
- [ ] **ラップアラウンド処理**が正しいか
  - apex_rudp: **ラップアラウンド処理なし**（plain `>` comparison）。uint32_t なので 1M PPS で ~71 分で破綻
  - mini_rudp: ラップアラウンド処理なし（hash map の exact equality のみ、順序比較なし）
  - kcp: signed cast `(IINT32)(later - earlier)` で half-space 比較
  - enet: 16 窓（各 4096 seq）の windowed dedup
  - yojimbo: half-space `s1-s2<=32768` 比較
  - raknet/slikenet: **24-bit** seq で uint32_t halfSpan を使用。型幅の不一致で大きな gap が誤判定される可能性

<details><summary>14ライブラリのシーケンス番号詳細</summary>

| lib | bit幅 | ラップアラウンド | 100K PPS で一周 | 1M PPS で一周 |
|-----|------|---------------|----------------|--------------|
| enet | 16-bit(per-channel) | windowed(16窓×4096seq) | 0.65s | 0.065s |
| kcp | 32-bit | signed cast half-space | 11.9h | 71.6min |
| coop_rudp | 32-bit(pkt) / 16-bit(channel) | `seq32_after()`: unsigned diff < 0x80000000 | 11.9h | 71.6min |
| apex_rudp | 32-bit | **なし（plain `>`）** | 11.9h | 71.6min |
| mini_rudp | 32-bit | **なし（equality のみ）** | 11.9h | 71.6min |
| yojimbo | 16-bit(reliable) / 64-bit(netcode) | half-space(32768) | 0.65s(reliable) | 0.065s |
| gns | 64-bit(内部) / 16-bit(wire) | 16-bit gap→64-bit 展開 | 実質なし | 実質なし |
| msquic | 64-bit(QUIC var-int) | monotonic increasing | 実質なし | 実質なし |
| quiche | 64-bit | `decode_pkt_num()` window-based | 実質なし | 実質なし |
| lsquic | 64-bit | monotonic increasing | 実質なし | 実質なし |
| udt4 | **31-bit**（MSB=control flag） | quarter-range 比較 | 5.9h | 35.8min |
| raknet | **24-bit**(uint24_t) | uint32 halfSpan（**型幅不一致**） | 167.8s | 16.8s |
| slikenet | **24-bit** | 同上 | 167.8s | 16.8s |
| litenetlib | **15-bit**(MaxSequence=32768) | modular(HalfMaxSequence=16384) | **0.33s** | **0.033s** |

</details>

---

## 5. 内部キューとフロー制御

- [ ] **送信キューが満杯になったときの挙動**を把握しているか
  - yojimbo: send queue full = **コネクションエラー**（`CHANNEL_ERROR_SEND_QUEUE_FULL`）。backpressure ではなく接続が切断される。これは設計上の選択だが、高負荷で意図しない切断を招く
  - coop_rudp: `RUDP_SEND_QUEUE_FULL` を返し、ACK を抑制
  - apex_rudp: -1 を返す（backpressure）
  - udt4: 非同期モードで `EASYNCSND` エラー、同期モードでブロック
- [ ] **受信バッファの上限**を設けているか。無制限だとロス時にメモリが膨らむ
- [ ] **in-flight の上限**（再送バッファ）がスループットのボトルネックになっていないか
  - raknet/slikenet: resendBuffer = **512 エントリ**のハードリミット。~500byte payload で in-flight 最大 ~256KB。高 BDP 環境では不足する

<details><summary>14ライブラリの内部キュー上限</summary>

| lib | 送信キュー上限 | 受信キュー上限 | 満杯時の挙動 |
|-----|-------------|-------------|------------|
| enet | 無制限（unreliable は throttle drop） | 32MB/peer | reliable: 滞留、unreliable: throttle |
| kcp | 無制限（<128 frags で常に成功） | rcv_wnd(128既定) | recv window が 0 に→sender 停止 |
| coop_rudp | per_conn_queue_cap(min 1024)/ring | max_recv_events | RUDP_SEND_QUEUE_FULL 返却 |
| apex_rudp | 4096/conn | inbox: 1M | send: -1 返却、recv: oldest drop |
| mini_rudp | 65536/conn | なし（直接配信） | send: -1 返却 |
| yojimbo | 4096/channel/direction | 4096/channel/direction | **コネクションエラー（切断）** |
| gns | SendBufferSize=32MB | RecvBufferSize=32MB / RecvBufferMessages=1M | send: k_EResultLimitExceeded |
| msquic | datagram:無制限 / stream:flow control | 16MB flow control window | datagram: queue→cancel |
| quiche | datagram:65536 / stream:flow control | datagram:1200 / inbox:65536 | Error::Done 返却 |
| lsquic | datagram:64(adapter) / stream:flow control | inbox:65536 | datagram: -1(drop) |
| udt4 | 8192pkt(動的拡張) | 8192pkt | async:EASYNCSND / sync:block |
| raknet | outgoing:無制限 / resend:**512** | 無制限 | resend full→reliable send blocked |
| slikenet | 同上 | 同上 | 同上 |
| litenetlib | outgoing:無制限 / pending window:**64** | 無制限 | window full→outgoing accumulate |

</details>

---

## 6. スレッドモデル

- [ ] ライブラリの**スレッドモデル**を把握しているか。adapter と相性が悪いとパフォーマンスが崩壊する
- [ ] litenetlib は既定で **conn ごとに 2 OS スレッド**（ReceiveThread + LogicThread）を生成。1000 conn = 2000 スレッドで飽和する。`StartInManualMode` で無効化必須
- [ ] raknet/slikenet は `RAKPEER_USER_THREADED=1` でも recv thread が生成されるバグ。Shutdown() 時に UAF。adapter は RakPeer を abandon（意図的リーク）で回避
- [ ] gns は単一 service thread がグローバルロックを保持しながら暗号処理を行う。1000 conn でロック競合により delivery が崩壊（adapter:96-99 コメント「1000conn collapse」）。poll group で軽減
- [ ] msquic adapter は単一 `std::mutex` で全コールバックを直列化。worker thread 数に対してボトルネックになりうる
- [ ] udt4 の `CSndUList::m_ListLock` は multiplexer 単位（全 conn 共有）。多 conn ワークロードで中心的な競合点

<details><summary>14ライブラリのスレッドとロック詳細</summary>

| lib | ライブラリ thread | adapter thread | ロック | 主な競合点 |
|-----|-----------------|---------------|-------|----------|
| enet | 0 | 0 | 0 | なし（single-threaded） |
| kcp | 0 | 0 | 0 | なし |
| coop_rudp | 0 | 0-1(async TX) | 1(tx_mu) | tx_mu: main↔TX worker |
| apex_rudp | 0 | 1-8 TX + 0-1 RX | 3(tx_mu,recycled_mu,rx_mu) | tx_mu: main↔TX workers |
| mini_rudp | 0 | 0 | 0 | なし |
| yojimbo | 0 | 0 | 0 | なし |
| gns | **1**(service) | 0 | 4+(global recursive_timed_mutex,per-conn,per-pollgroup) | **global lock × crypto** |
| msquic | **N**(worker+datapath, per core) | 0 | many(per-worker,per-conn,per-stream) | **adapter の単一 mtx_** |
| quiche | 0 | 0 | 0 | なし |
| lsquic | 0 | 0 | 0 | なし |
| udt4 | **3/multiplexer**(SndQueue worker,RcvQueue worker,GC) | 0 | 6+/conn+multiplexer | **CSndUList::m_ListLock** |
| raknet | **1/RakPeer**(recv) | 0 | 6+/RakPeer | bufferedPacketsQueueMutex |
| slikenet | **1/RakPeer**(recv) | 0 | 6+ | 同上 |
| litenetlib | 0-**2/NetManager** | 0 | 10+(pool,peers RWLock,event,per-channel) | **_poolLock（全スレッド競合）** |

</details>

---

## 7. メモリアロケーション

- [ ] 送受信の**ホットパスで heap allocation** を避けているか。パケットごとの malloc は高 PPS でアロケータがボトルネックになる
  - **ゼロアロケーション**: coop_rudp（ライブラリ内部で全事前確保。14ライブラリ中唯一）
  - **プール化**: enet（thread-local size-segregated free-list）、apex_rudp（thread-safe recycled buffer）、yojimbo（TLSF 10MB pool、O(1)）、lsquic（malo slab allocator）
  - **毎回 alloc**: kcp（segment ごとに malloc）、gns（FIXME コメントで pool 未実装を自認）
  - **マネージド言語**: litenetlib は `ArrayPool` + PacketPool で GC 圧力を軽減。PacketPoolSize を `max(1000, conns×2)` に auto-scale

---

## 8. 暗号

- [ ] 暗号のオン/オフと**性能への影響**を把握しているか
  - 暗号必須: msquic/quiche/lsquic（TLS 1.3）、yojimbo（libsodium、per-packet authenticated encryption）
  - gns: OpenSSL/libsodium。`Unencrypted=3` で無効化可能だが、encrypted がデフォルト
  - per-packet crypto は CPU を支配する。msquic は media_relay c50 で server CPU ~198% 飽和（§1.8）

---

## 9. コネクション管理

- [ ] `connect()` がブロッキングの場合、高 conns で接続フェーズだけで秒単位になる（§1.9: UDT4）
- [ ] `close()` が内部 worker と競合して deadlock しないか（§5.11: msquic no-op close）
- [ ] **接続 ID の衝突**を処理しているか。raknet/slikenet の GUID は `gettimeofday()` マイクロ秒値で同時接続時に衝突する

---

## 10. ベンチマーク計測の落とし穴

### 10.1 計測アーティファクト

- [ ] mixed traffic のレイテンシを**チャネル別ヒストグラム**で記録しているか（§1.1: combined histogram で結論が反転）
- [ ] client/server の**計測窓が重なっている**か（§1.7: `delivery ≈ (duration−ramp)/duration`）
- [ ] warmup データが計測に混入しないか（§5.10: measurement bit）
- [ ] busy-spin idle で CPU 100% を飽和と誤読していないか（§1.6）

### 10.2 統計

- [ ] 結論に使う数字は **N ≥ 3** で中央値 + IQR か（§1.5: N=1 は ±10〜20% 振れる）
- [ ] **Coordinated Omission** を認識しているか
- [ ] delivery ratio の定義（往復 echo vs 片道配信率）を明記しているか（§5.1）

### 10.3 CPU とメモリ

- [ ] シングルスレッド lib（上限 ~100%）とマルチスレッド lib（SMT2 で ~200%）を**スレッドモデル併記**で比較しているか（§5.2）
- [ ] ハーネスのメモリオーバーヘッドを分離して **server 側 RSS** で評価しているか（§5.4）

### 10.4 ネットワークエミュレーション

- [ ] netem `limit` を十分大きく設定しているか（既定 1000 → サイレントドロップ。§1.6）
- [ ] loopback netem は send/recv 両方を通る（片道 25ms → RTT ~50ms、loss 1% → 実効 ~2%）

---

## 11. 負荷生成の品質

- [ ] 負荷生成器が**意図したレートを出し切れているか検証する仕組み**があるか（§1.9）
- [ ] pacing 精度閾値を**送信間隔の比率**で設定しているか（§1.4: `max(100μs, interval/10)`）
- [ ] multi-process 生成で各プロセスの計測窓が揃っているか（§1.7）

---

## 12. 診断パターン

| 症状 | まず疑うこと | 事例 |
|------|-------------|------|
| c1 から delivery が低い | adapter/harness バグ（API 誤用、窓ズレ） | §1.9: yojimbo, udt4, msquic |
| delivery が `(duration−X)/duration` | client/server の active window ズレ | §1.7: msquic ramp 未加算 |
| 手動では動くが CI で落ちる | fd 上限、CWD、権限の環境差 | §1.7: systemd-run LimitNOFILE |
| CPU 100% だが delivery は低い | busy-spin idle の誤読 or tick 洪水 | §1.6, §1.9: yojimbo |
| バッファ増で性能悪化 | バッファブロート → 偽 RTO 再送 | kcp 256KB→1MB で 0.78→0.52 |
| 高 conns で突然 invalid | 負荷生成側の律速 | §1.9: client 4proc→8proc で解消 |
| 長時間稼働で突然壊れる | シーケンス番号ラップアラウンド | apex_rudp: ~71min@1M PPS |

---

## 付録A: Adapter がライブラリ既定値を上書きしている箇所

ベンチマーク結果を「ライブラリの性能」として解釈する際に注意が必要な設定差異。

| lib | パラメータ | ライブラリ既定 | adapter 設定 | 影響 |
|-----|----------|-------------|------------|------|
| kcp | nocwnd | 0（CC 有効） | **1（CC 無効）** | 輻輳制御が完全に無効 |
| kcp | snd_wnd | 32 | **256** | 送信ウィンドウ 8 倍 |
| kcp | fastresend | 0（無効） | **2** | Fast Retransmit 有効化 |
| gns | socket buffer | 256KB | **4MB** | 16 倍 |
| gns | SendBufferSize | 512KB | **32MB** | 64 倍 |
| gns | SendRateMin/Max | 256KB/s | **256MB/s** | 1000 倍 |
| msquic | CC algorithm | Cubic | **BBR** | ロス耐性が大幅に異なる |
| lsquic | CC algorithm | Adaptive(BBR→Cubic) | **BBR** | 固定 BBR |
| udt4 | CC algorithm | CUDTCC(DAIMD) | **BenchCCC（CC なし）** | 輻輳制御が完全に無効 |
| yojimbo | message queue size | 1024 | **4096** | 4 倍 |
| litenetlib | PacketPoolSize | 1000 | **max(1000,conns×2)** | 動的スケール |

## 付録B: 14ライブラリ設計サマリ

| lib | socket buf | fd model | I/O batch | threading | CC | seq bits | 暗号 | hot-path alloc | 死活検知 |
|-----|-----------|----------|-----------|-----------|-----|---------|------|---------------|---------|
| raw_udp | 256KB | per-conn(client) | sendto/recvfrom | single | — | — | なし | なし | — |
| mini_rudp | 256KB | single fd | sendto/recvfrom | single | なし | 32-bit | なし | pool | **なし** |
| coop_rudp | 4MB | single fd | **sendmmsg/recvmmsg** | +TX worker | rate AIMD | 32-bit | なし | **ゼロ** | **なし** |
| apex_rudp | 4MB | single+shard | **sendmmsg/recvmmsg** | TX×1-8+RX | **なし** | 32-bit(**wrap未処理**) | なし | pool | **なし** |
| enet | 256KB | single fd | sendmsg | single | throttle | **16-bit** | なし | pool | あり(5-30s) |
| kcp | 256KB | single fd | sendto/recvfrom | single | AIMD(**adapter無効**) | 32-bit | なし | malloc/send | あり(**adapter無視**) |
| raknet | 256KB/16KB | per-conn | sendto/recvfrom | 1 recv thread | Tahoe | **24-bit** | optional | pool+malloc | あり(10s) |
| slikenet | 256KB/16KB | per-conn | sendto/recvfrom | 1 recv thread | Tahoe | **24-bit** | optional | pool+malloc | あり(10s) |
| udt4 | 65KB(UDP) | per-conn | sendmsg | 3/mux | **BenchCCC(なし)** | **31-bit** | なし | malloc/vector | あり(~5s) |
| yojimbo | 4MB | single fd | sendto/recvfrom | single(100Hz) | なし | **16-bit** | **libsodium必須** | TLSF pool | あり(10s) |
| gns | 4MB | single fd | sendto/sendmsg | 1 service | TFRC | 64-bit | OpenSSL(opt) | malloc(**FIXME**) | あり(10s) |
| msquic | INT32_MAX/未設定 | per-conn | **sendmmsg/GSO** | N workers | **BBR** | 64-bit | **TLS必須** | alloc/send | あり(30s) |
| quiche | 256KB | single fd | **sendmmsg/recvmmsg** | single | **BBRv2** | 64-bit | **TLS必須** | vector/send | あり(30s) |
| lsquic | 256KB | single fd | **sendmmsg/recvmmsg** | single | **BBR** | 64-bit | **TLS必須** | malo slab | あり(30s) |
| litenetlib | 1MB | per-conn | SendTo | manual(既定) | なし | **15-bit** | transport | ArrayPool | あり(5s) |
