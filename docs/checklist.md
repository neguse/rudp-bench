# 高性能通信ライブラリの実装・ベンチマークチェックリスト

rudp-bench プロジェクトで実際に踏んだ罠と得た知見から構成。
各項目の末尾 `§X.Y` は [`dev-notes.md`](dev-notes.md) の該当セクション。

---

## 1. ライブラリ実装（Adapter）

### 1.1 API の正しい使い方

- [ ] reliable と unreliable を**別チャネル**で送っているか。同一チャネルだと reliable の retx が unreliable を head-of-line blocking する（§1.3: ENet channel 0 共用バグ）
- [ ] unreliable 送信に**順序保証解除フラグ**を付けているか（例: `ENET_PACKET_FLAG_UNSEQUENCED`）
- [ ] 暗号が API で無効化できないライブラリ（gns, msquic/QUIC）はその旨を記録し、**暗号込みの性能として評価**しているか（§5.7）
- [ ] ライブラリ固有の送受信単位（LiteNetLib `Unreliable` vs ENet `UNSEQUENCED` 等）の**意味の違い**を把握しているか（§5.6）

### 1.2 Tick モデルの整合

- [ ] 固定ティック設計のライブラリ（yojimbo 等）を busy-poll harness に繋ぐとき、**adapter 側で tick cadence を守っている**か。無制限 poll → 空パケット洪水 → CPU 飽和（§1.9: yojimbo 100Hz 修正）
- [ ] `SendPackets()` / `AdvanceTime()` の呼び出し頻度がライブラリの設計想定と合っているか

### 1.3 ブロッキングと輻輳制御

- [ ] `connect()` がブロッキングのライブラリで、高 conns 時に**接続だけでタイムアウト**しないか（§1.9: UDT4、conn 数に比例したスラック）
- [ ] 輻輳制御がベンチ条件（ランダムロス）で**過度にレート抑制**しないか確認したか（§1.9: UDT4 CUDTCC → CCC 変更、msquic Cubic → BBR）
- [ ] blocking send が 1 conn の stall を全 conn に波及させないか（§1.9: UDT4 `UDT_SNDSYN=false` + per-conn queue）

### 1.4 リソース管理

- [ ] conn ごとに socket を作るライブラリで fd 上限に引っかからないか（§1.7: msquic `LimitNOFILE` 1024 問題）
- [ ] conn ごとに別 socket を使う実装が高 conns で syscall 過多にならないか（§5.8: mini_rudp 単一 fd 多重化）
- [ ] `close()` が deadlock / double-free しないか。同期 teardown が内部 worker と競合する場合の対処（§5.11: msquic no-op close + `_Exit(0)`）

---

## 2. ベンチマーク設計

### 2.1 ワークロード設計

- [ ] **複数プロファイル**を用意しているか（broadcast unreliable / mixed reliable+unreliable / pure reliable / 高頻度小パケット 等）。1 プロファイルでは強み・弱みが隠れる
- [ ] 各プロファイルの conn 数スイープが**ブレイクポイント**を超える範囲まで含んでいるか（壊れない範囲だけ測っても capacity はわからない）
- [ ] ペイロードサイズが UDP MTU (≈1472B) 内に収まっているか。超過時の挙動（フラグメント or ドロップ）を把握しているか

### 2.2 統計的妥当性

- [ ] **N ≥ 3** で測って中央値 + IQR を取っているか。N=1 は shape だけ、「lib X > lib Y」は差が IQR を超えたときのみ（§1.5）
- [ ] Adaptive N を使う場合でも、ブレイクポイント付近はフル N で回しているか
- [ ] delivery ratio の閾値（例: 0.95）と valid 判定条件（例: 2/3 runs valid）を明示しているか

### 2.3 計測窓とライフサイクル

