# RUDP ライブラリ横断ベンチマーク設計

- 作成日: 2026-04-28
- 言語/プラットフォーム: C/C++ on Linux
- 状態: 設計中(ユーザーレビュー待ち)

## 目的

C/C++ 系の主要な信頼性 UDP / RUDP / QUIC 実装を、揃った条件で横並び比較する。
比較軸は信頼性・パケットサイズ・同時接続数に加え、過負荷時の取りこぼしとネットワークロスへの耐性も含む。

成果物は2段階:

1. **Phase 1 (スイープ)**: 各軸2点ずつの粗いマトリクスで全ライブラリを叩き、比較表を作る
2. **Phase 2 (詳細)**: Phase 1 の結果を見て対象ライブラリを絞り、軸ごとに1次元スイープしてグラフ化する

## 対象ライブラリ

| 略称 | ライブラリ | 備考 |
|---|---|---|
| ENet | enet | ゲーム向け軽量 |
| SLikeNet | SLikeNet (RakNet OSS fork) | 老舗高機能 |
| KCP | skywind3000/kcp | ARQ、低遅延寄り |
| GNS | Valve GameNetworkingSockets | Steam由来 |
| yojimbo | networkprotocol/yojimbo | Glenn Fiedler、ゲーム特化 |
| UDT4 | UDT4 | 大容量転送向け |
| msquic | microsoft/msquic | QUIC、reliable=stream / unreliable=datagram (RFC 9221) |

msquic は TLS ハンドシェイク必須のため接続確立コストが他より重い。`large conn` 軸でこの差が観測されることを注記する。

## 環境

- 単一ホスト・loopback (`127.0.0.1`)
- ロス注入は `tc qdisc add dev lo root netem loss X%` を `loopback` に対して適用
- ベンチ中は他プロセス影響を避けるため、pinned CPU / niceness 調整(必要なら `taskset`、`nice -n -10`)
- server / client は別プロセスで起動(CPU 計測を分離するため)

## 計測指標

| 指標 | 単位 | 取り方 |
|---|---|---|
| Throughput | Mbps, msg/sec | 受信側で 1秒ウィンドウ集計 |
| RTT | μs (p50/p95/p99) | アプリ層エコーで往復測定 |
| Delivery ratio | delivered/sent | sender が seq を打ち、receiver が受領 seq セットから算出 |
| CPU | % | server / client それぞれ `getrusage` ベース |
| RSS | MB | `/proc/self/status` の VmRSS |
| Connect cost | ms | `client_connect` 開始〜送受信開通まで |

## シナリオ軸

| 軸 | Phase 1 値 | Phase 2 刻み(暫定) |
|---|---|---|
| 信頼性 | reliable / unreliable | Phase 1 結果で確定 |
| パケットサイズ | 64 B / 64 KB | 32, 128, 512, 2 K, 8 K, 32 KB |
| 同時接続数 | 1 / 1000 | 1, 10, 100, 500, 1000, 2000 |
| 送信レート | 100 msg/sec/conn / 飽和超え | 飽和の 10%〜200% を log 10点 |
| 注入ロス率 | 0% / 5% | 0, 0.5, 1, 2, 5, 10% |

Phase 1 のマトリクス: 2⁵ = **32 シナリオ × 7 ライブラリ = 224 ラン**(各 30 秒、約 2 時間)。
当該モードを持たないライブラリ(例: UDT4 の unreliable)はそのシナリオを実行せず CSV に `na` を記録するため、実ラン数は 224 より少なくなる。

「飽和超え」レートの決め方: 送信レートを 100 → 1k → 10k → 100k msg/sec で段階上げし、**到達率が 95% を割る、または送信側 CPU が 95% に張り付く** のいずれかが起きた直近のレートを「飽和」とする(再現性確保のため、無制限送信ではなく段階法)。

Phase 2 の基準点(他軸を固定する値): `reliable, 256B, 100conn, 中レート(基準ライブラリの飽和の50%), ロス0%`。
信頼性軸でのスイープ重複の扱い(全部 reliable/unreliable 両方出すか、ロス率スイープだけか等)は **Phase 1 完了後に決定**。

## アーキテクチャ

```
rudp-bench/
├── harness/
│   ├── adapter.h            # 抽象 IF
│   ├── runner.cc            # シナリオドライバ
│   ├── metrics.cc           # 集計
│   └── scenarios.yaml       # シナリオ定義
├── adapters/
│   ├── enet/
│   ├── slikenet/
│   ├── kcp/
│   ├── gns/
│   ├── yojimbo/
│   ├── udt4/
│   └── msquic/
├── third_party/             # git submodule
├── scripts/
│   ├── run_phase1.sh
│   ├── run_phase2.sh
│   └── plot.py
├── results/
│   ├── phase1.csv
│   ├── phase1_table.md
│   └── phase2/
│       ├── *.csv
│       └── plots/*.png
└── docs/
```

