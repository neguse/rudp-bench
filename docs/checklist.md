# 高性能通信ライブラリの実装・ベンチマークチェックリスト

対象読者: **通信ライブラリの実装者**と**ベンチマークアダプタの実装者**。
前半はライブラリ実装の汎用知識、後半はベンチマーク計測で踏みやすい罠。
`§X.Y` は本プロジェクトの具体事例（[`dev-notes.md`](dev-notes.md)）。

---

## 1. ソケットと OS

### 1.1 バッファサイズ

- [ ] `SO_RCVBUF` / `SO_SNDBUF` を明示設定しているか。OS 既定値（Linux 208KB 等）は高スループットに不足し、カーネルがパケットをサイレントにドロップする
- [ ] 受信バッファが小さすぎて **recv が間に合わないとカーネルが UDP パケットを捨てる**ことを把握しているか。`netstat -su` の "receive buffer errors" や `/proc/net/udp` の drops 列で確認できる
- [ ] 逆にバッファを巨大にしすぎると**キューイング遅延**が増える。スループット重視 vs レイテンシ重視でトレードオフがある
- [ ] `net.core.rmem_max` / `net.core.wmem_max` の sysctl 上限を超えた `setsockopt` はサイレントにクランプされる。設定後に `getsockopt` で実値を確認しているか

### 1.2 バッチ I/O

- [ ] 高 PPS では `sendto` / `recvfrom` の 1 パケット 1 syscall が律速になる。`sendmmsg` / `recvmmsg` でバッチ化を検討したか
- [ ] `io_uring` / `epoll` + non-blocking の選択肢を検討したか。busy-poll（`SO_BUSY_POLL`）はレイテンシを下げるが CPU を食う
- [ ] GSO（Generic Segmentation Offload）/ GRO でカーネル内バッチ処理を活用できるか確認したか（QUIC 系で特に有効）

### 1.3 fd とリソース上限

- [ ] conn ごとに socket を作る設計で **fd 上限**に引っかからないか。`ulimit -n` 既定 1024 は 1000 conn で枯渇する（§1.7: msquic）
- [ ] 単一 fd で多重化できるなら per-conn socket より有利。per-conn socket は高 conns で syscall 数が O(conns) になる（§5.8: mini_rudp）
- [ ] サンドボックスやコンテナ環境では**対話シェルと fd 上限が異なる**。動作確認環境と本番/CI 環境の差に注意（§1.7: systemd-run 既定 1024 vs shell 524288）

### 1.4 MTU とフラグメンテーション

