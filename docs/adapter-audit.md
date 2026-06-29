# 14ライブラリ Adapter 調査結果

[`checklist.md`](checklist.md) の各項目を本プロジェクトの14ライブラリで検証した結果。
adapter コード + third_party ライブラリのソースコードを精読して得た具体的な値を記載。

---

## 1. ソケットバッファ

全ライブラリで `getsockopt` による設定後確認なし。 `net.core.rmem_max`（既定 ~208KB）を超えた要求はサイレントにクランプされている可能性が高い。

| lib | SO_RCVBUF | SO_SNDBUF | 設定箇所 | 備考 |
|-----|-----------|-----------|---------|------|
| raw_udp | 256KB | 256KB | adapter:55-58 | — |
| mini_rudp | 256KB | 256KB | adapter:95-96 | — |
| coop_rudp | 4MB | 4MB | adapter:85-86 | — |
| apex_rudp | 4MB | 4MB | adapter:146-147 | `APEX_RCVBUF_KB` env で変更可 |
| enet | 256KB | 256KB | host.c:65-66 + adapter:208-209 | `ENET_RCVBUF_KB` env で変更可 |
| kcp | 256KB | 256KB | adapter:124-125 | `KCP_RCVBUF_KB` env で変更可。1MB に増やすと delivery 0.78→0.52 に悪化（バッファブロート） |
| raknet | 256KB | 256KB | RakNetSocket2_Berkley.cpp:39,54 | vendored SLikeNet patch で送受信を対称化 |
| slikenet | 256KB | 256KB | RakNetSocket2_Berkley.cpp:39,54 | 同上 |
| udt4 | 65KB(UDP層) | 65KB(UDP層) | channel.cpp:152-153 | UDT 層 recv buffer: 8192pkt×1500≈12MB |
| yojimbo | 4MB | 4MB | netcode.c:55-58 | `#define` 固定、変更不可 |
| gns | 4MB | 4MB | lowlevel.cpp:2065-2073 + adapter:75 | lib 既定 256KB を adapter が 4MB に上書き。`gns_smallbuf` variant で 256KB |
| msquic | INT32_MAX | 未設定 | datapath_epoll.c:702-709 | 2.1GB を要求。確実にクランプされている |
| quiche | 256KB | 256KB | adapter:66-67 | — |
| lsquic | 256KB | 256KB | adapter:228-230 | — |
| litenetlib | 1MB | 1MB | LiteNetManager.Socket.cs:405-406 | — |

---

## 2. I/O syscall パス

| lib | send | recv | batch | 備考 |
|-----|------|------|-------|------|
| raw_udp | sendto | recvfrom | 1 | — |
| mini_rudp | sendto | recvfrom | 1 | ACK も個別 sendto（10byte/pkt）。ピギーバックなし |
| coop_rudp | sendmmsg | recvmmsg | send:256 / recv:256 | 同一 peer 宛を物理バッチ化。論理 batch size は 4096/32768 |
| apex_rudp | sendmmsg/sendto | recvmmsg | send:256 / recv:64 | 3 経路: inline / batch / async TX |
| enet | sendmsg | recvmsg | 1（iovec で ACK+data を peer 単位結合） | — |
| kcp | sendto | recvfrom | 1 | output callback 経由 |
| raknet | sendto | recvfrom | 1 | recv は blocking thread |
| slikenet | sendto | recvfrom | 1 | 同上 |
| udt4 | sendmsg(iovec) | recvmsg | 1 | UDT epoll 経由 |
| yojimbo | sendto | recvfrom | 1 | netcode 層で直接 syscall |
| gns | sendto/sendmsg | recvmsg/recvfrom | 1 | epoll_wait + DrainSocket |
| msquic | sendmmsg/sendmsg(GSO) | recvmmsg(GRO) | 可変 | GSO/GRO 対応 |
| quiche | sendmmsg | recvmmsg | 64 | — |
| lsquic | sendmmsg | recvmmsg | send:64(×最大1024pkt/tick) / recv:64 | engine がパケット収集→batch 送信 |
| litenetlib | .NET SendTo→sendto | .NET ReceiveFrom→recvfrom | 1 | packet merging で小パケット結合 |

