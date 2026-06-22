# 高性能通信ライブラリの実装・ベンチマークチェックリスト

対象読者: **通信ライブラリの実装者**と**ベンチマークアダプタの実装者**。
前半はライブラリ実装の汎用知識、後半はベンチマーク計測で踏みやすい罠。
各項目に本プロジェクトの14ライブラリ調査から得た具体例を付記。
`§X.Y` は [`dev-notes.md`](dev-notes.md) の事例。

---

## 1. ソケットと OS

### 1.1 バッファサイズ

- [ ] `SO_RCVBUF` / `SO_SNDBUF` を**明示設定**しているか。OS 既定値（Linux ~208KB）では高スループットに不足し、カーネルがパケットをサイレントにドロップする
  - 本プロジェクトの実態: raw_udp/mini_rudp/enet/kcp/quiche/lsquic は 256KB。coop_rudp/apex_rudp/gns は **4MB** に引き上げ。msquic/raknet/slikenet/litenetlib はライブラリ内部で管理し adapter からは不可視
  - gns は 256KB→4MB で 50 conns の delivery が改善（adapter コメントに記載）
- [ ] 設定後に `getsockopt` で**実値を確認**しているか。`net.core.rmem_max` / `wmem_max` の sysctl 上限を超えた値はサイレントにクランプされる
- [ ] バッファを大きくしすぎて**キューイング遅延**が増えていないか。kcp は 256KB→1MB でバッファブロートが起き、偽 RTO 再送で delivery が 0.78→0.52 に悪化（adapter コメントに A/B 結果あり）

### 1.2 I/O システムコール

- [ ] 高 PPS で `sendto`/`recvfrom` の **1 パケット 1 syscall が律速**になっていないか
  - 本プロジェクトの実態: raw_udp/mini_rudp/kcp/yojimbo は sendto/recvfrom（1pkt/syscall）。coop_rudp/apex_rudp は **sendmmsg/recvmmsg**（batch 256/64）。quiche/lsquic も recvmmsg/sendmmsg（batch 64）。enet は sendmsg/recvmsg（iovec 対応だが 1 パケット単位）
  - coop_rudp/apex_rudp が最高スループットを出せる理由の一つがバッチ I/O
- [ ] `io_uring` / `epoll` + non-blocking の選択肢を検討したか。UDT4 は独自 epoll（`UDT::epoll_wait`）を使用
- [ ] GSO/GRO（Generic Segmentation/Receive Offload）を活用できるか。QUIC 系（msquic, quiche, lsquic）では特に有効

### 1.3 fd とリソース上限

- [ ] conn ごとに socket を作る設計で **fd 上限**に引っかからないか
  - 本プロジェクトの実態: msquic（conn ごとに UDP socket）、raknet/slikenet（conn ごとに RakPeerInterface＝socket）、litenetlib（conn ごとに NetManager＝socket）が per-conn socket。raw_udp も client 側が per-conn。残り（mini_rudp, coop_rudp, apex_rudp, enet, kcp, gns, quiche, lsquic）は単一 fd で多重化
  - msquic は 1000 conn = 1000 fd で `ulimit -n` 1024 を超える（§1.7）
- [ ] per-conn socket は高 conns で **syscall 数が O(conns)** になる。単一 fd 多重化が可能ならそちらが有利（§5.8: mini_rudp は per-conn socket → 単一 fd 化で初めてまともに計測可能に）
- [ ] litenetlib は conn ごとに NetManager（=独立 socket）を作る設計。1 NetManager に複数 Connect すると同一 source endpoint からになり server 側で 1 peer に見えるため、per-conn NetManager が必須（adapter コメントに記載）

### 1.4 MTU とフラグメンテーション

- [ ] ペイロード + ヘッダが **Path MTU**（Ethernet: 1500 − IP 20 − UDP 8 = 1472B）内に収まるか
  - 本プロジェクトの実態: coop_rudp/apex_rudp は MTU 1200B。enet は MTU 1392B（`ENET_HOST_DEFAULT_MTU`）。kcp は MTU 1400B。gns/quiche/lsquic は 1200B（QUIC 系の標準）。yojimbo は 1024B fragment 化
