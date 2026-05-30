# rudp-bench 計測妥当性レビュー — 課題洗い出し

**作成日:** 2026-05-30
**目的:** 測定レポート4本（`docs/measurements/`）とコードを突き合わせ、「正しく測れていない疑惑のあるライブラリ」「同条件で性能向上の余地がある実装」を全件洗い出したもの。
**評価軸（重要）:** *マルチスレッドで複数コアを使う・暗号を切れる等はライブラリの特性であり、同一の物理ハードウェア上でそれを活かして速いなら優秀とみなす。* したがって「ハード不公平」系は課題から除外し、§F に「確認して問題なし＝実力」として分離した。

凡例: 確信度 = レポート/コードから見た確からしさ（高/中/低）。根拠は現コードの `file:line`。
**注意:** file:line はこのレビュー時点。着手前に現コードで再確認すること。

---

## A. ライブラリ別：正しく測れていない疑惑 + 実装改善余地

### msquic
- [ ] **L1** ［疑惑/改善・確信高］datagram 送信失敗を握り潰している。`DATAGRAM_SEND_STATE_CHANGED` で `LOST_DISCARDED`/`CANCELED` が来ても状態を見ずに ctx を delete するだけ（`adapters/msquic/msquic_adapter.cc:288-297`）。drop がカウントされず harness には「送ったが届かない」として現れる。
- [ ] **L2** ［疑惑/改善・確信中〜高］`QUIC_SETTINGS` で datagram 送信側の制御を何も設定していない（`SendBufferingEnabled`/`MaxBytesPerKey`/`CongestionControlAlgorithm`/`PacingEnabled` 全てデフォルト、`adapters/msquic/msquic_adapter.cc:96-97,138-139`）。flat 0.58 ≒ reliable stream≈1.0 + datagram≈0.16 の合算で、cwnd/pacing 既定が datagram を一定割合捨てている疑い。設定を詰めれば同一ハードで上がりうる。
- [ ] **L3** ［改善・確信中］1000conn で client_crash。全 conn を main thread が直列 `ConnectionStart`（`adapters/msquic/msquic_adapter.cc:169`）＋ `mtx_` 単一ロックで全 worker callback と main を直列化（`:482`）。ramp 化（`harness/scenario.h` の `ramp_up_ms` 既存）とロック粒度分割の余地。
- [ ] **L4** ［改善・確信中］datagram drop の可視化未実装。`ACKNOWLEDGED`/`SENT`/`LOST`/`CANCELED` を分類カウントすれば「届かない」か「送れていない」かを切り分けられる（L1 の対処と一体）。
- [ ] **L5** ［ドキュメント乖離・確信高］`close()` は純 no-op で `_Exit(0)` は現コードに無い（`adapters/msquic/msquic_adapter.cc:208-214`）が、レポート 2026-05-28 は「msquic ラン時のみ `_Exit(0)`」と記述。コードとドキュメントが乖離（DOC2/S4 と関連）。
- [ ] **L6** ［計測の非対称・確信中］`close()` no-op で teardown 時間を測っていない（harness は disconnect を計測しない、`harness/runner.cc:238`）。lifecycle 健全性という定性軸が欠落（数値 RSS/CPU は close 前確定なので不変）。

### kcp
- [ ] **L7** ［疑惑/改善・確信中〜高］CPU 88%（非飽和）なのに 0.71@1000。`ikcp_wndsize(128,128)`（`adapters/kcp/kcp_adapter.cc:176,280`）が受信窓頭打ちで相手の rmt_wnd を絞り送信窓を縮める疑い。窓拡大（256〜1024）で同一ハードでも上がる公算。
- [ ] **L8** ［改善・確信中］poll が毎回全 conn 走査で `ikcp_update`/`ikcp_check` を呼ぶ O(conns) 構造（`adapters/kcp/kcp_adapter.cc:233-242`）。タイムホイール/ヒープ化で O(due) にすれば 10ms 周期追従が改善。
- [ ] **L9** ［改善・確信中］`interval=10ms`（`ikcp_nodelay(1,10,2,1)`, `adapters/kcp/kcp_adapter.cc:174,278`）で flush/再送が最大 10ms 粒度。5ms に下げると ACK 応答が速くなる（CPU 余裕の範囲で）。