---

## 3. RTO と再送

| lib | RTO 方式 | 初期値 | 最小 | 最大 | バックオフ | Fast Retransmit |
|-----|---------|-------|------|------|----------|----------------|
| enet | Jacobson/Karels | SRTT=0 時 peer default | — | — | 2× exponential | なし |
| kcp | Jacobson/Karels | 200ms | 100ms / 30ms(nodelay) | 60s | 2×/1.5×/+RTO/2 | あり（dup ACK=2, fastlimit=5） |
| coop_rudp | 固定（SRTT 未使用） | 100ms | — | — | なし | あり（3pkt gap, SACK bitmap） |
| apex_rudp | 固定 | 100ms | — | — | なし | なし |
| mini_rudp | 固定（RTT 未測定） | 50ms | — | — | なし | なし |
| yojimbo | 固定 | 100ms | — | — | なし | なし |
| gns | 3×SRTT+60ms | 1s(ping 未知時) | — | — | NACK ベース | あり（NACK, 3ms） |
| msquic | QUIC PTO: SRTT+4×RTTVAR+MaxAckDelay | ~250ms | 1ms | — | 2^n | あり（3pkt+RACK） |
| quiche | RFC 9002 PTO | ~333ms | 1ms | 2^20×PTO | 2^n | あり（3pkt→20, +time-based） |
| lsquic | (SRTT+4×RTTVAR)×2^min(n,10) | 500ms | 200ms | 60s | 2×(max 10回) | あり（3pkt + early retransmit srtt/4） |
| udt4 | EXP_COUNT×(RTT+4×RTTVar)+SYN | 100ms | 300ms | — | EXP_COUNT 乗算 | なし（NAK ベース） |
| raknet | 2×SRTT+4×RTTVAR+30ms | 2s | — | 2s | なし | なし（NAK ベース） |
| slikenet | 同上 | 同上 | — | 同上 | なし | 同上 |
| litenetlib | 25ms+AvgRTT×2.1（Jacobson 非準拠） | 27ms | — | — | なし | なし |

### ACK 形式

| lib | ACK 形式 | サイズ | 遅延 | ピギーバック |
|-----|---------|-------|------|------------|
| enet | 個別/selective per command | 8byte/ACK | なし | あり |
| kcp | 個別+cumulative UNA | 24byte/seg | interval(5ms)まで | あり（UNA 全パケット） |
| coop_rudp | cumulative+64-bit SACK bitmap | 12byte | なし | あり（全パケット） |
| apex_rudp | 64-bit SACK bitmap | 12byte | 2ms delayed | あり |
| mini_rudp | 個別 per-packet | 10byte 専用パケット | なし | なし |
| yojimbo | 32-bit bitmap | 4-9byte | なし | あり |
| gns | range-based NACK+ACK | 5+byte | 50ms max | あり |
| msquic | range-based QUIC ACK | 5-30byte | 25ms / 2pkt即時 | あり |
| quiche | range-based(SmallVec<4>) | 5+byte | 25ms | あり |
| lsquic | range-based(最大256 ranges) | 4-7byte/range | max(1ms,min(ack_delay,srtt/4)) | なし（別 datagram） |
| udt4 | cumulative+別 NAK | 32-40byte | 10ms / 64pkt lite | なし（別 control packet） |
| raknet | range-based selective | 4-7byte/range | ~10ms | なし（別 datagram） |
| slikenet | 同上 | 同上 | 同上 | 同上 |
| litenetlib | bitmap(64pkt window) | 14byte | 15ms | なし |

---

## 4. 再送上限と死活検知