- [ ] MTU 超過時の戦略を決めているか: **アプリ層分割** vs **IP フラグメンテーション**
  - enet: 1024×1024 fragments まで対応。yojimbo: 8KB パケットを 1024B fragments に分割。coop_rudp: 256 fragments/message まで
  - IP フラグメントはロス環境で急激に劣化する（1 片ロス → 全体再送）のでアプリ層分割が推奨

---

## 2. プロトコル設計

### 2.1 再送戦略

- [ ] **RTO 計算方法**を選んでいるか
  - kcp: Jacobson/Karels 標準（SRTT + 4×RTTVAR）、nodelay mode で min RTO 30ms、通常 100ms（ikcp.c）
  - mini_rudp: 固定 50ms RTO（単純だが RTT 変動に追従しない）
  - coop_rudp/apex_rudp: 固定 100ms RTO
  - enet: RTT ベースの指数平滑（peer->roundTripTime）
  - UDT4: 独自 CC フレームワーク経由
- [ ] **Fast Retransmit**（重複 ACK による早期再送）を実装しているか。RTO 待ちだけだとレイテンシが跳ねる
  - kcp: fastresend=2（重複 ACK 2 回で即再送、adapter 設定）
- [ ] **SACK**（Selective ACK）を実装しているか
  - coop_rudp/apex_rudp: 64-bit bitmap SACK。kcp: ACK + fastresend。enet: per-packet ACK。mini_rudp: per-packet ACK のみ
- [ ] 再送の上限（最大回数 or 最大遅延）を設けているか。無制限再送は障害時にリソースを消費し続ける

### 2.2 輻輳制御

- [ ] 輻輳制御の有無と方針を決めているか。ゲーム用途なら CC なしが適切な場合もある
  - kcp: AIMD window ベースだが `nocwnd=1` で**無効化可能**（adapter 既定で無効）
  - enet: Packet throttle（確率的ドロップ、`ENET_NO_THROTTLE=1` で無効化可能）
  - coop_rudp: EWMA RTT ベースの safe BPS tracking
  - QUIC 系: msquic=BBR、quiche=BBR2、lsquic=BBR
- [ ] **ランダムロス環境で CC が過度にレート抑制しないか**。Cubic は 1% ロスで cwnd を大幅に絞る（§1.9: UDT4 CUDTCC→CCC、msquic Cubic→BBR で改善）
- [ ] UDT4 の BenchCCC（adapter 独自）: period=1μs / window=64 の固定値でロスに無反応。デフォルト CUDTCC はバルク転送向けで benchmark のリアルタイム通信パターンに合わなかった

### 2.3 シーケンス番号

- [ ] シーケンス番号の**ビット幅とラップアラウンド**を正しく処理しているか
  - enet: **16-bit** per channel（65536 で一周、高レートでは数秒で到達）
  - yojimbo: **16-bit** packet sequence
  - kcp/mini_rudp/coop_rudp/apex_rudp: **32-bit**
  - gns: **64-bit**（netcode 経由）
  - 16-bit は高 PPS（50Hz × 1000 conn = 50K msg/s）で 1.3 秒で一周する計算になり、実用上注意が必要

### 2.4 フロー制御

- [ ] 受信側のバッファ溢れを防ぐ**受信ウィンドウ**を実装しているか
  - kcp: send_wnd + remote_wnd の combined window（既定 256、adapter 設定）
  - enet: 4096〜65536 の negotiated window
  - gns: token bucket（SendRateMax）+ SendBufferSize 32MB
  - mini_rudp: per-conn pending limit 65536
  - apex_rudp: per-conn pending limit 4096
- [ ] ゼロウィンドウ時のプローブで再開を検出できるか

### 2.5 チャネル分離と HoL blocking