### mini_rudp
- [ ] **L10** ［疑惑・確信高］全 cond で valid=0 だが、これは「server が遅い」証明ではなく client adapter が負荷を出し切れていないだけ。mini_rudp の server delivery 能力は一度も測れていない（レポートの「破綻」は client 側の話）。
- [ ] **L11** ［改善・確信高］client が conn 毎に別 UDP ソケットを作る（`adapters/mini_rudp/mini_rudp_adapter.cc:74,81`）。200conn で poll/recv の syscall 数が膨大。単一 fd 多重化が最大の効き。これで valid 化すれば初めて mini_rudp transport を測定可能。
- [ ] **L12** ［改善・確信高］reliable 送信が毎回ヒープ alloc ＋ pending を `unordered_map` 蓄積（`adapters/mini_rudp/mini_rudp_adapter.cc:101-116`）、`poll()` が全 conn × 全 pending を線形走査して retx 判定（`:159-168`）。プール化＋ retx O(due) 化の余地。unreliable 側は `send_scratch_` 再利用で対処済みだが reliable は未対応。
- [ ] **L13** ［改善・確信中］server の `recv` が 1 tick 1 datagram（while ループ無し、`adapters/mini_rudp/mini_rudp_adapter.cc:120-153`）。reliable 1 通ごとに ACK sendto が必ず 1 回追加されパケット倍化。

### enet
- [ ] **L14** ［改善・未解決・確信中］`inbox_`（`ReusableInboundQueue`）が上限なし（`adapters/enet/enet_adapter.cc:123`、`harness/inbound_queue.h`）。dev-notes §4-5 の「上限付きリングバッファ化」は未実装。harness が drain する限り実害は出にくいが保証なし。
- [ ] **L15** ［改善・確信中］client/server とも `SO_RCVBUF` 未設定（既定のまま）。高 conns の delivery 崩壊点に socket buffer 取りこぼしが混入する可能性。raw_udp のみ 256KB なので全 adapter で揃っていない（L17 と一体で要統一）。

### raw_udp
- [ ] **L16** ［改善・軽微・確信中］client が conn 毎に別 fd（`adapters/raw_udp/raw_udp_adapter.cc:115`）。unreliable のみなので軽いが mini_rudp と同設計。基準線としては妥当。
- [ ] **L17** ［定義不一致・確信中］client が 256KB recvbuf（`adapters/raw_udp/raw_udp_adapter.cc:50-54`）を持つのに enet/mini_rudp client は既定。基準線比較で socket buffer の土俵が揃っていない（L15 と一体）。

### gns
- [ ] **L18** ［改善余地は限定的・確信高］暗号化は API で切れない（`encryption_on()=true`, `adapters/gns/gns_adapter.cc:179`）＝仕様。adapter 側で軽くする手段は無い（NoNagle は既に指定済み `:117-119`）。※ユーザー軸では「暗号は lib 特性」なので問題ではなく、改善余地が乏しいという事実のみ記録。

---

## B. 計測指標の定義不一致（lib 横断比較を歪める）