- [ ] client と server の **active window が重なっている**か。ramp-up 時間が server 寿命に含まれているか（§1.7: msquic canonical 崩壊の正体）
- [ ] warmup 期間の echo が計測に混入しないか（§5.10: measurement bit による除外）
- [ ] 計測後の tail 時間が reliable retransmit の完了に十分か（§1.6: `--tail-ms`）
- [ ] multi-proc client の ramp 窓がプロセス間で揃っているか（§1.7: 全 proc が ramp 窓全体を消費してから計測開始）

---

## 3. 計測指標

### 3.1 Delivery Ratio

- [ ] delivery_ratio が**往復 echo 成功率**であることを明記しているか（片道配信率とは別物。§5.1）
- [ ] 片道を分析するなら `forward_delivery_ratio` / `server_echo_accept_ratio` / `return_delivery_ratio` を使っているか
- [ ] channel 別（`_r` / `_u`）で分離しているか

### 3.2 RTT

- [ ] reliable と unreliable の RTT を**別ヒストグラム**で記録しているか。combined histogram は混合シナリオで結論を反転させる（§1.1: combined RTT 汚染）
- [ ] HoL blocking の定義は「pure-u u99 vs mixed u99 の差」であって combined p99 ではないことを守っているか
- [ ] 高 conns で `recv_drained_p99` が大きい場合に RTT 絶対値を割り引いているか（§5.5: harness drain 遅延）

### 3.3 CPU

- [ ] `idle=spin` の CPU 100% を飽和と誤読していないか。CPU 律速の診断には `idle=adaptive` 必須（§1.6）
- [ ] シングルスレッド lib（上限 ~100%）とマルチスレッド lib（SMT2 で上限 ~200%）を**スレッドモデル併記**で比較しているか（§5.2）
- [ ] CPU 計測が warmup を除外しているか
- [ ] 瞬間ピーク（`cpu_pct_peak`）も見ているか。平均に薄まるスパイクを捕捉

### 3.4 メモリ

- [ ] **server RSS** をライブラリのメモリ効率指標にしているか。client RSS は harness オーバーヘッド（dedup ring, histogram bins）が混じる（§5.4）

---

## 4. 環境隔離と再現性

### 4.1 CPU 隔離

- [ ] ベンチ用 CPU コアを OS / 他プロセスから隔離しているか（systemd slice, cpuset cgroup, taskset）
- [ ] CPU governor を `performance` 固定しているか（省電力モードで周波数が揺れると計測ノイズ）
- [ ] NIC IRQ をベンチコアから追い出しているか
- [ ] SMT の物理コア / 論理コアの対応を把握しているか（§1.6: pin 7,15 は 1 物理コアの 2 スレッド）

### 4.2 ネットワークエミュレーション

- [ ] netem の `limit` を十分大きく設定しているか（既定 1000 packets は高 conns で BDP 超過 → サイレントドロップ。§1.6: `limit=100000`）
- [ ] loopback netem は send/recv 両方を通ることを把握しているか（片道 25ms 指定 → RTT ~50ms、loss も `1-(1-loss)²`）
- [ ] ベンチ後に netem を確実にクリアしているか（残留すると後続テストがサイレントに壊れる。§1.9: `test_no_netem_on_lo`）

### 4.3 プロセス実行環境

- [ ] `systemd-run` 経由実行時の fd 上限（`LimitNOFILE`）が十分か（§1.7: 既定 1024 vs 対話 shell 524288）
- [ ] `systemd-run` の CWD が正しいか（既定 `/` → 相対パスのファイル出力がサイレントに失敗。§2: `--working-directory=$PWD`）
- [ ] root 実行で SIGABRT する lib がないか（§2: msquic は `-p User=$USER` 必須）
- [ ] cgroup の AllowedCPUs と taskset が衝突しないか（§2: systemd-run 経由必須）

### 4.4 共有リソースの認識

- [ ] CPU 隔離していても**メモリ帯域・L3 キャッシュは物理共有**であることを認識しているか（§2: AMD CAT 非対応環境）
- [ ] 「絶対値の信頼度は中、相対順位の指標として使う」運用にしているか