- [ ] reliable と unreliable を**別チャネル**で送っているか
  - enet: channel 0=reliable、channel 1=unreliable+UNSEQUENCED（§1.3 で修正）
  - yojimbo: channel 0=RELIABLE_ORDERED、channel 1=UNRELIABLE_UNORDERED
  - gns: optional split_lanes（lane 0=reliable、lane 1=unreliable）
  - kcp: reliable は KCP ARQ、unreliable は raw UDP bypass（チャネルではなく経路分離）
  - coop_rudp: 複数 ordered/unordered チャネル対応
  - QUIC 系: stream=reliable、datagram=unreliable（プロトコルレベルで分離）
- [ ] HoL を「低負荷で出ないから OK」で判断していないか。**200conn 超で初めて顕在化する**（§1.2）
- [ ] HoL の測定: **pure unreliable p99 vs mixed unreliable p99 の差**。combined p99 は計測アーティファクト（§1.1）

---

## 3. スレッドモデルとメモリ

### 3.1 スレッド設計

- [ ] ライブラリの**スレッドモデル**を把握しているか
  - シングルスレッド（user-driven）: enet, kcp, mini_rudp, quiche, lsquic
  - 内部 worker スレッド: msquic（複数 worker）、gns（1 service thread）、raknet/slikenet（recv thread + update thread、ただし `RAKPEER_USER_THREADED=1` で無効化）
  - オプショナル: coop_rudp（async TX worker）、apex_rudp（TX worker ×1-8 + RX worker）、litenetlib（既定 2 thread/conn → manual mode で無効化）
- [ ] litenetlib は既定で **conn ごとに 2 OS スレッド**を生成する。1000 conn = 2000 スレッドで client が飽和する。`StartInManualMode` で無効化必須（adapter コメント）
- [ ] raknet/slikenet は `RAKPEER_USER_THREADED=1` でも**内部 recv thread が生成されるバグ**がある。Shutdown 時の UAF を回避するため adapter は RakPeer を abandon する（adapter コメント）
- [ ] blocking `send()` が 1 conn の stall を**全 conn に波及**させないか（§1.9: UDT4 `UDT_SNDSYN=false`）

### 3.2 Tick モデルの整合

- [ ] 固定ティック設計のライブラリを busy-poll ループから呼ぶとき、**adapter 側で tick cadence を制御**しているか
  - yojimbo: 100Hz に間引き（`next_send_packets_` で制御）。無制限だと空パケット洪水 → delivery 0.42→0.989（§1.9）
  - litenetlib: `UpdateTime` 15ms（~67Hz）、manual mode の logic cadence 5ms
  - kcp: `ikcp_update` の interval=5ms（adapter 設定）

### 3.3 メモリアロケーション

- [ ] 送受信の**ホットパスで heap allocation** を避けているか
  - allocation なし: raw_udp（stack buffer）、coop_rudp（全事前確保）
  - プール化: enet（size-segregated free-list、`ENET_POOL=1`）、mini_rudp（free_buffers_ freelist）、apex_rudp（thread-safe recycled_buffers_）、yojimbo（message factory allocator）
  - 毎回 alloc: kcp（segment ごとに malloc）、UDT4（deque/vector）、msquic（SendCtx per send）、quiche/lsquic（frame vector per send）
  - litenetlib: `ArrayPool` で GC 圧力を軽減
- [ ] **per-conn メモリ使用量**を見積もっているか。dedup window + 再送バッファ + 受信順序バッファの合計が conn 数に比例する
  - mini_rudp: per-conn `PendingSend` map + `SlidingDedupWindow`
  - apex_rudp: per-conn `deque<PendingSend>` + RecvSackWindow
  - coop_rudp: per-conn flow/channel/sequenced state（全事前確保、max 4096 conn）

### 3.4 マネージド言語の注意点

