# 高性能通信ライブラリの実装・ベンチマークチェックリスト

対象読者: **通信ライブラリの実装者**と**ベンチマークアダプタの実装者**。
rudp-bench で実際に踏んだ罠から抽出した、フレームワーク非依存の知見集。
各項目の末尾 `§X.Y` は [`dev-notes.md`](dev-notes.md) の事例。

---

## 1. チャネル設計と HoL blocking

- [ ] reliable と unreliable を**別チャネル（または別キュー）** で送っているか。同一チャネルだと reliable の再送が unreliable を head-of-line blocking する（§1.3: ENet channel 0 共用バグ）
- [ ] unreliable 送信に**順序保証解除フラグ**を付けているか（例: ENet `ENET_PACKET_FLAG_UNSEQUENCED`）。ordered unreliable はライブラリ内部で sequence 管理し、reliable と干渉しうる
- [ ] HoL blocking の有無を「低負荷で出ないから OK」で判断していないか。低 conns では出ず、**高負荷（200conn〜）で初めて顕在化する**（§1.2: mini_rudp は c10 で ±1%、c200 で 13〜19 倍）
- [ ] HoL の測り方: **pure unreliable の u99 vs mixed traffic の u99 の差**。combined p99（reliable + unreliable 混合ヒストグラム）は reliable の retx tail に汚染されて結論が反転する（§1.1）

---

## 2. ライブラリ API の正しい使い方

### 2.1 tick モデルの整合

- [ ] ライブラリが固定ティック設計（yojimbo 等）の場合、**呼び出し側で tick cadence を守っている**か。busy-poll ループから無制限に `SendPackets()` / `AdvanceTime()` すると空パケット洪水 → CPU 飽和 → 受信側崩壊（§1.9: yojimbo を 100Hz に間引いて delivery 0.42→0.989）
- [ ] ライブラリのドキュメントが想定する呼び出し頻度（10Hz? 60Hz? 自由?）を確認したか

### 2.2 輻輳制御とロス耐性

- [ ] ライブラリの輻輳制御が**ランダムロス環境で過度にレート抑制しないか**確認したか。バルク転送向け CC（Cubic 等）は 1% ランダムロスで cwnd を絞り、リアルタイム通信の offered rate を維持できない（§1.9: UDT4 CUDTCC → CCC、msquic Cubic → BBR）
- [ ] CC アルゴリズムを選択できるライブラリなら、ユースケースに合った CC を選んでいるか（バルク転送向け vs リアルタイム向け）

### 2.3 ブロッキング API

- [ ] `connect()` がブロッキングのとき、高 conns で**接続フェーズだけでタイムアウトしないか**。conn 数 × RTT の積算が想定外に大きくなる（§1.9: UDT4 で 100conn = 数百 ms → スクリプトの slack を使い切って kill）
- [ ] blocking `send()` が 1 conn の stall を**同一プロセスの全 conn に波及**させないか。非ブロッキング化 + per-conn queue で隔離する（§1.9: UDT4 `UDT_SNDSYN=false`）

### 2.4 暗号と公平比較

- [ ] 暗号が API で無効化できないライブラリ（gns, QUIC 系）はその旨を記録し、**暗号込みの性能として評価**しているか（§5.7）
- [ ] ライブラリ間で unreliable の意味が異なる場合（LiteNetLib `Unreliable` vs ENet `UNSEQUENCED` 等）、内部の sequence 処理の違いを把握しているか（§5.6）

---

## 3. リソース管理とスケーラビリティ

- [ ] conn ごとに socket を作るライブラリで、**fd 上限**に引っかからないか。1000 conn = 1000 fd で、プロセスの `ulimit -n` やサンドボックスの既定値（1024 等）を超える（§1.7: msquic）
- [ ] conn ごとに別 socket → 高 conns で **syscall 過多**にならないか。単一 fd で多重化できるなら切り替えを検討（§5.8: mini_rudp は per-conn socket → 単一 fd 化で初めてまともに計測可能に）
- [ ] `close()` / shutdown が**内部 worker スレッドと競合して deadlock / double-free しないか**。非同期 teardown が必要なライブラリでは graceful close のタイムアウトと強制終了の両方を用意する（§5.11）
- [ ] 実行環境が変わると fd 上限やスレッドスケジューリングが変わる。**対話シェルでは動くがサンドボックス/コンテナ経由で落ちる**パターンに注意（§1.7: systemd-run 既定 LimitNOFILE=1024）

---

## 4. 計測の落とし穴

### 4.1 ヒストグラムの分離

- [ ] mixed traffic（reliable + unreliable）のレイテンシを**チャネル別ヒストグラム**で記録しているか。combined histogram では reliable の retx tail（200ms+）が unreliable の p99 に混入し、「全ライブラリに HoL あり」という偽の結論を導く（§1.1）
- [ ] 新しい計測指標を追加するとき「mixed で値が変わるか？ 実体か計測アーティファクトか？」を先に問うているか