- [ ] ペイロード + ヘッダが **Path MTU**（Ethernet 1500B − IP 20B − UDP 8B = 1472B）を超えないか
- [ ] 超える場合、IP フラグメンテーションに頼るのか、アプリ層で分割するのか方針を決めているか。IP フラグメントは 1 片でもロスすると全体再送になり、ロス環境で急激に劣化する
- [ ] `DF` (Don't Fragment) ビットの設定方針を決めているか。Path MTU Discovery を使うなら ICMP Fragmentation Needed の処理が必要

---

## 2. プロトコル設計

### 2.1 信頼性と再送

- [ ] 再送タイマ（RTO）の計算方法を選んでいるか。固定値は単純だが RTT 変動に追従しない。Karn/Partridge + Jacobson/Karels の標準的な SRTT/RTTVAR ベースが無難
- [ ] 再送の上限（最大再送回数 or 最大遅延）を設けているか。無制限再送はコネクション障害時にリソースを食い続ける
- [ ] NACK ベース vs ACK ベース vs SACK の選択理由を説明できるか。NACK はロス検出が速いが受信側に状態が必要。SACK は帯域効率が良いがヘッダが大きい
- [ ] Fast Retransmit（重複 ACK による早期再送）を実装しているか。RTO 待ちだけだとレイテンシが跳ねる

### 2.2 順序制御

- [ ] reliable ordered / reliable unordered / unreliable ordered / unreliable unordered の**どの組み合わせをサポートするか**明確か
- [ ] reliable ordered で後続パケットが先着したときの**受信バッファの上限**を設けているか。無制限だとロス時にメモリが膨らむ
- [ ] シーケンス番号の**ビット幅とラップアラウンド**を正しく処理しているか。16bit seq は 65536 で一周し、高レートでは数秒で到達する

### 2.3 輻輳制御

- [ ] 輻輳制御の有無と方針を決めているか。ゲーム用途なら CC なし（固定レート送信）が適切な場合もある
- [ ] CC ありの場合、**ランダムロス環境で過度にレート抑制しないか**。Cubic は 1% ロスで cwnd を大幅に絞る。BBR やレート制御ベースの CC の方がランダムロスに強い（§1.9: UDT4, msquic）
- [ ] CC のウィンドウサイズ初期値は適切か。小さすぎるとスロースタートが長い、大きすぎるとバースト

### 2.4 フロー制御

- [ ] 受信側のバッファが溢れないよう**受信ウィンドウ（flow control）** を実装しているか
- [ ] flow control が sender を完全に止めるとき、**ゼロウィンドウプローブ**で再開を検出できるか

### 2.5 コネクション管理

- [ ] ハンドシェイクの往復数は最小限か（1-RTT? 0-RTT?）。3-way は堅実だが高レイテンシ環境で接続が遅い
- [ ] `connect()` がブロッキングの場合、高 conns で**接続フェーズだけで秒単位になる**ことを把握しているか（§1.9: UDT4 で conn 数 × RTT）
- [ ] Keepalive / heartbeat の間隔と判定基準を設定可能にしているか
- [ ] `close()` が内部 worker スレッドと**競合して deadlock / double-free しないか**。非同期 teardown が必要なライブラリでは graceful close のタイムアウトを用意する（§5.11: msquic）

---

## 3. スレッドモデルとメモリ

### 3.1 スレッド設計

- [ ] ネットワーク I/O スレッドとアプリロジックスレッドの分離方針を決めているか
- [ ] API がスレッドセーフか、呼び出し側でロックが必要かを明記しているか
- [ ] blocking `send()` が 1 conn の stall を**同一スレッドの全 conn に波及**させないか。non-blocking + per-conn queue で隔離する（§1.9: UDT4）
- [ ] 固定ティック設計のライブラリが busy-poll ループから呼ばれたとき、**想定外の呼び出し頻度**で壊れないか（§1.9: yojimbo の空パケット洪水）

### 3.2 メモリアロケーション

- [ ] 送受信のホットパスで **`malloc` / `new` / GC アロケーション**を避けているか。パケットごとにヒープ確保すると、高 PPS でアロケータ or GC がボトルネックになる
- [ ] パケットバッファのプール（事前確保 + 再利用）を実装しているか
- [ ] per-conn のメモリ使用量を見積もっているか。dedup ウィンドウ、再送バッファ、受信順序バッファの合計が conn 数に比例して増える

### 3.3 マネージド言語（C# / Java / Go 等）

- [ ] GC ポーズがレイテンシの p99 に影響しないか確認したか。世代別 GC のフル GC は数十〜数百 ms 止まりうる
- [ ] ピン留め（`fixed` / `Unsafe` / off-heap buffer）で GC の移動を防いでいるか。native interop でバッファアドレスが動くとクラッシュする
- [ ] アロケーション量を `dotnet-counters` / JFR / pprof 等で計測し、ホットパスの alloc rate を確認したか

### 3.4 ゼロコピー

- [ ] ユーザーバッファ → カーネルバッファ → NIC のコピー回数を把握しているか
- [ ] `MSG_ZEROCOPY`（Linux 4.18+）や `io_uring` の fixed buffer で送信コピーを削減できるか検討したか
- [ ] ゼロコピーはスループット向上するが completion 通知のオーバーヘッドがあり、小パケットでは逆効果になりうる

---

## 4. チャネル設計と HoL blocking

- [ ] reliable と unreliable を**別チャネル（または別キュー）** で送っているか。同一チャネルだと reliable の再送が unreliable を head-of-line blocking する（§1.3）
- [ ] unreliable 送信に**順序保証解除フラグ**を付けているか（例: ENet `ENET_PACKET_FLAG_UNSEQUENCED`）。ordered unreliable はライブラリ内部の sequence 管理で reliable と干渉しうる
- [ ] HoL の有無を「低負荷で出ないから OK」で判断していないか。**高負荷（200conn〜）で初めて顕在化する**（§1.2: mini_rudp は c10 で ±1%、c200 で 13〜19 倍）
- [ ] HoL の測定方法: **pure unreliable の p99 vs mixed traffic の unreliable p99 の差**。combined p99（reliable + unreliable 混合）は計測アーティファクトで結論が反転する（§1.1）

---

## 5. 暗号と認証

- [ ] 暗号のオン/オフを選択できるか。QUIC のように TLS 必須の場合はその旨を明記し、**暗号込みの性能として評価する**（§5.7）
- [ ] 暗号処理が per-packet か per-connection か。per-packet crypto は高 conns × 高 PPS で CPU を支配する（§1.8: msquic media_relay で server CPU 飽和）
- [ ] 暗号のバッチ処理（AES-NI / ChaCha20 の SIMD 化、複数パケットの一括暗号化）を活用できるか
- [ ] handshake の暗号オーバーヘッド（鍵交換、証明書検証）が接続確立時間に与える影響を計測したか

---

## 6. ベンチマーク計測の落とし穴

### 6.1 ヒストグラム分離

- [ ] mixed traffic のレイテンシを**チャネル別ヒストグラム**で記録しているか。combined histogram では reliable の retx tail（200ms+）が unreliable の p99 に混入し偽の結論を導く（§1.1）

### 6.2 計測窓（measurement window）

- [ ] client と server の**計測窓が重なっている**か。ramp-up 時間を server の寿命に含めないと、一定割合の delivery が構造的に欠損する（§1.7）
- [ ] warmup 期間のデータが計測に混入しないか。payload に measurement-window bit を入れてフィルタする（§5.10）
- [ ] tail 時間が reliable retransmit の完了に十分か

### 6.3 統計

- [ ] 結論に使う数字は **N ≥ 3** で中央値 + IQR を取っているか。N=1 は ±10〜20% 振れる（§1.5）
- [ ] **Coordinated Omission** を認識しているか。固定間隔で送信してレスポンスを待つ設計だと、遅延が長いリクエストの後続が「待ち行列に入らない」ため p99 が過小評価される
- [ ] delivery ratio の定義（往復 echo 成功率 vs 片道配信率）を明記しているか（§5.1）

### 6.4 CPU 計測

- [ ] busy-spin idle で CPU 100% を「飽和」と誤読していないか（§1.6）
- [ ] シングルスレッド lib（上限 ~100%）とマルチスレッド lib（SMT2 で ~200%）を比較するとき**スレッドモデルを併記**しているか（§5.2）
- [ ] 平均 CPU だけでなく**瞬間ピーク**も見ているか。平均に薄まるスパイクが本当のボトルネック

### 6.5 メモリ計測

- [ ] 計測ハーネス自体のメモリオーバーヘッド（dedup バッファ、ヒストグラム bins）がライブラリの RSS に混じっていないか。**ライブラリのメモリ効率は server 側で見る**（§5.4）

### 6.6 ネットワークエミュレーション

- [ ] netem の **packet limit** を十分大きく設定しているか。既定 1000 は高 conns で BDP 超過 → サイレントドロップ（§1.6）
- [ ] loopback netem は **send/recv 両方**を通る。片道 delay=25ms → RTT ~50ms、loss=1% → 実効 ~2%
- [ ] CPU 隔離しても**メモリ帯域・L3 キャッシュは物理共有**。絶対値より相対順位で判断する（§2）

---

## 7. 負荷生成の品質

- [ ] 負荷生成器が**意図したレートを出し切れているか検証する仕組み**があるか。生成側が律速だとライブラリの限界ではなく生成器の限界を測る（§1.9）
- [ ] pacing 精度の閾値を**送信間隔の比率**で設定しているか。固定閾値は低レートで厳しすぎ、高レートで甘すぎる（§1.4）
- [ ] multi-process 生成で**各プロセスの計測窓が揃っている**か（§1.7）
- [ ] ワークロードの種類（broadcast vs echo 等）で最適な並列度が異なることを把握しているか（§1.9）

---

## 8. 診断パターン

### 8.1 「c1 から壊れている」→ adapter / harness バグ

最小接続数で delivery が低いのはライブラリ起因として不自然。API 誤用かハーネスのライフサイクルバグを先に疑う（§1.9: yojimbo, udt4, msquic は全て harness/adapter 側の問題だった）

### 8.2 「delivery が duration に比例して欠ける」→ 窓ズレ

`delivery ≈ (duration − X) / duration` のパターンなら、client/server の active window のズレ。ライブラリのチューニングに走る前に window の重なりを確認する（§1.7）

### 8.3 「手動では動くがスクリプト/CI で落ちる」→ 環境差

fd 上限、CWD、ユーザー権限、cgroup 制約など、対話シェルと自動実行環境の暗黙の差（§1.7）
