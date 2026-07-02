# 改善ポイント一覧（2026-07-02 全実装レビュー）

adapter 14種・coop_rudp/apex_rudp の自作実装・C++ ハーネス・Go ベンチマークコントローラを
全部読み直して洗い出した改善ポイントの集約。既存の [`adapter-audit.md`](adapter-audit.md) /
[`tasks.md`](tasks.md) の残存項目と重複するものは参照で示し、今回新たに見つかったものを主に載せる。

## 実装状況（2026-07-02 同日実装）

本ドキュメントの改善ポイントは同日中に実装済み。全 25 C++ テスト + Go テストがグリーン。
実装が提案（改善案）と異なる形になった点:

- **§1.6 SACK/in-flight 整合**: ワイヤの ACK bitmap 拡張ではなく「reliable の
  発行 seq を un-ACK 最古から 64 以内にゲート」+「unreliable 専用パケットは
  seq=0 で送り seq 空間を reliable 専用にする」（skip_unreliable_acks=1 時）で
  解決。混在トラフィックで unreliable が SACK 窓を押し流す問題も同時に解消。
- **§2.2 apex の窓整合**: 同様に SEND_WINDOW=64 の送信ゲート方式（ワイヤ不変）。
- **§1 perf のうち以下は意図的に見送り**（コメントで明文化済み）:
  ring_push_priority の O(n) は priority=0 の fast path が既にありベンチは
  priority 未使用。reasm はハッシュ化ではなく `max_reassemblies` cfg 分離
  （adapter は 256）で死蔵 37MB とスキャン幅の両方を解消。ordered hold の
  O(k·H) は max_ordered_holds=1024 の稀パスのため現状維持。
- **§1.6 handshake**: cookie 導入ではなく `max_incoming_conns_per_poll`
  レート制限 cfg（ベンチ既定は無制限、`COOP_INCOMING_CONN_LIMIT` で有効化）。
- **§3.2-3.3**: library-default variant の追加ではなく、まず `cc_algo` /
  `thread_model` 列を全 adapter → raw CSV → scenarios/summary に通して
  結果表で分離可能にした（variant 追加は tasks.md に残置）。
- **§5.4 パーセンタイル統一**: C++ 2 実装と Go（histogram.go）を
  nearest-rank ceil(count*p) に統一。過去の bin ファイルから再計算すると
  端数ケースで 1 ランクずれることがある（旧 published 値との比較時に注意）。
- **§7.1**: resume は「res CSV 内の (run_id, library) 行の実在」判定 +
  Exec 前の該当行除去で重複追記も解消。**既存の results/ ディレクトリに
  過去の重複行が残っている可能性があるため、published 結果を再集計する
  場合は一度点検すること。**

影響度: **高** = 正確性・結果の信頼性を直接損なう / **中** = 特定条件で結果を歪める・運用リスク / **低** = 品質・保守性。

`file:line` は 2026-07-02 時点のコード参照。影響度「高」の項目はレビュー後に現物コードで裏取り済み。

---

## 0. 最優先サマリ