### 4.2 計測窓（measurement window）

- [ ] client と server の**計測窓が重なっている**か。ramp-up 時間を server の寿命計算に含めないと、client がまだ送信中に server が退場する → 一定割合の delivery 欠損が出る（§1.7: msquic で `delivery ≈ (duration − ramp) / duration` の理論値と完全一致）
- [ ] warmup 中に送った echo が計測期間に戻ってきて **delivery > 1.0** にならないか。payload に measurement-window bit を入れてフィルタする（§5.10）
- [ ] 計測後の tail 時間が **reliable retransmit の完了に十分**か。短すぎると in-flight の reliable メッセージが未達扱いになる

### 4.3 統計

- [ ] 結論に使う数字は **N ≥ 3** 取って中央値 + IQR か。同じ条件で N=1 を 10 回回すと ±10〜20% 振れる。「lib X > lib Y」は差が IQR を超えたときだけ言える（§1.5）

### 4.4 delivery ratio の定義

- [ ] delivery ratio が何を測っているか明記しているか。**往復 echo 成功率**（片道でどちらかが落ちれば未達）と**片道配信率**は別物。片道を見るなら forward / return を分離する（§5.1）

---

## 5. CPU 計測の罠

- [ ] busy-spin idle（`idle=spin`）で **CPU 100% を「飽和」と誤読**していないか。無負荷でも 100% になる。CPU 律速の診断には adaptive idle 必須（§1.6）
- [ ] シングルスレッド lib（CPU 上限 ~100%）とマルチスレッド lib（SMT2 なら ~200%）を比較するとき、**スレッドモデルを併記**しているか。CPU% の絶対値だけでは比較できない（§5.2）
- [ ] CPU 計測の warmup 除外をしているか。起動直後の初期化スパイクが平均を歪める
- [ ] 平均 CPU だけでなく**瞬間ピーク**も見ているか。平均に薄まるスパイクが実際のボトルネック

---

## 6. 負荷生成側の品質

- [ ] 負荷生成器が**意図した送信レートを出し切れているか検証する仕組み**があるか。生成側が律速していると、ライブラリの限界ではなく生成器の限界を測ってしまう（§1.9: `attempted_ratio < 1.0` で client 律速を検出）
- [ ] pacing の精度閾値を**絶対値でなく送信間隔の比率**で設定しているか。固定閾値だと低レートで厳しすぎ、高レートで甘すぎる（§1.4: `max(100us, interval/10)`）
- [ ] multi-process 負荷生成で、**各プロセスの計測窓が揃っている**か。プロセスごとに conn 数が異なると ramp 消費時間がずれ、delivery が構造的に欠損する（§1.7）
- [ ] ワークロードの種類（broadcast vs echo）で最適な並列度が異なることを把握しているか。一律に増やすと thread の多い lib で受信側 delivery が落ちる場合がある（§1.9: echo=8proc, broadcast=4proc）

---

## 7. 診断パターン

### 7.1 「c1 から壊れている」→ adapter / harness バグ

- [ ] 最小接続数（c1）で delivery が低い場合、**ライブラリ起因として不自然**。adapter の API 誤用か harness のライフサイクルバグを先に疑う（§1.9: yojimbo, udt4, msquic は全て harness / adapter 側の問題だった）

### 7.2 「delivery が duration に比例して欠ける」→ 窓ズレ

- [ ] delivery が `(duration − X) / duration` のパターンなら、client/server の **active window のズレ**をまず疑う。ライブラリのチューニングに走る前に connect_ms と window の重なりを確認する（§1.7）

### 7.3 結果の解釈

- [ ] 高 conns の RTT には**ハーネスの recv drain 遅延**が混じりうる。1 tick に大量メッセージをドレインすると後方メッセージの RTT が水増しされる（§5.5）
- [ ] client 側のメモリ使用量には**計測ハーネスのオーバーヘッド**（dedup バッファ, ヒストグラム bins）が混じる。ライブラリのメモリ効率は server 側で見る（§5.4）

---

## 8. ネットワークエミュレーションの注意点

- [ ] netem / tc の **packet limit** を十分大きく設定しているか。既定 1000 packets は高 conns で BDP を超え、サイレントにドロップする（§1.6: `limit=100000` に修正）
- [ ] loopback で netem を使うとき、**send と recv の両方で netem を通る**ことを把握しているか。片道 25ms 指定 → 実効 RTT ~50ms、loss 1% 指定 → 実効 `1-(1-0.01)² ≈ 2%`
- [ ] CPU は隔離できても**メモリ帯域・L3 キャッシュは物理共有**。他プロセスのキャッシュ汚染で RTT p99 が跳ねる。絶対値より相対順位で判断する（§2）