- [ ] **D1** ［確信高］litenetlib の accepted 計上が他 lib と非対称。litenetlib は `peers[i].Send()`(void) の後に無条件 `MarkAccepted`（`adapters/litenetlib/Program.cs:368-372`）。C++ は `a.send()==0` 成功時のみ（`harness/runner.cc:291-296`）。※実際には enet 等も send は通常成功し accepted≈attempted なので 0.994 フラットの主因ではない。厳密化のため定義を揃える価値（軽微）。
- [ ] **D2** ［確信高］LiteNetLib `DeliveryMethod.Unreliable`（`adapters/litenetlib/Program.cs:367`）は enet `ENET_PACKET_FLAG_UNSEQUENCED`（`adapters/enet/enet_adapter.cc:79`）と非等価。前者は順序保証なしだが per-channel sequence 管理が走る。mixed の HoL 挙動が厳密同一でない。レポートに注記すべき。
- [ ] **D3** ［確信中］delivery_ratio の accepted が「send 受理」ベースで楽観的（`harness/runner.cc:291-296`）。loss シナリオでは往復成功率として意図通りだが、片道配信率とは別物。アダプタに送出パケット数を返す IF を足せば分離可能。
- [ ] **D4** ［確信中］broadcast モードで accepted を `expected_per_send=conns` 倍に水増し（`harness/runner.cc:294-296`）。受信側は per-(cid,seq) で 1 回 dedup。同一 cid に複数 echo が同一 seq で来ると重複扱いになりうる。echo モードでは無害、broadcast の解釈は要注意（テストで固定すべき）。
- [ ] **D5** ［確信中］server CPU% を全 lib 横並びで並べているが、シングルスレッド lib（enet/kcp 最大 ~100%）とマルチスレッド lib（gns/litenetlib ~200%）で意味が違う。※ユーザー軸では正当な lib 特性。ただし `summary.csv` は区別せず数値だけ並べるので読み手が誤読しやすい（DOC3 と関連）。
- [ ] **D6** ［確信高］server 側の CPU/RSS 計測 API が lib で異なる。C++ は `getrusage(RUSAGE_SELF)`（`harness/proc_sampler.cc:10-16`）、litenetlib は .NET `Process.TotalProcessorTime`（`adapters/litenetlib/Program.cs:955-959`）。どちらも全スレッド合算だが系統誤差が乗りうる。統一サンプラ（親が `/proc/<pid>/task/*` を周期 poll）で吸収可能。

---

## C. 計測器（harness / metrics / proc_sampler）の歪み・限界

- [ ] **M1** ［確信中］CPU% が begin/end の 2 点差分のみで periodic 化されていない（`harness/proc_sampler.cc:53-57`）。RSS は 100ms 周期化済みだが CPU は未対応。スパイクが平均に薄まる。
- [ ] **M2** ［確信中］CPU 計測区間が warmup を除外していない（`ps.begin()` が warmup 前、`harness/runner.cc:241-242`）。20s run で warmup 2s（litenetlib は 5s）が平均に混入。
- [ ] **M3** ［確信中］client 側 `rss_max_mb` が lib 固有メモリと harness オーバーヘッド（per-conn 65536 dedup + LatencyHist）を分離できていない。lib のメモリ効率を語るなら server 側 RSS を使うべき（注記 or 引き算可能化）。
- [ ] **M4** ［確信中］attempted と accepted の warmup/run_end ガード境界が非対称（attempted は `now>=warmup_end && now<run_end`＝`harness/runner.cc:282`、accepted は `in_measure(now)` で上限 run_end なし＝`:292`）。tick 跨ぎで `attempted_ratio` が 1.0 から微妙に外れうる。両者を統一すべき。
- [ ] **M5** ［確信低〜中］RTT が「per-tick recv ドレイン時刻」基準（`harness/runner.cc:343-346`）。カーネル受信時刻でなく、高 conns で 1 tick に大量ドレインすると後方メッセージの RTT が水増し。`client_recv_drained_p99` が大きい行に harness 由来遅延が混入。`SO_TIMESTAMPNS` 採用 or 該当行のフィルタで対処。
- [ ] **M6** ［確信低・無害］per-conn seq の 48bit マスク（`harness/metrics.h:72`, `harness/runner.cc:284-285`）。現状は map キーが `conn_id` なので衝突しないが、送信側 `i` と受信側 `cid` の対応はアダプタ依存。将来の罠としてのみ記録。

---

## D. スクリプト / 集約 / 集計パイプライン

- [ ] **S1** ［確信高］N=3 中央値と valid run 選別がコード化されていない。`reduce_result.py` は 1 run を正規化するだけで、median/valid 抽出ロジックは存在せず。`summary.csv` はレポート自身が「手で焼き込み」と明記（`docs/measurements/2026-05-30-scale-sweep-v2/report.md:80`）。どの run を valid 採用し中央値に含めたかが再現・検証不能。集計スクリプト化が必要。
- [ ] **S2** ［確信高］valid 判定が litenetlib では .NET 側の自己申告（`client_tick_ok`）に依存。`scripts/reduce_result.py:265-266` は raw CSV の `client_tick_ok` をそのまま信じる。他 lib は C++ harness が立てるが litenetlib は `adapters/litenetlib/Program.cs:450-496` が自分で立てる。
- [ ] **S3** ［確信中］`combine_clients.py` の rtt_* 二重処理。MAX_COLS で max を取った後 histogram 再計算で上書き（`scripts/combine_clients.py:234`→`:237-242`）。結果は正しいが、bin 欠落時に誤った max 値が残るフォールバック経路。
- [ ] **S4** ［確信中］msquic は `_Exit` 系の終了で、終了コードベースの crash 判定（`scripts/reduce_result.py:166-176`）が効かない可能性。L5/DOC2 で `_Exit` の所在がコメントと乖離している件と関連、要コード再確認。

