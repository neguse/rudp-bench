# 開発・計測ノート

旧アシスタントメモリから救出した開発知見を1本に集約したもの。
各セクションは記載の日付時点の観察であり、コード参照(`file:line`)は現状と異なる場合がある。
**断言する前に必ず現コードで確認すること。**

目次:
1. [計測の落とし穴](#1-計測の落とし穴)
2. [再現性ベンチ環境・手順(ARK/Minecraft 同居機)](#2-再現性ベンチ環境手順arkminecraft-同居機)
3. [コードベース規約](#3-コードベース規約)
4. [Phase 2 着手前の技術的負債](#4-phase-2-着手前の技術的負債2026-05-30-計測妥当性レビューで照合済み)
5. [計測指標の定義と既知の系統差](#5-計測指標の定義と既知の系統差2026-05-30-計測妥当性レビュー反映)

関連: 測定レポートは [`docs/measurements/`](measurements/) 配下。

---

## 1. 計測の落とし穴

*(2026-05-27 / 一部 2026-05-30 追記。mixed-traffic HoL 検証中などに実際に踏んだ罠)*

### 1.1 combined RTT histogram は混合シナリオで結論を反転させる

**事例(2026-05-27):** rate_r=50 + rate_u=50、netem 5% loss で「ENet が +300% HoL、全 lib HoL あり」と結論しかけたが、これは **combined RTT histogram の汚染**だった:
- mixed の RTT を reliable と unreliable で同じヒストグラムに記録していた
- p99 = 上位 1% の遅い tail。mixed では reliable が全体の 50%、そのうち 5% loss → 全体の 2.5% が retx tail で 200ms+
- p99(上位 1%)はその retx tail に落ちる → unreliable が HoL に巻き込まれた「ように見える」だけ
- per-channel histogram で測ると **全 lib HoL ±1% に収まる**(mini_rudp ですら HoL なし)

**対策:**
- mixed traffic 計測では**必ず per-channel ヒストグラム**(`rtt_r_p*_us` と `rtt_u_p*_us` を分離して記録)。実装済み(commit `3763972`)。現 CSV ヘッダも per-channel 化されている(`harness/csv_writer.cc` の `write_header`)
- harness の payload tag(byte 16 = reliable flag)を server も client も読み、record_us を振り分ける
- 「combined p99 が上がった」を HoL の根拠にしない。HoL の定義は「pure-u u99 vs mixed u99 の差」一択
- 新メトリクス追加時は「mixed で値が変わるか? その理由は実体か計測アーティファクトか?」を先に問う

### 1.2 「channel 分離設計だから HoL なし」は理論として正しい

per-channel histogram 採用後の実測(2026-05-27、netem 25ms+5ms+5% loss、conns=10、50/50 mix):

| lib | pure-u u99 | mixed u99 | Δ |
|---|---|---|---|
| mini_rudp | 66.1ms | 66.8ms | +0.7ms (+1%) |
| enet | 66.2ms | 66.3ms | +0.1ms (0%) |
| gns | 66.5ms | 66.8ms | +0.3ms (0%) |
| litenetlib | 88.7ms | 88.4ms | -0.3ms (0%) |
| msquic | 66.7ms | 66.5ms | -0.2ms (0%) |

低 conns(=10)では `mini_rudp ですら HoL なし`。理屈は「reliable retx が別タイマで動いて send queue を専有しないなら、channel 概念なしでも unreliable は阻害されない」。
ただし**高負荷(200conn〜)では mini_rudp が massive HoL leakage を起こす**(2026-05-28 report 参照、13–19倍)。低 conns で出ないのは負荷不足。HoL 軸スイープは per-channel u99 で比較すること。

### 1.3 ENet は channel 0(reliable)+ channel 1(unreliable + UNSEQUENCED)で送る

**事例:** ENet adapter が reliable/unreliable とも channel 0 を使い、unreliable に flags=0(=ordered)を渡していた。channel 内 sequence 維持のため reliable 未到着時に後続 unreliable が待たされていた(commit `085cefc` で修正)。

**対策:** ENet で reliable + unreliable を同一 peer に送るときは:
- 違う channel ID を使う(channel 0 = reliable、channel 1 = unreliable。`host_create` の channelCount で 2 以上を確保)
- unreliable には `ENET_PACKET_FLAG_UNSEQUENCED` を立てる(ordering 解除、fire-and-forget)

(この修正は HoL 結果には影響しなかった — それも combined histogram の罠だった — が ENet の正しい使い方として残す)

### 1.4 pacing budget は rate-proportional + 床

**事例:** 古い式 `max(20, min(100, interval/10))` は低レートで 100us cap が支配的、50Hz(interval 20ms)で 100us = 0.5% 精度を要求していた。yojimbo は per-Send アロケータコストで 103us 出る → tick_ok=0 → delivery_ratio=1.0 なのに valid=false(commit `c0853eb` で `max(100, interval/10)` に修正)。

**対策:** pacing/精度系の閾値は「絶対値」でなく「interval の何 %」で書く。floor は別途置く。

### 1.5 single run の結果に意味はない

**事例:** isolation 効果検証で 1 lib × 1 条件 × N=1 で「mini_rudp -19%、enet -22%」と出したが、同じ run を 10 回回せば run-to-run で ±10〜20% 振れる範囲。signal と noise が分離できていない。

**対策:** 結論用の数字は N≥3、できれば N=10。公開数字は最低 N=3 取って中央値+IQR。N=1 で言えるのは shape だけで、「lib X > lib Y」と言えるのは差が IQR を超えたときのみ。

### 1.6 idle=spin の cpu 100% を飽和と誤読しない / SMT 誤読(2026-05-30 追記)

- `--idle=spin` は無負荷でも常時ビジースピンするので `cpu_pct=100%` は飽和の証拠にならない。**CPU を律速根拠にするなら `--idle=adaptive` 必須**
- pin `7,15` / `6,14` は 5750GE では**各々 1物理コアの SMT 2スレッド**(2物理コアではない)。server=1物理コアの最大 CPU は ~200%
- **netem は `limit` 未指定だと既定 1000 packets** が効き、高 conns で BDP がこれを超えると問答無用で drop(計測アーティファクト)。`scripts/netem.sh` / `set_loss.sh` は既定 `limit=100000` に修正済み。詳細は [`docs/measurements/2026-05-30-netem-limit-artifact/report.md`](measurements/2026-05-30-netem-limit-artifact/report.md)
- relief test(片側資源を増やして指標が動くか)は **`attempted_ratio=1.0`(client が負荷を出し切っている)を前提条件として確認しないと誤読する**。過少資源の client は生成律速になりうる
- loss 条件で reliable retransmit を見る場合、active window 後の既定 tail 500ms だけでは切り捨てが起きうる。`--tail-ms` を伸ばし、`scenarios.csv` の `tail_ms` と一緒に結果を読む。
- server の per-loop recv drain に人工上限があると、1000conn mixed のような burst 条件で forward delivery を過小評価しうる。現 runner は既定で上限なし、切り分け時だけ `RUDP_SERVER_RECV_DRAIN_LIMIT=<n>` を使う。`server_recv_drained_p99` / `server_recv_drained_max` が大きい行は server drain が ranking に影響している可能性を疑う。

---

## 2. 再現性ベンチ環境・手順(ARK/Minecraft 同居機)

*(2026-05-27)*

開発機(Ryzen 7 PRO 5750GE、8c/16t)は ARK サーバと Minecraft サーバ(Java)を常時稼働させたまま rudp-bench を走らせる必要がある。専用機/クラウドへは移行しておらず、ソフト隔離で「ゲーム動かしたまま再現性ベンチ」を成立させている(ゲームを落とすと他人のセッションが切れるため)。

**手順:**
1. `scripts/bench_isolate.sh setup` — systemd の system/user/init slice を一部コアに閉じ込め、bench 用に物理コアを空ける。governor を performance 固定、NIC IRQ も追い出す
   - ※ 隔離レイアウトは更新されている。最新の client/server コア割当は `bench_isolate.sh` と各 report のセットアップ節を参照(2026-05-30 時点: client=2物理コア, server=1物理コア)
2. ベンチ実行は `--isolate=systemd --server-cpu=... --client-cpu=...` 付き。`systemd-run --slice=bench-server.slice --working-directory=$PWD -p User=$USER -p AllowedCPUs=... -p RuntimeMaxSec=...s --quiet --wait --pipe --collect` で起動
3. ネット条件は `sudo scripts/netem.sh apply <delay_ms> <jitter_ms> <loss_pct>` を loopback に。**loopback は send/recv 両方で netem を通る**ので片道 25ms 指定で RTT ~50ms、loss も実効 `1-(1-loss)²`
4. 終わったら `scripts/netem.sh clear`、`scripts/bench_isolate.sh teardown`

**落とし穴(踏んで覚えた):**
- AllowedCPUs 縛り中に普通の `taskset -c 7,15 <bin>` で起動するとシェルの cgroup と衝突して即死。必ず `systemd-run --slice=...` 経由(`run_phase1_quick.sh --isolate=systemd` がそれ)
- `sudo systemd-run` は既定で unit を root 実行。msquic は root だと SIGABRT(QUIC datapath 初期化中)。**`-p User=$USER` 必須**
- `systemd-run` の unit CWD は既定 `/`。相対パスの `--out=results/...` が `/results/...` 解釈で fopen 失敗 → サイレントクラッシュ。**`--working-directory=$PWD` 必須**
- 同居で隔離できるのは CPU だけ。**メモリ帯域・L3 cache は物理共有**(5750GE は AMD CAT 非対応)。ARK が cache を食う瞬間に RTT p99 が跳ねる → 「絶対値の信頼度は中、相対順位の指標として使う」運用

**Multi-process client farm(commit `a0f4156` 以降):**
- `--client-procs=N` で client を N プロセス並列起動。各 client が CONNS/N 担当、独立 bin file 書く → `scripts/combine_clients.py` が count を sum、bin を merge してから percentile を recompute
- 1 process だと CONNS 大で client tick が pacing を落とす(N=1 で 100conn = u99 1017us、N=4 で 414us)。過分割も逆効果(N=10 で 100conn = scheduler thrashing で valid=0)
- 配置目安: client の論理コア数に対し N=2〜4 が sweet spot。重い lib(gns/litenetlib)は高 conns で client 3物理コア必要(load generator 過剰供給の原則)
- litenetlib は当初 bin 出力未対応で N>1 自動スキップだったが、**2026-05-30 に multi-proc 対応済み**(`combine_clients.py` の tick_ok は per-proc AND ではなく集約比率ベースに変更、commit `523b38a`)
- 検証は `tests/test_combine_clients.py`(N=1 round-trip で harness の percentile と完全一致を確認)

---

## 3. コードベース規約

*(2026-04-28 確立。Plan 1 で固定。CSV 列順の記述のみ 2026-05-30 に現コードへ更新)*

**Adapter 抽象 IF(`harness/adapter.h`):**
- `Adapter` は pure virtual 構造体(server_listen / client_connect / is_connected / send / recv / poll / close / name / supports / encryption_on)
- `send` 戻り値: 0=ok, -1=err。`recv` 戻り値: 1=msg, 0=none, -1=err(conn_id は output param)
- `client_connect` は async 対応: handle 即返却 + `is_connected(handle)` で readiness 確認

**Adapter 登録パターン:**
- 各 adapter cc 末尾に `namespace rudp_bench { void register_X_adapter() { register_adapter("X", []{...}); } }`
- `harness/main.cc` 冒頭で全 register 関数を**明示呼び出し**(macro auto-registration ではなく明示展開)
- **Why:** Macro 自動登録(`REGISTER_ADAPTER`)は CMake STATIC ライブラリ越しで linker が dead-strip する。明示パターンは main.cc 1行編集が増えるが確実(`--whole-archive` 等の特殊設定が不要)
- smoke test では `static XRegistrar registrar;` で test 内 static init 経由で同じ関数を呼ぶ

**CSV 列順は固定。** 単一の真実源は `harness/csv_writer.cc` の `write_header`。
- 現在は per-channel histogram 化済み(`rtt_r_p*_us` / `rtt_u_p*_us` を分離。combined ではない — §1.1 参照)、`rate_r`/`rate_u` 分離、`client_attempted_ratio`/`client_accepted_ratio` 等の診断列が追加されている
- **列順を変えるときは** `scripts/` の pandas pivot(plot 系)と各集約スクリプトが依存している点に注意。header 行と読み出し側を必ず同時に直す

**その他:**
- **コメント言語:** 日本語コメント OK(プラン・コード両方)。レビュアーが「英語に揃えるべき」と指摘しても従わなくてよい(意図的選択)
- **ファイル粒度:** 1 adapter = 1 cc(ヘッダ無し、static class を内部 namespace に隠す)。harness の各機能(metrics/csv/scenario/runner/proc_sampler)は h+cc の対。テストは smoke 1本
- **Canonical benchmark test:** 人間向け入口は [`docs/CANONICAL.md`](CANONICAL.md)。`scripts/run_canonical_tests.sh` を標準の再測定入口にする。ここでの test は unit test ではなく、build 後に latest final saturation profiles (`media_relay`, `game_server`, `echo`) を current canonical target set で N=3 全実行する benchmark test。測定後に `scripts/render_canonical_report.py` が `$OUT/report.md` と `$OUT/plots/*.png` を生成し、`scripts/publish_canonical_result.py` が dated measurement directory と `docs/measurements/current.md` を更新する。各 adapter は STATIC lib(`adapter_<name>` 命名)

---

## 4. Phase 2 着手前の技術的負債(2026-05-30 計測妥当性レビューで照合済み)

*(2026-04-28 の Plan 1 最終レビューで識別。2026-05-30 のレビュー(`tasks.md`)で各項目を現コードと照合し、解決状況を確定した。**コード参照は確認時点**)*

1. ~~**size = 65536 が UDP datagram 上限超え**~~ → harness は `min_payload..max_payload` 外を skip row として弾く(`harness/main.cc`)。`mini_rudp` は L11 のヘッダ拡張で max_payload=65497、`raw_udp`=65507。スイープ側で上限超えを投げなければ無害(skip 扱い)。
2. ~~**`LatencyHist`/`DeliveryTracker` 無制限成長**~~ → **解決済み**。固定サイズ bin 配列(`harness/metrics.h`)+ per-conn 65536 リング(`harness/sliding_dedup_window.h`)で上限あり。
3. ~~**`ProcSampler` の RSS が 2点のみ**~~ → **解決済み**。RSS は 100ms periodic(M1 以前から)。**CPU も M1 で periodic 化**し `cpu_pct_peak` 列を追加(`harness/proc_sampler.cc` の `sample_cpu`)。さらに warmup を CPU 窓から除外(M2, `mark_measure_begin`)。
4. **`mini_rudp`/`raw_udp` の CMakeLists 不整合** — トップレベルで `-Wall -Wextra -Wpedantic` がかかるため機能差なし(`CMakeLists.txt:12`)。adapter テンプレートとしてコピー時の紛らわしさのみ。実害なしのため未対応で記録のみ。
5. ~~**ENet `inbox_` 無制限成長**~~ → **解決済み(L14)**。`ReusableInboundQueue` に `set_limit()`(リング)+ `dropped()` を追加。enet は 65536 上限、溢れたら `enet_inbox_dropped: N` を stderr に出す。
6. ~~**`DeliveryTracker::pack` が conn_id 16bit 切り捨て**~~ → **解決済み**。dedup は `conn_id` を map キーにした per-conn 管理で衝突しない(`harness/metrics.cc`)。さらに M6 で seq mask を 48bit→64bit に拡張し将来の別用途でも別名衝突しない。

---

## 5. 計測指標の定義と既知の系統差(2026-05-30 計測妥当性レビュー反映)

*(`tasks.md` の D/M/L 系項目の結論。lib 横断比較で誤読しないための定義集)*

### 5.1 delivery_ratio は「往復(echo)成功率」であって片道配信率ではない(D3)

`delivery_ratio = received / accepted`。`accepted` は **client が adapter に送出受理された数**(`a.send()==0`)、`received` は **echo が client に戻って per-(conn,seq) で初観測された数**。したがって loss シナリオでは「往復で 1 回でも落ちれば未達」を測っている。片道配信率(server がいくつ受け取ったか)とは別物。S5 以降は `forward_delivery_ratio` / `server_echo_accept_ratio` / `return_delivery_ratio` と channel 別 `_r` / `_u` で片道を分離して読む。

### 5.2 server CPU% は thread モデルを併記して読む(D5 / DOC3)

`cpu_pct` は全スレッド合算(`getrusage(RUSAGE_SELF)` / .NET `TotalProcessorTime`)。1物理コア(SMT 2スレッド)上限は ~200%。**単一スレッド lib(enet/kcp)の上限 ~100% と、マルチスレッド lib(gns/litenetlib、msquic は内部 worker)の ~200% は意味が違う**。ユーザー軸では「同一ハードで SMT 2レーンを使い切るのは実力」なので割り引かないが、`summary.csv` の CPU 値を単独で見ると誤読しやすい。**v2 summary.csv に `thread_model` 列を追加**(single/multi/internal_worker)。M1 で `cpu_pct_peak`(瞬間ピーク)も併記され、平均に薄まったスパイクが見える。

### 5.3 CPU/RSS 計測 API の系統差(D6)

C++ は `getrusage(RUSAGE_SELF)`、litenetlib は .NET `Process.TotalProcessorTime`。**両者とも全スレッド合算の whole-process CPU を測っており測定対象は同一**。サンプリング形状も揃えた(2点平均 + M1 periodic peak + M2 warmup 除外)。系統誤差をさらに潰すなら親プロセスが `/proc/<pid>/stat` を周期 poll する統一サンプラに寄せる余地があるが、現状の差は小さいと判断し未導入。

### 5.4 client RSS は lib メモリ効率の指標にしない(M3)

client 側 `rss_max_mb` には harness オーバーヘッド(per-conn 65536 dedup リング + LatencyHist 固定 bin)が混じる。**lib のメモリ効率を語るなら server 側 RSS を使う**。client RSS は load generator の規模指標として読む。

### 5.5 高 conns の RTT には harness 由来の遅延が混じりうる(M5)

RTT は「per-tick recv ドレイン時刻」基準(カーネル受信時刻ではない)。高 conns で 1 tick に大量ドレインすると後方メッセージの RTT が水増しされる。**`client_recv_drained_p99` が大きい行は RTT 絶対値を割り引く**(該当行フィルタで対処)。厳密化には `SO_TIMESTAMPNS` 採用が必要だが全 adapter 改修コストに見合わないため診断列での運用とする。

### 5.6 LiteNetLib Unreliable ≠ enet UNSEQUENCED(D2)

`DeliveryMethod.Unreliable`(litenetlib)と `ENET_PACKET_FLAG_UNSEQUENCED`(enet)は厳密には非等価。両者とも順序保証なしの fire-and-forget だが内部の sequence 取り扱いが異なりうるため、mixed の HoL 挙動が完全同一とは限らない。HoL 比較は per-channel u99 の pure-u vs mixed 差で見る(§1.1-1.2)前提なら実害は小さいが、絶対比較時は注記する。

### 5.7 gns/msquic の暗号は API で切れない(L18 / OK2)

`gns` は `encryption_on()=true` 固定(`adapters/gns/gns_adapter.cc`、NoNagle は指定済み)、`msquic` も QUIC で暗号必須。adapter 側で軽くする手段はない=仕様。ユーザー軸では「暗号しながら高 delivery は評価を上げる材料」なので問題ではない。改善余地が乏しいという事実のみ記録。

### 5.8 mini_rudp は L11 で初めて測定の土俵に乗った(L10)

旧 report の mini_rudp「全 cond valid=0/client_tick」は **server delivery の限界ではなく client adapter の負荷生成律速**(conn 毎に別 socket → syscall 過多)。L11 の単一 fd 多重化で `attempted_ratio=1.0 / tick_ok=1` になり、mini_rudp transport の delivery が初めて測定可能になった(200conn ローカルで dr~0.90 を観測)。

### 5.9 新しい診断シグナル一覧(2026-05-30 追加)

| シグナル | 出所 | 意味 |
|---|---|---|
| `cpu_pct_peak` / `server_cpu_pct_peak` (CSV列) | M1 | 100ms 区間 CPU% の最大。raw/diagnostics と canonical server 側に出し、平均に薄まるスパイクを捕捉 |
| `close_ms` (CSV列) | L6 | teardown(close())所要 ms。msquic は no-op で ~0(=同期 teardown 不可の lifecycle シグナル)、litenetlib は数百 ms の graceful stop |
| `client_tick_ok_check` (diagnostics列) | S2 | reducer が比率から独立再計算した valid 判定。自己申告 `client_tick_ok` とのドリフト検出用 |
| `msquic_datagram: offered/submit_failed/acked/lost/canceled` (stderr) | L1/L4 | QUIC datagram 送信状態の分類。「送ったが lost」と「そもそも submit 失敗」を切り分け |
| `enet_inbox_dropped: N` (stderr) | L14 | 上限付き inbox が溢れて落とした受信メッセージ数 |
| `forward_delivery_ratio` / `server_echo_accept_ratio` / `return_delivery_ratio` | S5 | round-trip delivery を client→server、server echo submit、server→client に分解する。各 ratio は `_r` / `_u` の channel 別値も出す。payload byte16 は bit0=reliable、bit1=measurement-window |
| `conn_peak` / `conn_disc_transport` / `conn_disc_peer` | S6 | adapter が観測した接続ピークと bench 中 disconnect 数。multi-process client は proc 別 raw CSV の値を合算して diagnostics に出す |
| `server_recv_drained_p99` / `server_recv_drained_max` | S7 | server loop 1 tick で `recv()` した message 数。既定は上限なしで、`RUDP_SERVER_RECV_DRAIN_LIMIT=<n>` 指定時の上限影響を検出する |

### 5.10 measurement bit で warmup 境界の delivery>1.0 を除外(S5)

旧 runner は client 受信側を `t_recv >= warmup_end` だけで集計していたため、warmup 中に送った echo が warmup 後に戻ると `received` に入り、`delivery_ratio > 1.0` が起こりえた。S5 で payload byte16 の bit1 を measurement-window とし、client もこの bit が立った echo だけを RTT/throughput/delivery に入れる。これにより、新しい CSV では warmup echo の混入を除外できる。

### 5.11 msquic の close() / _Exit(0) の所在(L5 / DOC2 / S4)

`adapters/msquic/msquic_adapter.cc` の `close()` は **純 no-op**(同期 teardown が worker と競合し deadlock/double-free するため)。`std::_Exit(0)` は **`harness/main.cc` 側**にあり `cfg.library=="msquic"` のときだけ実行(`write_output()` の後)。つまり 2026-05-28 report の「msquic ラン時のみ `_Exit(0)`」は**正しい記述**(所在は adapter ではなく harness)。終了コードベースの crash 判定は、実 run 中の crash は `_Exit(0)` 到達前に非0 signal で死ぬため正しく検出される(S4)。