### Adapter 抽象

各ライブラリは以下の IF を実装する:

```cpp
struct Adapter {
  virtual void server_listen(uint16_t port) = 0;
  virtual void client_connect(const char* host, uint16_t port) = 0;
  virtual void send(uint32_t conn_id, const void* data, size_t len, bool reliable) = 0;
  virtual size_t recv(uint32_t conn_id, void* buf, size_t cap) = 0; // non-blocking
  virtual void poll() = 0;   // イベントポンプ駆動
  virtual void close() = 0;
  virtual const char* name() const = 0;
};
```

QUIC (msquic) は `reliable=true → stream`、`reliable=false → datagram (RFC 9221)` にマップする。
UDT4 のように非信頼モードを持たない実装では unreliable シナリオは `N/A` で結果に記録する。

### Runner

CLI: `rudp-bench --library=enet --role=server|client --scenario=<json> --out=<csv>`

役割:
1. 指定 adapter を生成し `listen` または `connect`
2. シナリオ仕様に従って送信ループ + 受信ループ + メトリクス採取を別スレッドで動かす
3. 終了時に集計結果を CSV 1行で stdout または `--out` に書く

### Scenario runner script

`scripts/run_phase1.sh` がやること:

1. シナリオごとに `tc qdisc` でロス率を設定
2. `server` プロセスを起動してポート待ち合わせ
3. `client` プロセスを起動して計測実行
4. 双方の CSV 行を `results/phase1.csv` に追記
5. 後始末(`tc qdisc del`、プロセス停止)

### ビルド

- CMake + git submodule
- 各ライブラリは `third_party/<lib>/` に submodule で取り、`add_subdirectory` で取り込む(find_package が効かないものが多いため)
- 単一トップレベル `cmake --build` で全 adapter ビルド可能にする

## 出力フォーマット

### CSV(1ラン1行)

```
library,phase,reliable,size,conns,rate,loss,
throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,
delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s
```

`reliable` は `r`/`u`/`na`(該当ライブラリが当該モード非対応の場合 na)。

### Phase 1 アウトプット

- `results/phase1.csv`: 全 224 行
- `results/phase1_table.md`: ライブラリ × シナリオのピボット表。throughput と delivery_ratio を主指標に色/記号で強調

### Phase 2 アウトプット

- `results/phase2/<axis>.csv`: 軸ごとの 1 次元スイープ結果
- `results/phase2/plots/<axis>_<metric>.png`: matplotlib で軸ごと折れ線(ライブラリ別の線)

### 後処理ツール

Python + pandas + matplotlib(`scripts/plot.py`)。C++ 側は CSV 吐くだけ、整形・可視化はすべて Python 側に寄せる。

## エラーハンドリング

- ある adapter / シナリオの組み合わせがクラッシュした場合: その行は `failed` として CSV に記録し、次のシナリオに進む。全体停止はしない
- ライブラリが該当モード(例: UDT4 の unreliable)を持たない場合は `na` を立て、計測しない
- タイムアウト: 各ランに wall-clock のハードリミット(scenario duration の 3 倍)を設け、超えたら kill

## テスト戦略

- `harness/runner.cc` の単体テストは loopback で 1 シナリオを回す smoke テスト(adapter は mock または ENet を 1 ライブラリだけ使う)
- adapter ごとの最小疎通テスト: 64B reliable を 1 メッセージ送って echo 戻り、を `ctest` に登録
- フルスイープは CI には載せない(2時間級のため、ローカル実行)

## 未確定 / 後で決める

- Phase 2 の信頼性軸スイープ重複の扱い(Phase 1 結果で判断)
- yojimbo の認証フローの扱い(必要なら共有鍵を embed)
- msquic の TLS 自己署名証明書の取り扱い(adapter 側で生成)
- UDT4 のメンテナンス状態が古い場合のフォールバック先(候補: UDT C++11 fork)

## 段階的着手順

1. リポジトリ初期化、CMake 雛形、`adapter.h`、ENet adapter 1本だけで `runner` の疎通を取る
2. ENet で Phase 1 シナリオを 1 セット回し、CSV 出力を固める
3. 残り 6 ライブラリの adapter を順次追加(1ライブラリ 1 PR/コミット粒度)
4. `run_phase1.sh` 完成、tc 注入確認、Phase 1 全 224 ラン実施
5. 結果を見て Phase 2 の対象ライブラリと軸刻みを確定、Phase 2 実施