| # | 対象 | 内容 | 影響度 |
|---|------|------|--------|
| 1 | coop_rudp | `conn_map` からエントリが削除されず、スロット再利用で別コネクションに誤配送・新規接続が恒久失敗（[§1.1](#11-conn_map-にエントリ削除がない)） | 高 |
| 2 | apex_rudp | 受信 `conv` を無検証で `resize(conv+1)` — 不正パケット1発で ~16GB 確保を試み即死（[§2.1](#21-受信-conv-を無検証で-vector-インデックスに使う)） | 高 |
| 3 | apex_rudp | 送信ウィンドウ4096 vs 受信SACK窓64の不整合 — 損失/並べ替え環境で reliable 配送が破綻（[§2.2](#22-送信ウィンドウ4096-vs-sack窓64の不整合)） | 高 |
| 4 | Go sweep | resume の完了判定キー不一致で resume が一切効かず、結果CSVに重複行が追記される（[§7.1](#71-resume-の完了判定キーが不一致)） | 高 |
| 5 | harness | サーバCPU計測窓に無トラフィックの tail（2.5s+）が入り、capacity ゲート指標 `server_cpu_pct` が系統的に過小（[§5.1](#51-サーバcpu計測窓が-tail-のidle区間で希釈される)） | 高 |
| 6 | coop_rudp | `safe_bps`/`pacing_bps` を計算するだけで送信ゲートに使っておらず、輻輳制御が実質不在（[§1.3](#13-輻輳制御が実質不在)） | 高 |
| 7 | adapter横断 | 送信 backpressure の単位・閾値がバラバラ（lsquic dgram=64件、msquic=無制限）で `accepted_ratio` の意味が lib ごとに違う（[§3.1](#31-backpressure-の単位閾値が不統一)） | 高 |

---

## 1. coop_rudp（`coop_rudp/rudp.c` + `adapters/coop_rudp/coop_rudp_adapter.cc`）

### 正しさ

#### 1.1 conn_map にエントリ削除がない
**影響度: 高（裏取り済み）** — `abort_conn_internal`（rudp.c:912-940）は `c->active=0` にするだけで
`conn_map` からエントリを消さない（削除関数自体が存在しない。rudp.c:524-553 は lookup/insert のみ）。
`conn_by_index`（rudp.c:491-494）は active しか見ず conn_id を照合しないため:
- abort/idle timeout でスロットが別 conn_id の新規接続に再利用されると、古い conn_id の lookup が**新しい別コネクションを返す**（誤配送）
- linear probing + tombstone なしなので短命接続を繰り返すと used エントリが埋まり続け、`conn_map_insert` が -1 → **新規接続が恒久的に失敗**（rudp.c:1018）

改善案: `conn_map_remove`（tombstone または backward-shift deletion）を実装して abort から呼ぶ。lookup 後に conn_id 一致を必ず検証。

#### 1.2 adapter の conn ポインタキャッシュが conn_id を照合しない
**影響度: 高** — `remember_conn`/`find_conn`（adapter.cc:732-764）は wire_id→`rudp_conn*` 生ポインタを
キャッシュし `status.usable` だけで有効判定。1.1 と合わさると、スロット再利用後に**誤ったコネクションへ send する**。
改善案: コア側に index→conn_id 取得口を作り、キャッシュ検証時に照合する。

#### 1.3 輻輳制御が実質不在
**影響度: 高** — `safe_bps` は ACK 加算（rudp.c:695-697）/loss 半減（rudp.c:1633-1634）されるが、
`rudp_endpoint_flush`/`pick_next_msg`（rudp.c:1583-1605）はトークンバケットと DRR deficit のみで送出判断し、
**pacing_bps を送信ゲートに使っていない**（利用箇所は `rudp_get_status` の出力のみ）。実際の in-flight 上限は
sent_packet スロット（512）の空きだけの固定ウィンドウ。adapter-audit §5 の「custom rate-based AIMD」という記載は実態と異なる。
改善案: cwnd を導入して flush で in-flight を制限、pacing_bps を送信インターバルに実際に反映。

#### 1.4 ordered チャネルで seq 消費後の送信失敗が受信側を恒久ストールさせうる
**影響度: 中** — `rudp_send` は ordered の場合 `channel_seq = ch->next_ordered_seq++`（rudp.c:1455-1456）を
フラグメント確保ループの**前**に実行し、以降の `ring_push_priority` 失敗は戻り値を捨てる（rudp.c:1503-1505）。
seq に穴が空くと受信側 `expected_ordered` が永久に待つ。
改善案: 採番はキュー投入成功後に確定。ordered drop 時の skip フレーム（rudp.c:1536-1539 のコメントの設計課題）を実装。

#### 1.5 reliable dedup テーブルが有限 set-associative で衝突退避→重複配信
**影響度: 中** — `rx_seen` は 1024 bucket × 4 way（rudp.c:35-37, 1967-1993）。全 way 埋まると victim を
上書きし、退避された msg_id の再送が RELIABLE_UNORDERED でアプリに重複配信される。
また `rx_seen` 埋め込みで `sizeof(rudp_conn)` ~20KB、max_conns=4096 で conns 配列 ~86MB。
改善案: 「最小未受信 + スライディングビットマップ」方式にすれば有限メモリで取りこぼしゼロ。

#### 1.6 その他プロトコル
- **RTO 固定100ms・バックオフなし**（rudp.c:1657, 1695）。`srtt_ns` を計測しているのに使っていない（tasks.md 既存項目）。RFC6298 相当 + 指数バックオフ推奨。影響度: 中
- **ACK が max_seq 相対 64bit SACK のみ**（rudp.c:100-103, 464-489）。in-flight 上限512に対し ACK 到達範囲64。窓外は gap 判定（rudp.c:1004）で lost 扱い→不要再送。SACK 複数ワード化か in-flight を64に整合。影響度: 中
- **max_retransmits がパケット単位カウント**（rudp.c:1623-1652）。ピア一時停止で窓分が一括 lost 判定→64到達で正常接続を誤 abort。「連続 RTO ラウンド数」か「RX なし経過時間」基準に。影響度: 中
- **ハンドシェイクなしで受信パケットから conn 生成**（rudp.c:1951-1965）。spoof conn_id で conn テーブル枯渇可能。SYN/cookie か未確立接続の上限・レート制限。影響度: 中

### パフォーマンス

- **sent_packet 配列（512）の線形全走査が多発**: `service_retransmits`（rudp.c:1660）、`remove_msg_from_inflight_packets`（rudp.c:770）、`release_late_acked_messages`（rudp.c:829-846、do/while 再走査で最悪 O(n²)）。時刻順ヒープ + msg→packet 逆参照へ。影響度: 中
- **リング操作が O(n)**: `ring_push_priority`（rudp.c:369-394）/`ring_remove_value`（rudp.c:396-409）は全体回転。優先度別 FIFO と key→msg インデックスへ。影響度: 中
- **reassembly / ordered hold の線形走査**: `find_or_alloc_reasm`（rudp.c:2154-2185）はフラグメント毎に reasm_count（broadcast で 32768）走査、`drain_ordered`（rudp.c:2038-2056）は O(holds²)。さらに `reasm`/`reasm_data` ~37MB は adapter が単一フラグメント運用のためほぼ死蔵。ハッシュ表化 + 設定分離。影響度: 中（フラグメント使用時 高）
- **adapter 非同期 TX がパケット毎に `std::vector` heap 確保**（adapter.cc:307-314, 700-729）。リング+スラブ化。影響度: 中
- **seq 採番のバッチ内二重走査**: `next_packet_seq`（rudp.c:1810-1824）× `sent_slot_available`（rudp.c:1784-1802）で最悪 O(512×batch)。使用中ビットマップへ。影響度: 低〜中

### 責務分担

- **adapter がコアのワイヤフォーマットを二重定義してパース**（adapter.cc:30-41, 144-150）。コアの format 変更がサイレントに壊す。判定 API をコア側に公開。影響度: 中
- borrow API が実際は `borrow_data` へコピーしており「ゼロコピー借用」になっていない（rudp.c:2433, 2452）。命名と実体の乖離。影響度: 低

### テスト
`tests/test_coop_rudp.cc` に 1.1/1.2（スロット再利用時の誤参照・誤送信）と 1.3（cwnd 非適用）を突く回帰テストがない。最優先で追加。

---

## 2. apex_rudp（`adapters/apex_rudp/apex_rudp_adapter.cc`）

### 正しさ

#### 2.1 受信 conv を無検証で vector インデックスに使う
**影響度: 高（裏取り済み）** — `server_conn_for()` はリモートが任意指定できる 32bit `conv` で
`peer.id_by_conv.resize(conv + 1, 0)`（:672-673）。`conv=0xFFFFFFFF` で 1 ピアあたり ~16GB 確保を試み OOM/abort。
改善案: `unordered_map<uint32_t,uint32_t>` にするか conv 上限を設けて超過破棄。

#### 2.2 送信ウィンドウ4096 vs SACK窓64の不整合
**影響度: 高（裏取り済み）** — `MAX_PENDING_RELIABLE_PER_CONN = 4096`（:58, :456）に対し
`RecvSackWindow` の dedup/ACK 範囲は 64bit 固定（:286, :355）。in-flight が64を超えた状態で
`highest_` が未ACKパケットより64以上先行すると、受信側は `insert()` が false を返して**未配送のまま破棄**し続け、
ACK bitmap でも表現できないため送信側は永久再送 → `reliable_timeout`（10s）で接続ごと落ちる。
ループバック無損失では顕在化しないが、損失/並べ替えのある実網で reliable 配送が破綻する。
改善案: 送信ウィンドウを SACK 幅以下に制限するか、SACK を複数ワード/範囲ベースに拡張して必ず連動させる。

#### 2.3 その他プロトコル
- **輻輳制御・ペーシングなし**。バックプレッシャは pending 4096 + TXキュー上限のみ。ベンチ専用と割り切るなら明示コメントを。影響度: 中
- **RTO 固定100ms・バックオフなし・RTT計測なし・fast retransmit なし**（:61, :1323）。持続損失下で 100ms 周期の再送ストーム。SACK の穴を使った即時再送 + 適応 RTO へ。影響度: 中
- **任意 src からのペイロードで無制限に Conn 生成**（:655-686）。`max_connections()` 未実装。接続数上限か cookie を。影響度: 中

### パフォーマンス・保守性

- **タイマ処理が毎回全 Conn 線形走査**: `service_standalone_acks`（:1361）/`service_retransmits`（:1306）。ACK 遅延2ms 既定で 1000conn だと高頻度 O(N)。dirty リスト or 最小ヒープへ。影響度: 中
- **`direct_batch_` が pending.bytes への生ポインタを保持**（:699）。「flush → drain」の厳密順序（:553-565）と `drop_direct_refs_for`（:1346）に UAF 回避を全面依存する脆い設計。所有バッファ化か不変条件のコード強制を。影響度: 中
- **ホットパス heap 確保**: `send_many` の `FanoutJob::bytes`（:506-507）、`send_batch` の作業バッファ（:958-961）がプール外。影響度: 低〜中
- `reliable_timeout()` が毎回 getenv（:262-268）。static キャッシュへ。影響度: 低
- batch フレーム内の `FLAG_BATCH` で再帰しうる（:1214→:1172）。ネスト禁止を明示。影響度: 低
- `recv()` の予算/空ドレイン制御が複雑（:537-551）。cap 超過メッセージは -1 でサイレント破棄（`inbound_queue.h:44`）。扱いの明文化を。影響度: 低〜中

---

## 3. adapter 横断の公平性

### 3.1 backpressure の単位・閾値が不統一
**影響度: 高** — バイト32MB基準（enet/kcp/quiche/lsquic reliable/udt4/raknet/litenetlib）、
メッセージ数基準（mini_rudp=65536、yojimbo=4096、**lsquic unreliable=64件** lsquic_adapter.cc:32,367-369）、
**上限なし**（msquic は send が常に submit、msquic_adapter.cc:571-615。gns も send 側で事前チェックなし）が混在。
`client_accepted_ratio` の意味が lib ごとに変わり、特に lsquic dgram の64件は負荷時に不当に早く送信拒否する。
改善案: 全部を32MBバイト基準に統一（lsquic dgram もバイト化、msquic にも未ACK bytes 上限）。統一不能な lib は backpressure ポリシーを CSV 列に出す。

### 3.2 CC の土俵が揃っていない
**影響度: 高** — CC 無効群（kcp nocwnd kcp_adapter.cc:81、udt4 BenchCCC udt4_adapter.cc:59-65、gns SendRateMax=256MB/s gns_adapter.cc:71、mini_rudp、apex、coop※実質）と BBR 群（quiche/lsquic/msquic）が同じ表で比較される。
tasks.md の「library-default variant 追加」項目と同根。最低限 `cc_algo` を CSV 列化して結果表で分離。

### 3.3 内部ワーカースレッドの非対称
**影響度: 高** — msquic は `poll()` 完全 no-op で内部スレッドプール駆動（msquic_adapter.cc:281-283）、gns もサービススレッド1本。単一スレッド勢（enet/kcp/raw_udp/mini_rudp/udt4）と並列度が違う。
dev-notes §5.2 の thread_model 併記に加え、スレッド数/使用コア数を CSV 出力し、必要ならワーカーを1コアに束縛した variant を。

### 3.4 その他の非対称（影響度: 中）
- **最大ペイロード不揃い**: yojimbo 4096B / litenetlib 1000B（Program.cs:705）は大メッセージシナリオで**無言の -1**（accepted_ratio 低下）になる。runner 側で size が対象 adapter の `max_payload_bytes` 外なら skipped 行として明示すべき
- **暗号**: QUIC 系 + yojimbo（libsodium）常時 ON vs その他 OFF。`encryption` 列で開示済みだが、暗号ありの土俵での同条件対決行（gns_encrypted 等）を1つ用意する
- **ソケットバッファ**: gns だけ既定 4MB（gns_adapter.cc:63）に逸脱。udt4/msquic/yojimbo/raknet/litenetlib は未調整（adapter-audit §1 残存と同根）
- **Nagle 相当**: gns は 5ms coalescing 既定 ON（gns_adapter.cc:54）、yojimbo は 100Hz 固定送信で全メッセージに最大10msのレイテンシ床（yojimbo_adapter.cc:211-231）。broadcast スループット比較にコアレシング戦略が混入する点を注記

---

## 4. 個別 adapter のバグ・改善点

| adapter | 内容 | 影響度 |
|---------|------|--------|
| quiche | `process_conns()` が全 readable ストリームを単一 `stream_recv_buf` に連結してフレーム解析（quiche_adapter.cc:435-453）。「1接続1受信ストリーム」で偶然成立しているだけで、複数ストリームが readable になると境界が壊れる。ストリームID毎にバッファ/フレーム状態を持つべき | 中 |
| raknet/slikenet | `close()` が接続済み slot ごとに `flush_disconnects`（2ms×5 sleep、rak_family_adapter.h:227-244）。多 conn client で close_ms が conn数×10ms に膨張。まとめて1回の update サイクルで flush する | 中 |
| raknet/slikenet | client が接続ごとに `RakPeerInterface`（=socket + 無条件 recv スレッド、rak_family_adapter.h:122-151）。高 conn でスレッド爆発（litenetlib が解消した問題と同型） | 中 |
| msquic | 送信 backpressure なし（§3.1）に加え `close()` no-op + cert の `/tmp` リーク | 中 |
| quiche/lsquic/msquic | `/tmp/rudp-bench-*-{cert,key}.pem` を生成して掃除しない（quiche_adapter.cc:60-77 ほか）。atexit で unlink | 低 |
| enet | `send()` だけ `len > max_payload_bytes` の入力ガードがない（enet_adapter.cc:318-345）。他 adapter は全部先頭で検証 | 低〜中 |
| enet | server の RECEIVE/DISCONNECT で未知 peer に id 採番するが `connected_ids_` に入れず `is_connected()`=false になりうる（enet_adapter.cc:404-431） | 低 |
| gns | `connection_stats()` 未オーバーライドで常に {0,0,0}。`connected_ids_` は追跡しているのに conn_peak/切断数が出ない | 中 |
| kcp | unreliable は KCP バイパスの生 sendto で送信キュー上限のカウント対象外（kcp_adapter.cc:335-348） | 低 |
| udt4 | `flush_pending` が deque 前方から copy+erase（udt4_adapter.cc:299-320）。大量残留で O(n²) 寄り。ring/offset 方式へ | 低 |
| udt4 | `UDT::connect` 同期ブロッキング（udt4_adapter.cc:147-151）。高 conn の接続確立が直列化（ramp 頼み） | 中 |
| raw_udp | server `recv()` が oversized datagram で -1 を返し、harness の drain ループ（runner.cc:227-230）が break してその tick の残り受信が止まる。drop 扱いで継続すべき | 低 |

補足: raknet/slikenet の `.cc` はスタブではない。両方 `rak_family_adapter.h`（562行）の共通実装を登録する薄いラッパーで、per-connection RakPeer の完全実装。

---

## 5. 計測方法論（harness）

### 5.1 サーバCPU計測窓が tail の idle 区間で希釈される
**影響度: 高（裏取り済み）** — サーバは `measure_begin = start + warmup + ramp`（runner.cc:199-200）から
計測を始めるが、終了は `deadline = … + server_tail_ms`（≥2000ms、runner.cc:186,192-194）まで。
クライアント送信は run_end で止まるため、CPU 平均の分母に**約2.5秒以上の無トラフィック区間**が入り
`server_cpu_pct` が過小評価される。これは capacity ゲートの主要指標（sweep.go）なので影響が大きい。
改善案: サーバにもトラフィック終了時刻を導入し、CPU 計測窓を traffic 終了で閉じる（tail poll は継続、窓からは除外）。

### 5.2 送信タイムスタンプが tick 先頭の now を全 conn で共有
**影響度: 中** — `now` はループ先頭で1回だけ取得（runner.cc:467）され、payload に書く送信時刻 `ts` に
全メッセージで共有される。高 conns で 1 tick が数十ms になると、tick 後半のメッセージの RTT が
**最大 tick 長ぶん過大**に出る（受信側 `t_recv` は毎回 fresh で非対称）。
改善案: `try_send_on` 内で送信直前に取り直す（pacing 判定用とは分離）。

### 5.3 coordinated omission
**影響度: 中** — RTT は実送信時刻基準のため、送信ループ停滞（`pacing_lag`）はレイテンシに含まれず楽観側に偏る。
`pacing_lag_us` は diagnostics に出るが report.md に載らない。スケジュール時刻基準の corrected latency 併記か、
report で `pacing_lag_p99` と RTT を並べて出す。

### 5.4 その他（影響度: 低〜中）
- **スループットの分子だけ tail 込み**: 受信集計は `in_measure(t_recv)` に上限がなく（runner.cc:500-502）、tail 到着分も加算されるのに `duration_s` で割る（:547-548）。スループット集計だけ `t_recv < run_end` に限定を
- **パーセンタイル定義が2実装で不一致**: `LatencyHist`（metrics.cc:61-72、floor+bin上限で最大1ms上方バイアス）vs `BoundedHistogram`(runner.cc:34-44、ceil)。丸め規約を統一
- **netem が lo で双方向に効く**点（RTT 2倍・loss ~2倍）は dev-notes §1.6 に記載済みだが、canonical の delivery ゲート 0.95 が unreliable の理論上限（1% loss 双方向 ≈0.98）に近い。ゲート値を loss 前提で見直すか明記
- **実行順が固定**（sweep.go:191-253、lib 外側・run 内側・lib 順固定）。熱ドリフト/キャッシュ状態が特定 lib に系統的に効く。run のラウンドロビン化 or lib 順シャッフル
- **CPUピークが100ms窓平均の最大**（runner.cc:108, proc_sampler.cc:62-75）。サブ100msスパイクは平滑化される。意味をレポートに明記

---

## 6. ハーネス実装バグ

### 6.1 broadcast で outstanding が発散し adaptive idle が無効化
**影響度: 中** — broadcast では accepted に fanout 全量（runner.cc:436-447）を足すが、自プロセスは
担当 conn 分しか受信しない（:509）ため `outstanding` が 0 に戻らず単調増加。
（1）`client_outstanding_max` が無意味な巨大値（2）`outstanding==0` を要求する adaptive idle（:515-517）が
発火せず常時 spin。broadcast では期待受信数（origin×自conn数）で勘定するか idle 判定を無効化。

### 6.2 その他（影響度: 低）
- adaptive skip の `minValid` 引き下げが同一 conns 点の**全 lib**に波及（sweep.go:242-264）。skip した lib だけに限定すべき → §7.2 と同根
- inbound queue の `dropped()`（inbound_queue.h:18）が CSV に出ない。診断列に追加すれば delivery 低下の切り分けが速くなる
- マルチクライアント集計で `msgs = msg_per_sec × duration` の整数復元誤差、CPU% は proc 単純合計（combine.go:107-130, 224-229）。総メッセージ数を CSV に直接出力して合算を

---

## 7. Go ベンチマークコントローラ

### 7.1 resume の完了判定キーが不一致
**影響度: 高（裏取り済み）** — `CompletedPoints` は `completed[runID] = true`（manifest.go:153、lib を含まない）
だが、参照側は `completed[runID+"/"+lib]`（sweep.go:195）。キーが絶対に一致せず **resume が一切 skip しない**。
さらに `result.Append` は既存 `res_<runID>.csv` に追記（csv.go:113）なので、resume/再実行で同一 run の行が重複し、
`Aggregate` の中央値が重複行の上で計算される。
改善案: 完了判定を「res ファイル内の (run_id, library) 行の存在」で行うか、Exec 前に該当 lib 行を除去。
少なくとも生成キーと参照キーを一致させ、既存 out ディレクトリの重複行有無を一度点検する。

### 7.2 プロセス・環境のクリーンアップ（影響度: 中）
- **Ctrl-C 時に harness が孤児化しうる**: 子は `timeout`（isolate.go:114-138、`Setpgid: true`）で、context cancel の SIGKILL は timeout にしか届かない。`cmd.Cancel` でプロセスグループごと kill するか `WaitDelay` 設定
- **netem/CPU隔離の残留**: teardown は `defer` のみで、前段 return や `os.Exit` 経路では走らない。起動時に前回残留の qdisc/隔離をクリアする冪等ステップを追加

### 7.3 その他（影響度: 低）
- `ApplyNetem` の引数を数値検証しない。jitter>0 で netem は reorder を起こす（reliable は吸収するが unreliable delivery に影響）点も未注記（netem.go:10-41）
- `dominant()` の同数タイブレークが map 反復順依存で `note` 列が非決定的（aggregate.go:99-119）。辞書順タイブレークへ
- `loadPrior` が `ReadDir` の名前順最後=最新という暗黙前提（main.go:298-312）。mtime ソートへ
- `concatFiles`/`os.WriteFile` 等の I/O エラー黙殺が複数（exec.go:463-482, netem.go:45-46 ほか）。warning 出力を
- report の profile 対応付けが scenario_id 正規表現 + shape 一致依存（report.go:15, 109-140）。profile 名を CSV の明示列にして join

---

## 8. 既存 docs との対応

- [`tasks.md`](tasks.md) の未完了項目（socket buffer diagnostics 残り、msquic buffer、raknet recv thread、固定 RTO 群、library-default variant 等）は引き続き有効。本ドキュメントの §1.6（coop RTO）、§3.2（CC variant）はその具体化
- [`adapter-audit.md`](adapter-audit.md) §5 の coop_rudp「custom rate-based AIMD」は §1.3 の通り**送信ゲートに未接続**なので、audit 側の記載も要更新
- 計測指標の解釈ルールは [`dev-notes.md`](dev-notes.md) §5 参照。本ドキュメント §5 はそこに未記載の系統誤差を追加するもの

### 推奨着手順

1. **結果の信頼性を直す**: §7.1（resume/重複行）→ §5.1（サーバCPU窓）→ §3.1（backpressure 統一）。これまでの published 結果の再解釈が必要かもここで判断
2. **自作実装の実バグ**: §1.1/§1.2（coop 誤配送）→ §2.1/§2.2（apex DoS・SACK 窓）。いずれも損失/長時間/悪意入力で顕在化する類で、ループバック smoke では見えない
3. **プロトコル品質**: §1.3（coop CC 接続）、§1.6/§2.3（適応 RTO）、fast retransmit
4. **公平性の可視化**: `cc_algo`/`thread_model`/backpressure ポリシーの CSV 列化、library-default variant（tasks.md 既存項目）