| lib | 再送上限 | 死活検知 | keepalive | timeout |
|-----|---------|---------|-----------|---------|
| enet | ~5-6回 | reliable timeout chain | 500ms PING | 5-30s |
| kcp | dead_link=20/seg | adapter が `kcp->state=-1` を切断扱い | なし | dead_link 到達 |
| coop_rudp | adapter既定64回(`COOP_MAX_RETRANSMITS`) | max retransmit / idle timeout | なし | 10s(`COOP_IDLE_TIMEOUT_MS`) |
| apex_rudp | 10s reliable pending timeout(adapter) | 未 ACK reliable timeout | なし | 10s (`APEX_RELIABLE_TIMEOUT_MS`) |
| mini_rudp | 10s reliable pending timeout(adapter) | 未 ACK reliable timeout | なし | 10s (`MINI_RUDP_RELIABLE_TIMEOUT_MS`) |
| yojimbo | 無制限(msg level) | netcode keepalive | 100ms(10Hz) | 10s |
| gns | 無制限 | TimeoutConnected | 10s(active)/60s(idle) | 10s |
| msquic | 無制限(PTO backoff) | DisconnectTimeout | 無効(既定) | idle:30s, disconnect:16s |
| quiche | 無制限(PTO 2^20 まで) | idle timeout | なし | 30s |
| lsquic | 無制限(2^10 まで) | idle+no-progress | 15s PING | idle:30s, handshake:10s |
| udt4 | 無制限 | adapter が UDT `BROKEN`/`CLOSED`/`NONEXIST` を切断扱い | EXP timer 時 | ~5s+ |
| raknet | 無制限 | AckTimeout | timeoutTime/2 | 10s(release)/30s(debug) |
| slikenet | 同上 | 同上 | 同上 | 同上 |
| litenetlib | 無制限 | DisconnectTimeout | 1000ms Ping/Pong | 5s |

---

## 5. 輻輳制御

| lib | アルゴリズム | 初期 window | loss 応答 | 備考 |
|-----|-----------|-----------|----------|------|
| enet | custom bandwidth throttle | 65536byte(max, slow start なし) | throttle−=2/32 per RTT | window ベースではなく確率的 |
| kcp | TCP Reno AIMD | cwnd=0→1, ssthresh=2 | ssthresh=cwnd/2, cwnd=1 | adapter で無効化（nocwnd=1） |
| coop_rudp | custom rate-based AIMD | UINT32_MAX（~4.3Gbps） | safe_bps/=2 | 初期レート実質無制限。slow start なし |
| apex_rudp | なし | — | — | — |
| mini_rudp | なし | pending cap=65536 のみ | — | — |
| yojimbo | なし | — | — | — |
| gns | TFRC(RFC 3448風)+token bucket | 4380byte/assumed_RTT | TFRC formula | Nagle 5ms 結合あり |
| msquic | BBR（adapter 選択） | 10×MSS≈12KB | Recovery: window=BytesInFlight | lib 既定は Cubic |
| quiche | BBRv2(adapter 選択) | 10×MSS=12KB | beta=0.3（70%削減） | 標準の 50% より攻撃的。lib 既定は CUBIC |
| lsquic | BBR（adapter 選択） | 32×1460=46720byte | Recovery window | lib 既定は Adaptive(BBR→Cubic) |
| udt4 | BenchCCC（adapter 独自、CC なし） | cwnd=64pkt, period=1μs | なし（空実装） | lib 既定 CUDTCC(DAIMD) を完全置換 |
| raknet | TCP Reno 風 | 1 MTU(~548byte) | cwnd=1MTU（Tahoe 動作） | 回復が極めて遅い |
| slikenet | 同上 | 同上 | 同上 | 同上 |
| litenetlib | なし | 固定 64pkt window | — | — |

---

## 6. シーケンス番号

