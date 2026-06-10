# raknet/slikenet 共通化と安定化 — GUID 衝突と USER_THREADED teardown バグ

**測定日:** 2026-06-10
**目的:** `raknet` の run 間再現性崩壊 (game_server が 64↔1 で乱高下、valid_runs=1/3 多発) と、
`slikenet` が max_connections=1 のスタブだった問題を解消する。両 target は同じ bundled
SLikeNet (RakNet 4.082 derived, `RakNetLibStatic`) を使うため、adapter 実装を共通化した。

## 構成変更

- `adapters/raknet/rak_family_adapter.h` に共通 adapter を実装し、`raknet` / `slikenet`
  の両 target がこれを登録する (アダプタ実装差を排除。違いは登録名のみ)。
- `slikenet` は旧来「単一 RakPeer 共有」で同一 server への多重接続が張れず
  max_connections=1 だった → client 側 connection ごとに RakPeer を持つ方式 (旧 raknet
  実装由来) に統一し、max_connections=4096 になった。

## 根本原因 1: GUID = gettimeofday() のマイクロ秒値 (upstream)

SLikeNet の POSIX 実装は `RakPeerInterface::Get64BitUniqueRandomNumber()` が

```c
gettimeofday(&tv, NULL);
return tv.tv_usec + tv.tv_sec * 1000000;
```

で、**同一マイクロ秒に Startup した RakPeer は同一 GUID になる**。4 client process が
数十個の RakPeer を同時生成する本ベンチでは衝突が頻発する。server は OCR2 ハンドシェイクで
GUID 重複を検出すると `ID_ALREADY_CONNECTED` で接続を拒否する ("someone else took this
guid") が、旧 adapter はこれを「接続成功」として扱っていた。結果、client は connected・
server 側には存在しないゾンビ接続が測定に混入していた (canonical game_server c5 で
client conn_peak=5 / server conn_peak=4、delivery 0.63 の run の正体)。

**対処:** `ID_ALREADY_CONNECTED` と接続失敗系メッセージを受けたら RakPeer を作り直す
(GUID は Startup 時に 1 回だけ生成されるため、同じ peer での Connect リトライは永遠に
拒否される)。`is_connected()` の偽装 (failed → true) も廃止。接続回復は connect phase
中に完了するので、ゾンビが測定区間に入らない。

## 根本原因 2: conn ごとの update thread が client tick を壊す

per-connection RakPeer は既定で conn ごとに update thread + recv thread を立てる。
echo c200 (proc あたり 50 RakPeer = 100 thread) で client_tick invalid が多発していた。

**対処:** `RAKPEER_USER_THREADED=1` (upstream 提供のコンパイル設定) でビルドし、adapter
の poll() が `RunUpdateCycle()` を駆動する。update thread が消え、socket の blocking
recv thread だけが残る (idle 時は recvfrom でブロック、タイマー起床なし)。

### 派生バグ: USER_THREADED の teardown は use-after-free する (upstream)

- `Shutdown()` の recv thread 停止 (`SignalStop`/`BlockOnStopRecvPollingThread`) は
  `#if RAKPEER_USER_THREADED!=1` ガード内。一方 recv thread の生成は無条件。
  → 素の Shutdown は recv thread を止めずに socket と受信バッファを解放し、
  トラフィック下で SIGSEGV する (media c50 で全 run クラッシュを確認)。
- 自前で止めようにも `GetSockets()` は update thread へのクエリ実装で、update thread
  不在 (`isMainLoopThreadActive=false`) だと空リストで即 return する。RNS2_Berkley に
  到達する公開経路が無い。

**対処:** RakPeer を「破棄せず放棄」する (`abandon_peer`)。放棄 peer は接続拒否済みで
トラフィック無しか process 終了直前のものだけなので、リークは有界で OS が回収する。
close() では CloseConnection + 数回の RunUpdateCycle で切断通知だけ flush してから放棄。

## 試して断念したこと

- `USE_SLIDING_WINDOW_CONGESTION_CONTROL=0` (UDT 風 rate-based CC): SLikeNet v0.1.3 の
  CCRakNetUDT 経路は旧 RakNet の型名 (`RakNetTimeMS`) が残っておりビルド不能。
  submodule にはパッチしない方針のため断念。既定の CCRakNetSlidingWindow は TCP 風で、
  netem の 1% ランダムロスでウィンドウを半減し続けるため、fanout の unreliable が
  2-3% 遅延損する特性は残る (下記 media/game の上限の主因)。

## 結果 (境界点 N=3 中央値、canonical 手順)

`results/rak_boundary_n3` ほか (2026-06-10):

| point | 旧公開値 (raknet) | 今回 raknet | 今回 slikenet | 備考 |
|---|---|---:|---:|---|
| media c50 | 0.76 で flaky | 0.7623 (3/3) | 0.7691 (3/3) | CC 特性で gate 未達。last_ok=5 だが安定化 |
| game c64 | 64↔1 で乱高下 | 0.9483 (3/3) | 0.9787 (3/3) | gate(0.95) がノイズ帯 (0.93-0.98) 内 |
| game c96 | — | 0.9515 (N=1) | 0.9398 (3/3) | 同上 |
| echo c200 | invalid 多発 | 0.9677 ✓ | 0.9676 ✓ | client_tick invalid 解消 |
| echo c600 | — | 0.9402 | 0.9434 | break。CPU 118% でロスは CC 遅延 |
| reliable c600 | last_ok 50 | **0.9536 ✓** | **0.9528 ✓** | 50 → 600+ |

- 旧実装の特徴だった `aggregate_invalid:valid_runs=1/3` / `client_tick` / crash が全廃。
  全確認ポイントで 3/3 valid。
- slikenet は max_conns=1 のスタブ卒業 (echo/reliable 1 → 200/600 級)。
- raknet と slikenet は同一ライブラリ・同一 adapter なので、canonical 上の差は
  run-to-run ノイズのみ (game c64 の 0.948 vs 0.979 はその実例)。

## 盛らない注記

- game_server c64/c96 は delivery が gate 0.95 を跨ぐノイズ帯にあり、canonical の
  break 位置は run セットによって 5/64/96 の間で揺れうる。
- media c50 の 0.76 と echo c600 の 0.94 は接続安定化後の「ライブラリの実力」
  (sliding window CC が random loss を輻輳と誤認する特性)。adapter 側のレバーは
  尽きており、これ以上は submodule fork の領域。
- abandon (leak) は process-per-run のベンチ構成だから許される手段。長寿命プロセスへの
  流用は不可。

## 生データ

- 境界点 N=3: `results/rak_boundary_n3/`、ピンポイント: `results/rak_check_*` (`results/` は gitignore)
- クラッシュ解析 core: `coredumpctl` (RecvFromLoopInt → 解放済み eventHandler への仮想呼び出し)
- 全 lib の canonical 再計測 (公開済み): [`../2026-06-10-canonical-022240Z/report.md`](../2026-06-10-canonical-022240Z/report.md) — `current.md` もこの run を指す。他 lib の break 位置に回帰なし。
