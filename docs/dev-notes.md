# 開発・計測ノート

旧アシスタントメモリから救出した開発知見を1本に集約したもの。
各セクションは記載の日付時点の観察であり、コード参照(`file:line`)は現状と異なる場合がある。
**断言する前に必ず現コードで確認すること。**

目次:
1. [計測の落とし穴](#1-計測の落とし穴)
2. [再現性ベンチ環境・手順(ARK/Minecraft 同居機)](#2-再現性ベンチ環境手順arkminecraft-同居機)
3. [コードベース規約](#3-コードベース規約)
4. [Phase 2 着手前の技術的負債](#4-phase-2-着手前の技術的負債)

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
- **ビルド:** C++17 + CMake 3.20+ + GoogleTest via FetchContent。`cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure` で全部回る。各 adapter は STATIC lib(`adapter_<name>` 命名)

---

## 4. Phase 2 着手前の技術的負債

*(2026-04-28 の Plan 1 最終レビューで識別。commit `0bfa21b` で部分対応。**現状の解決状況は未照合 — 着手前に各 file:line を確認すること**。Phase 2 相当の scale 計測は既に進行しているため、一部は未対応のまま走っている可能性がある)*

Phase 1 の粗いスイープでは目立たないが、軸を細かく刻んで長時間ランすると結果を歪める可能性のある項目:

1. **size = 65536 が UDP datagram 上限超え** — UDP 最大ペイロードは 65507B。`mini_rudp` のヘッダ 6B + 65536B は `EMSGSIZE` で `sendto` 失敗、または受信側 2KB バッファで切り詰め。`raw_udp` も送信失敗。
   - 案A: `mini_rudp` にアプリ層フラグメンテーション/再アセンブリ実装(比較フェアだが高コスト)
   - 案B(低コスト): スイープの size を 65536 → 8192 に下げ spec も修正
2. **`LatencyHist::samples_` と `DeliveryTracker::received_keys_` が無制限成長**(`harness/metrics.{h,cc}`) — 高 throughput 長時間ランで harness 自体が数百 MB 食い、`rss_max_mb` が lib でなく harness オーバーヘッドを反映。reservoir sampling か上限付きリングバッファに切替(percentile は近似で可)
3. **`ProcSampler` の RSS は begin/end の2点のみ**(`harness/proc_sampler.cc`) — run 中の peak が間で起きて戻ると取りこぼす。長時間ランなら periodic sampling を追加
4. **`mini_rudp` と `raw_udp` の CMakeLists 不整合** — `raw_udp` には `-Wall -Wextra` があるが `mini_rudp` には無い(トップレベルでカバー済みで機能差はないが、adapter テンプレートとしてコピーされる時に紛らわしい)
5. **ENet adapter の `inbox_` が無制限成長**(`adapters/enet/enet_adapter.cc`、項目2と同類) — harness が `recv` を引かないと RECEIVE イベントを溜め続ける。上限付きリングバッファ化 or push 前ドロップ + dropped カウンタ
6. **`DeliveryTracker::pack` が conn_id 上位16bit切り捨て** — `(conn_id << 48) | (seq & 0xFFFF_FFFF_FFFF)`。`conns ≤ 1000` では問題ないが、`conns = 2000+` で衝突が理論上起きうる。`(uint64)conn_id<<32 | (seq & 0xFFFFFFFF)` 化で安全