| lib | bit幅 | ラップアラウンド処理 | 100K PPS で一周 | 1M PPS で一周 |
|-----|------|-------------------|----------------|--------------|
| enet | 16-bit(per-channel) | windowed(16窓×4096seq) | 0.65s | 0.065s |
| kcp | 32-bit | signed cast half-space | 11.9h | 71.6min |
| coop_rudp | 32-bit(pkt) / 16-bit(ch) | `seq32_after()`: diff<0x80000000 | 11.9h | 71.6min |
| apex_rudp | 32-bit | `seq32_after()` half-space + seq 0 skip | 11.9h | 71.6min（wrap 処理済み） |
| mini_rudp | 32-bit | bounded equality dedup + seq 0 skip + 10s pending timeout | 11.9h | 71.6min |
| yojimbo | 16-bit(reliable) / 64-bit(netcode) | half-space(32768) | 0.65s | 0.065s |
| gns | 64-bit(内部) / 16-bit(wire) | 16bit gap→64bit 展開 | 実質なし | 実質なし |
| msquic | 64-bit(QUIC var-int) | monotonic increasing | 実質なし | 実質なし |
| quiche | 64-bit | window-based reconstruction | 実質なし | 実質なし |
| lsquic | 64-bit | monotonic increasing | 実質なし | 実質なし |
| udt4 | 31-bit(MSB=control flag) | quarter-range 比較 | 5.9h | 35.8min |
| raknet | 24-bit(uint24_t) | uint32 halfSpan（型幅不一致） | 167.8s | 16.8s |
| slikenet | 24-bit | 同上 | 167.8s | 16.8s |
| litenetlib | 15-bit(MaxSequence=32768) | modular(HalfMaxSequence=16384) | 0.33s | 0.033s |

---

## 7. 内部キュー上限

| lib | 送信キュー上限 | 受信キュー上限 | 満杯時の挙動 |
|-----|-------------|-------------|------------|
| enet | reliable adapter cap 32MiB(`ENET_RELIABLE_QUEUE_BYTES`) / unreliable は throttle drop | lib 32MB/peer + adapter inbox 65536 | reliable cap で -1 / unreliable throttle / adapter inbox 満杯で oldest drop |
| kcp | adapter cap 32MiB(`KCP_SEND_QUEUE_BYTES`)（1 message は <128 frags） | rcv_wnd(256, adapter設定) | adapter cap で -1 / window→0 で sender 停止 |
| coop_rudp | per_conn_queue_cap(min 1024)/ring | max_recv_events | RUDP_SEND_QUEUE_FULL 返却 |
| apex_rudp | reliable 4096/conn + 10s timeout + async TX 1M（server unreliable 既定 ON） | inbox: 1M | send: reliable 満杯で -1 / timeout で conn inactive / async TX 満杯で drop / recv: oldest drop |
| mini_rudp | 65536/conn + 10s timeout | なし（直接配信） | send: -1 / timeout で conn inactive |
| yojimbo | 4096/channel/direction | 4096/ch/dir | send: -1（adapter が CanSendMessage で事前チェック） |
| gns | SendBuffer=32MB | lib RecvBuffer=32MB/Msgs=1M + adapter inbox 65536(`GNS_INBOX_MESSAGES`) | send:k_EResultLimitExceeded / inbox oldest drop + stderr |
| msquic | datagram:無制限 / stream:flow control | 16MB flow control + adapter inbox 65536(`MSQUIC_INBOX_MESSAGES`) | datagram: queue→cancel / inbox oldest drop + stderr |
| quiche | datagram:65536 / stream:adapter pending 32MiB(`QUICHE_STREAM_PENDING_BYTES`)→flow control | datagram:1200 / inbox:65536 | datagram:Error::Done / stream:adapter cap で -1 / partial write は pending |
| lsquic | datagram:64(adapter) / stream:adapter pending_writes 32MiB(`LSQUIC_PENDING_WRITE_BYTES`)→flow control | inbox:65536 | datagram: -1(drop) / stream:adapter cap で -1 / partial write は pending |
| udt4 | adapter out_pending 32MiB(`UDT4_OUT_PENDING_BYTES`)→8192pkt(動的拡張) | 8192pkt | adapter cap で -1 / async:EASYNCSND なら保持 |
| raknet | outgoing adapter cap 32MiB(`RAKNET_OUTGOING_BYTES`/`RAK_FAMILY_OUTGOING_BYTES`) / resend:512 | 無制限 | adapter cap で -1 / resend full→reliable blocked |
| slikenet | outgoing adapter cap 32MiB(`SLIKENET_OUTGOING_BYTES`/`RAK_FAMILY_OUTGOING_BYTES`) / resend:512 | 無制限 | adapter cap で -1 / resend full→reliable blocked |
| litenetlib | reliable outgoing adapter cap 32MiB(`LNL_OUTGOING_BYTES`) / pending window:64 | 無制限 | adapter cap で backpressure / window full→outgoing 蓄積 |