- [ ] **GC ポーズ**がレイテンシ p99 に影響しないか
  - litenetlib: warmup 5 秒で JIT/GC を安定化。`PacketPoolSize` を `max(1000, conns*2)` に自動スケール（枯渇すると GC 圧力増大）。`ArrayPool` で receive buffer の churn を削減
- [ ] conn ごとのスレッド生成がスレッドプールや GC に過度な負荷を与えないか（litenetlib: 前述の manual mode）

---

## 4. 暗号

- [ ] 暗号のオン/オフを**選択できるか**把握しているか
  - 暗号必須: msquic（TLS 1.3）、quiche（TLS 1.3）、lsquic（TLS 1.3）、yojimbo（libsodium）、gns（OpenSSL/libsodium、ただし `Unencrypted=3` で無効化可能）
  - 暗号なし: raw_udp, mini_rudp, coop_rudp, apex_rudp, enet, kcp, UDT4
  - オプショナル: raknet/slikenet（`InitializeSecurity()` で有効化可能、bench では無効）、litenetlib（transport encrypted だが adapter は "off" 表記）
- [ ] **per-packet 暗号**が CPU を支配しないか。msquic は per-datagram crypto で media_relay c50 の server CPU ~198% 飽和（§1.8）。gns は同条件で 117%
- [ ] 暗号ありのライブラリを比較する場合、**暗号込みの性能として正しく評価**しているか（§5.7）

---

## 5. コネクション管理

- [ ] `connect()` がブロッキングの場合、高 conns で**接続フェーズだけで秒単位**になることを把握しているか（§1.9: UDT4 で conn × RTT）
- [ ] `close()` が内部 worker と**競合して deadlock しないか**
  - msquic: close() は no-op、`_Exit(0)` でプロセスごと終了（§5.11）
  - raknet/slikenet: Shutdown() が内部 recv thread との UAF を起こすため abandon（adapter コメント）
- [ ] **接続 ID の衝突**を処理しているか
  - raknet/slikenet: GUID が `gettimeofday()` マイクロ秒値で、同時接続で衝突する。adapter は `ID_ALREADY_CONNECTED` を検知して RakPeer を再生成（adapter コメント）

---

## 6. ベンチマーク計測の落とし穴

### 6.1 ヒストグラム分離

- [ ] mixed traffic のレイテンシを**チャネル別ヒストグラム**で記録しているか。combined histogram では reliable の retx tail が unreliable の p99 に混入し偽の結論を導く（§1.1）

### 6.2 計測窓

- [ ] client と server の**計測窓が重なっている**か。ramp-up を server 寿命に含めないと一定割合の delivery が構造的に欠損する（§1.7: `delivery ≈ (duration − ramp) / duration`）
- [ ] warmup のデータが計測に混入しないか（§5.10: measurement bit）
- [ ] tail 時間が reliable retransmit の完了に十分か

### 6.3 統計

- [ ] 結論に使う数字は **N ≥ 3** で中央値 + IQR か（§1.5: N=1 は ±10〜20% 振れる）
- [ ] **Coordinated Omission** を認識しているか。固定間隔送信 + レスポンス待ちだと遅延リクエストの後続が待ち行列に入らず p99 が過小評価される
- [ ] delivery ratio の定義（往復 echo vs 片道配信率）を明記しているか（§5.1）

### 6.4 CPU 計測

- [ ] busy-spin idle で CPU 100% を「飽和」と誤読していないか（§1.6）
- [ ] シングルスレッド lib（enet, kcp: 上限 ~100%）とマルチスレッド lib（gns, msquic, litenetlib: SMT2 で ~200%）を**スレッドモデル併記**で比較しているか（§5.2）
- [ ] 平均 CPU だけでなく**瞬間ピーク**も見ているか

### 6.5 メモリ計測

- [ ] 計測ハーネスのオーバーヘッド（dedup バッファ、histogram bins）がライブラリの RSS に混じっていないか。**server 側 RSS** で評価する（§5.4）

### 6.6 ネットワークエミュレーション