---

## E. ドキュメント / レポートとコードの乖離

- [ ] **DOC1** ［確信高］dev-notes §4 の技術的負債 3 点が全て解消済み（陳腐化）。dev-notes を現コードに合わせて更新する。
  - (a) `DeliveryTracker::pack` の conn_id 16bit 切り詰め → 現コードに該当せず。`conn_id` を map キーにした per-conn 管理で衝突しない（`harness/metrics.cc:74-86`, `harness/metrics.h:76`）。
  - (b) `LatencyHist::samples_`/`received_keys_` 無制限成長 → 固定サイズ配列（`harness/metrics.h:39`）＋ per-conn 65536 リング（`harness/sliding_dedup_window.h`）で上限あり。
  - (c) ProcSampler RSS 2 点のみ → 100ms periodic 化済み（`harness/runner.cc:105,320`, `harness/proc_sampler.cc:41-45`）。ただし CPU は今も 2 点（M1 と整合）。
- [ ] **DOC2** ［確信高］L5 と同じ：2026-05-28 report の msquic `_Exit(0)` 記述がコードと乖離。レポート修正。
- [ ] **DOC3** ［確信中］v2 report は「同じ 1 物理コア server で litenetlib が最良のスケーラ」と書くが、litenetlib/gns がマルチスレッドで SMT 2 レーンを使い、enet/kcp はシングルスレッドで 1 レーンというスレッド並列度の差を本文注記でしか吸収していない。※ユーザー軸では正当な実力差なので「誤り」ではないが、CSV 単独で見ると誤読しやすい。`summary.csv` に thread モデル列を足す等。
- [ ] **DOC4** ［確信中］v2 report 自身が litenetlib 0.994 を「意外、裏取り価値あり」と保留。コード精査の結論は「正当な実力の可能性が高い」（received/accepted は server drop を必ず捕捉するため、0.994 維持は echo を捌けている証拠）。レポートの保留を解消・更新する価値あり（裏取りとして `StartInManualMode()` でシングルスレッド化再測すれば実力か資源かを切り分けられる）。

---

## F. 確認して「問題なし＝ライブラリの実力」と判明した項目（参考・対処不要）

- **OK1** litenetlib のマルチスレッド（ReceiveThread/LogicThread）は同一 1 物理コア cpuset 内で動作（`scripts/run_phase1_quick.sh:178` が他 lib と同じ `run_timeout`/`$SERVER_CPU`、CPU 191% < 200% が 1 物理コア内の証拠）。同じハードで SMT 2 レーンを使い切るのは設計上の強み。割り引かない。
- **OK2** gns/msquic の暗号化必須はライブラリ特性。暗号をやりながら高 delivery を出すのはむしろ評価を上げる材料。
- **OK3** litenetlib の 0.994 フラットは received/accepted が server drop を捕捉する以上、アーティファクトでなく実力の公算（DOC4）。当初の「アーティファクト」断定は撤回。
- **OK4** netem の queue limit=100000 化（`scripts/netem.sh`）でキュー溢れアーティファクト除去済み。loss 経路は全 lib 共通で公平。
- **OK5** per-channel RTT histogram（`harness/runner.cc:207-208,342,348`）、tick_gap ゲート除去（`harness/runner.cc:430-442`）、combined histogram の罠回避は実装で確認済み。

---

## メモ

- 全 34 項目（L1-L18, D1-D6, M1-M6, S1-S4, DOC1-DOC4, OK1-OK5）。
- 最も「正しく測れていない」色が濃いのは L1/L2（msquic datagram）と L10/L11（mini_rudp client が測定の土俵に乗れていない）。
- 計測器コア（harness/metrics/proc_sampler）は概ね健全。残る計測器課題は CPU の 2 点・warmup 込み（M1/M2）と集計の手作業（S1）。