---

## 8. スレッドとロック

| lib | ライブラリ thread | adapter thread | 主なロック | 競合点 |
|-----|-----------------|---------------|----------|-------|
| enet | 0 | 0 | — | なし |
| kcp | 0 | 0 | — | なし |
| coop_rudp | 0 | 0-1(async TX) | tx_mu | tx_mu: main↔TX |
| apex_rudp | 0 | 1-8 TX + 0-1 RX | tx_mu,recycled_mu,rx_mu | tx_mu: main↔TX workers |
| mini_rudp | 0 | 0 | — | なし |
| yojimbo | 0 | 0 | — | なし |
| gns | 1(service) | 0 | global recursive_timed_mutex 等 | global lock で 1000conn collapse（既定は暗号なし。gns_encrypted variant のみ crypto 負荷加算） |
| msquic | N(worker+datapath) | 0 | per-worker,per-conn,per-stream | adapter の単一 mtx_ が全 callback 直列化 |
| quiche | 0 | 0 | — | なし |
| lsquic | 0 | 0 | — | なし |
| udt4 | 3/mux(SndQ,RcvQ,GC) | 0 | 6+/conn+mux | CSndUList::m_ListLock（全 conn 共有） |
| raknet | 1/RakPeer(recv) | 0 | 6+/RakPeer | bufferedPacketsQueueMutex |
| slikenet | 1/RakPeer(recv) | 0 | 6+ | 同上。RAKPEER_USER_THREADED=1 でも recv thread 生成バグ→UAF |
| litenetlib | 0-2/NetManager | 0 | 10+(pool,peers RWLock等) | _poolLock（全スレッド競合）。既定 2thread/conn→manual mode で無効化必須 |

---

## 9. MTU とフラグメンテーション

| lib | MTU/payload | 最大 fragment 数 | OoO 再組立 | 備考 |
|-----|-----------|----------------|-----------|------|
| raw_udp | 65507B | なし | — | — |
| mini_rudp | 65497B | なし | — | 分割非対応 |
| coop_rudp | 1200B(payload=1148) | 256 | あり(256-bit bitmap) | lib は 256-frag 対応だが adapter max_payload=1148B で単一フレーム上限。ベンチマークでは多フラグメント不可 |
| apex_rudp | 65486B | なし | — | 分割非対応 |
| enet | 1392B | 1,048,576 | あり(bitmap) | ~32MB まで |
| kcp | 1400B(MSS=1376) | 127 | なし（in-order のみ） | multi-frag message で HoL |
| raknet | ~576B(min MTU) | 2^32 / 65536(同時) | あり | unreliable→reliable 自動昇格 |
| slikenet | 同上 | 同上 | あり | 同上 |
| udt4 | ~1472B(MSS) | 8192+ | あり(DGRAM) | STREAM は透過 |
| yojimbo | 1024B(fragment) | pkt:16 / block:256 | あり | block=250ms resend |
| gns | ~1232B | ~14(unreliable) / 無制限(stream) | あり | unreliable は超過で破棄 |
| msquic | QUIC MTU / datagram payload 1000B | N/A(stream) | あり | stream segmentation。unreliable は datagram path（adapter 上限 1000B） |
| quiche | QUIC MTU | N/A(stream) | あり(BTreeMap) | datagram: ~1200B 上限 |
| lsquic | QUIC MTU | N/A(stream) | あり | datagram: ~1200B 上限 |
| litenetlib | MTU−header | 65535 | あり | adapter が MaxPayloadBytes=1000 で制限。ベンチマークではそれ以上のサイズは不可 |

---

## 10. Adapter がライブラリ既定を上書きしている箇所

ベンチマーク結果を「ライブラリの性能」として解釈する際に注意が必要な設定差異。