- [ ] netem の **packet limit** を十分大きく設定しているか（既定 1000 → サイレントドロップ。§1.6）
- [ ] loopback netem は **send/recv 両方**を通る（片道 25ms → RTT ~50ms、loss 1% → 実効 ~2%）

---

## 7. 負荷生成の品質

- [ ] 負荷生成器が**意図したレートを出し切れているか検証する仕組み**があるか（§1.9: attempted_ratio で検出）
- [ ] pacing 精度閾値を**送信間隔の比率**で設定しているか（§1.4: `max(100μs, interval/10)`）
- [ ] multi-process 生成で**各プロセスの計測窓が揃っている**か（§1.7）

---

## 8. 診断パターン

| 症状 | まず疑うこと | 事例 |
|------|-------------|------|
| c1 から delivery が低い | adapter/harness バグ（API 誤用、窓ズレ） | §1.9: yojimbo, udt4, msquic |
| delivery が `(duration−X)/duration` | client/server の active window ズレ | §1.7: msquic ramp 未加算 |
| 手動では動くが CI で落ちる | fd 上限、CWD、権限の環境差 | §1.7: systemd-run LimitNOFILE |
| CPU 100% だが delivery は低い | busy-spin idle の誤読 or tick 洪水 | §1.6, §1.9: yojimbo |
| バッファ増で性能悪化 | バッファブロート → 偽 RTO | kcp 256KB→1MB で delivery 0.78→0.52 |
| 高 conns で突然 invalid | 負荷生成側の律速 | §1.9: client 4proc→8proc で解消 |

---

## 付録: 14ライブラリの設計一覧

| lib | socket buf | fd model | I/O batch | threading | CC | seq bits | encryption | hot-path alloc |
|-----|-----------|----------|-----------|-----------|-----|---------|-----------|----------------|
| raw_udp | 256KB | per-conn (client) | sendto/recvfrom | single | なし | — | なし | なし |
| mini_rudp | 256KB | single fd | sendto/recvfrom | single | なし | 32-bit | なし | pool |
| coop_rudp | 4MB | single fd | **sendmmsg/recvmmsg** | optional TX worker | EWMA BPS | 32-bit | なし | **全事前確保** |
| apex_rudp | 4MB | single+shard | **sendmmsg/recvmmsg** | TX×1-8 + RX worker | SACK window | 32-bit | なし | pool (thread-safe) |
| enet | 256KB | single fd | sendmsg/recvmsg | single | packet throttle | **16-bit** | なし | pool |
| kcp | 256KB | single fd | sendto/recvfrom | single | AIMD (無効化可) | 32-bit | なし | malloc/free |
| slikenet | lib 内部 | **per-conn** | lib 内部 | user-driven | lib 内部 | lib 内部 | optional | lib 内部 |
| raknet | lib 内部 | **per-conn** | lib 内部 | user-driven | lib 内部 | lib 内部 | optional | lib 内部 |
| udt4 | lib 内部 | per-conn | UDT API (epoll) | single | BenchCCC (固定) | lib 内部 | なし | malloc/vector |
| yojimbo | netcode 経由 | single fd | sendto/recvfrom | single (100Hz) | なし | **16-bit** | **libsodium 必須** | factory pool |
| gns | 4MB | single fd | lib 内部 | 1 service thread | lib 内部 | 64-bit | OpenSSL (optional) | lib 内部 |
| msquic | lib 内部 | **per-conn** | lib 内部 | internal workers | **BBR** | lib 内部 | **TLS 1.3 必須** | SendCtx/send |
| quiche | 256KB | single fd | **recvmmsg/sendmmsg** | single | **BBR2** | lib 内部 | **TLS 1.3 必須** | vector/send |
| lsquic | 256KB | single fd | **recvmmsg/sendmmsg** | single | **BBR** | lib 内部 | **TLS 1.3 必須** | vector/send |
| litenetlib | lib 内部 | **per-conn** | PollEvents | manual mode (既定) | lib 内部 | lib 内部 | transport | ArrayPool |