---

## 5. 負荷生成（Client Farm）

- [ ] client の `attempted_ratio` が 1.0 か確認しているか。1.0 未満なら**負荷生成側が律速**（§1.6, §1.9）
- [ ] `client_tick_ok` が valid 判定に組み込まれているか（pacing budget を守れなかった run は invalid）
- [ ] multi-proc client 数が適切か（少なすぎ → client 律速、多すぎ → scheduler thrashing で valid=0。§2: 論理コア数に対し N=2〜4）
- [ ] broadcast と echo で client proc 数を分けているか（§1.9: echo=8 proc, broadcast=4 proc。broadcast を 8 にすると thread の多い lib の受信 delivery が落ちる）
- [ ] pacing budget の閾値が rate-proportional + 絶対値の床になっているか（§1.4: `max(100, interval/10)`）

---

## 6. 診断と原因切り分け

### 6.1 ブレイクポイント診断

- [ ] delivery < 0.95 のとき、まず以下を切り分けているか：
  - client 律速（`attempted_ratio < 1.0`, `client_tick_ok = 0`）
  - server CPU 飽和（`server_cpu_pct` ≈ コア上限）
  - ネットワーク律速（netem loss / limit 超過）
  - ライブラリ固有の限界（輻輳制御、retx queue）
  - **harness バグ**（窓ズレ、fd 上限、ramp 不整合）

### 6.2 「c1 から壊れている」パターン

- [ ] c1（最小接続数）で delivery が低い場合、**ライブラリ起因として不自然**なので harness / adapter バグを疑っているか（§1.9: yojimbo, udt4, msquic すべて harness 側の問題だった）

### 6.3 duration 依存パターン

- [ ] 「delivery が duration 非依存の一定割合で欠ける」を見たら、**両端のライフサイクル（窓ズレ）を疑う**（§1.7: `delivery ≈ (duration − ramp + 2s) / duration` で理論値と完全一致）
- [ ] ライブラリのチューニングに走る前に、client/server の active window が重なっているか `connect_ms` と突き合わせる

### 6.4 診断シグナルの活用

- [ ] `server_recv_drained_p99` が大きい行はサーバー drain が ranking に影響する可能性を疑う
- [ ] `conn_disc_transport` / `conn_disc_peer` でベンチ中の unexpected disconnect を検出しているか
- [ ] `close_ms` で teardown の健全性を確認しているか
- [ ] 「手動再現では通るが script 経由で落ちる」ときは **ulimit / cgroup の unit 既定値差**を疑う（§1.7）

---

## 7. 結果の報告と解釈

- [ ] Capacity の定義を明記しているか（例: 「N=3 で valid ≥ 2/3 かつ delivery ≥ 0.95 を満たす最大 conns」）
- [ ] break reason を記録しているか（delivery 不足 / client_crash / server_crash / client_tick invalid / unsupported）
- [ ] 暗号有無・スレッドモデル・conn あたりの socket 数など、**公平比較を妨げる差異を注記**しているか
- [ ] 「combined p99 が上がった」を HoL の根拠にしていないか（§1.1）
- [ ] 新メトリクス追加時に「mixed で値が変わるか？ その理由は実体か計測アーティファクトか？」を先に検討しているか（§1.1）
- [ ] 結果の再現に必要な情報（プロファイル定義、netem パラメータ、CPU ピン配置、N、duration）が全て記録されているか

---

## 8. CI / 自動化

- [ ] build + smoke test を CI で回しているか（canonical ベンチは重すぎるので CI 外）
- [ ] netem 残留チェック（`test_no_netem_on_lo`）を test 先頭に入れているか（§1.9）
- [ ] CSV 列順変更時に下流スクリプト（plot, 集約, report）を同時に直しているか（§3）
- [ ] resume 機能で途中失敗からの再開ができるか
- [ ] 前回結果（`capacity.csv`）を参照した adaptive N で計測時間を短縮しているか