| lib | パラメータ | ライブラリ既定 | adapter 設定 | 影響 |
|-----|----------|-------------|------------|------|
| kcp | nocwnd | 0（CC 有効） | 1（CC 無効） | 輻輳制御が完全に無効 |
| kcp | snd_wnd | 32 | 256 | 送信ウィンドウ 8 倍 |
| kcp | fastresend | 0（無効） | 2 | Fast Retransmit 有効化 |
| gns | socket buffer | 256KB | 4MB | 16 倍 |
| gns | SendBufferSize | 512KB | 32MB | 64 倍 |
| gns | SendRateMin/Max | 256KB/s | 256MB/s | 1000 倍 |
| msquic | CC algorithm | Cubic | BBR | ロス耐性が大幅に異なる |
| quiche | CC algorithm | CUBIC | BBRv2 | `quiche_config_set_cc_algorithm` で上書き |
| lsquic | CC algorithm | Adaptive(BBR→Cubic) | BBR | 固定 BBR |
| udt4 | CC algorithm | CUDTCC(DAIMD) | BenchCCC（CC なし） | 輻輳制御が完全に無効 |
| yojimbo | message queue size | 1024 | 4096 | 4 倍 |
| litenetlib | PacketPoolSize | 1000 | max(1000,conns×2) | 動的スケール |

---

## 11. 要注意事項サマリ

### 危険（正確性・安定性リスク）

#### 改善済み

1. apex_rudp: シーケンス番号ラップアラウンドを `seq32_after()` half-space 比較に変更。seq 0 も送信側で skip
2. apex_rudp, mini_rudp: 未 ACK reliable が 10s 残った接続を inactive 化し、pending buffer を解放
3. kcp: `dead_link` 到達後の `kcp->state=-1` を adapter が切断扱いに反映
4. raknet/slikenet: SO_SNDBUF を 16KB から 256KB に上げ、SO_RCVBUF と対称化
5. yojimbo: send queue full は adapter が `CanSendMessage` で事前チェックし、切断ではなく -1 backpressure として扱う
6. udt4: UDT `BROKEN`/`CLOSED`/`NONEXIST` を切断扱いにし、adapter `out_pending` に byte cap/backpressure を追加
7. quiche: stream partial write の残りを adapter pending に保持し、cap 超過は -1 backpressure として扱う
8. lsquic: stream `pending_writes` に byte cap/backpressure を追加
9. gns/msquic: adapter inbox に message cap と oldest-drop diagnostics を追加
10. enet: reliable queue に byte cap/backpressure を追加
11. kcp: send queue に byte cap/backpressure を追加
12. raknet/slikenet: outgoing queue に byte cap/backpressure を追加
13. litenetlib: reliable outgoing queue に byte cap/backpressure を追加
14. coop_rudp: per-conn abort と max retransmit/idle timeout で crashed peer の reliable queue を解放

#### 残存

1. msquic: SO_RCVBUF に INT32_MAX を要求、SO_SNDBUF 未設定
2. raknet/slikenet: recv thread 停止バグ。 RAKPEER_USER_THREADED=1 でも生成→Shutdown で UAF。adapter は abandon で回避（意図的リーク）

### 意外な設計選択

1. coop_rudp: SRTT を計算するが RTO に使わない。 status 表示専用
2. coop_rudp: ホットパスゼロアロケーション。 14ライブラリ中唯一
3. mini_rudp: ACK ごとに 10byte 専用パケットを sendto。 ピギーバックなし
4. litenetlib: RTT 推定が cumulative mean（sum/count）。 EWMA ではなく全サンプル均等。3 秒ごとにリセット
5. enet: slow start なし。 初期 window が最大値から開始
6. coop_rudp: 初期送信レート = UINT32_MAX（~4.3Gbps）。 ramp-up なし
7. quiche BBRv2: beta=0.3（70% 削減）。 標準の 50% multiplicative decrease より攻撃的
8. raknet/slikenet: loss で cwnd=1MTU（Tahoe 動作）。 min MTU=576 で初期 window=548byte
9. lsquic: QUIC 実装で唯一 ACK を別 datagram で送信
10. gns: 1000conn でグローバルロック競合により delivery 崩壊（既定は暗号なし）。 poll group で軽減
