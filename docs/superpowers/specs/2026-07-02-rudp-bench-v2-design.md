# rudp-bench v2 設計 — idiomatic server 方式によるゼロベース再構築

- 作成日: 2026-07-02
- 状態: レビュー済み(ユーザー + Codex 第二意見を反映)・実装可
- 前提: [v1 設計](2026-04-28-rudp-bench-design.md) の後継。要素は流用するが構造はゼロベース。
  この repo 内で v2 ツリーを育て、移植完了後に v1 を削除する

## v1 の教訓(なぜ作り直すか)

v1 は「harness が event loop を所有し、adapter が抽象 IF でそれに合わせる」構造だった。
2ヶ月の運用で得た教訓([dev-notes](../../dev-notes.md) §1、[improvements](../../improvements.md)):

1. **ループのはめ込み事故が数桁級の誤測定を生む**: yojimbo の空パケット洪水(0.42→0.989)、
   msquic の poll() no-op、mini_rudp の client 律速。adapter の作りが結果を桁で変える。
2. **時刻計算ベースの lifecycle は窓ズレのバグ工場**: ramp 窓ズレ(§1.7)、multi-proc 窓ズレ。
   「delivery が duration 非依存の一定割合で欠ける」系は全部これ。
3. **検証手段が高価な canonical 一式 + 人間の違和感検出しかない**: アーティファクト発見の
   たびに全再計測。収束しない。
4. **公平性のもぐら叩き**: backpressure/CC/thread model を「揃える」ゴールには終わりがない。
5. **自作 lib(coop/apex)の開発ループと計測器の信頼性が絡まる**: 被計測物のバグ修正と
   計測器の修正が同時に着地し、切り分け不能になる。

## 目的と測定対象の定義

**測定対象(measurand)**: 「ライブラリ X で idiomatic に書かれた server/client が、
所定 workload・所定ネットワーク条件で発揮する容量・配送・遅延・資源特性」。
**full-stack(client 実装込み)が測定対象**であり、「lib を抽象 IF 越しに叩いた値」でも
「server 単体の理論性能」でもない。これはユーザーが実際にデプロイするものの近似。
adapter 起因の歪みを「なくす」のではなく、**律速要因を毎回機械判定して開示する**。
server capacity として読む場合の妥当性は、client farm の十分性 gate と censored 表記が
守る(「client farm の計測規約」参照)。

### 設定ポリシー(2段のみ)

| policy | 定義 |
|---|---|
| `library-default` | ライブラリ既定値。ソケットバッファ含め一切いじらない |
| `tuned-disclosed` | 調整可。ただし全調整項目を metadata で開示(cc_algo, buffers, thread 設定…) |

publish では両ポリシーを常に併記する(見出し図に両方を出し、`tuned` の優位が
チューニング量の差でないことを常時反証可能に保つ)。`tuned-disclosed` の調整は
**upstream の公式ドキュメント・推奨に根拠を持つものに限る**(自由探索の最適化合戦に
しない)。「この設定は公平か」という議論はこの2段への割当てで即決し、以後蒸し返さない。

### 非目標

- apex_rudp / coop_rudp の維持(**廃止**。将来の自作 lib は外部 lib と同一の入口=
  conformance → smoke → 校正 → canonical 参加、で入る)
- 完全な公平性(開示と層別で代替)
- Linux 以外のプラットフォーム

## 対象トランスポート

v2 は「RUDP 横断」から「リアルタイム通信トランスポート横断」に広げる。答える問いに
「RUDP/QUIC は、実運用で選ばれがちな TCP 系スタックに対して何をどれだけ改善するか」を含める。

**minimum-first**: 最初から全幅を移植せず、必須セットで完結させてから追加する。
入場経路(conformance → smoke → 校正 → canonical 参加)が最初から追加参加のために
設計されているので、後から足しても既存結果の解釈は壊れない(ブロック内比較のため
追加時は同一ブロックでの再計測が必要、というだけ)。

ランキング必須セット(6):

| transport | 系 | カバーする極端 |
|---|---|---|
| enet | UDP / C++ | 単一スレッド service loop |
| gns | UDP / C++ | マルチスレッド・暗号必須 |
| litenetlib | UDP / C# | .NET ランタイム |
| msquic | QUIC | 内部スレッド + callback。外部アンカー(secnetperf)あり |
| websocket | TCP | 実装は要選定(候補: uWebSockets / ASP.NET Core) |
| magiconion | TCP / .NET | gRPC over HTTP/2 RPC スタック。StreamingHub を idiomatic 実装とする |

追加候補プール(後続、必要になったら入場経路を通す): kcp, slikenet, udt4, yojimbo,
quiche, lsquic

raknet は不参加(slikenet が maintained fork で系譜が重複するため)。
mini_rudp / coop_rudp / apex_rudp は廃止(自作実装は v2 に持ち込まない)。

**校正専用機材**(非ランキング・profile 不参加。「公平な対戦相手」ではなく、
真値が構成から自明な試験機材であることが資格要件):

| 機材 | 役割 | 校正できる会計 |
|---|---|---|
| null | 通信しないダミー(send が同一プロセス内で受信キューに直結)。**会計の零点** | benchkit の全集計経路(delivery=1.0 厳密、staleness=サンプル周期、ズレたら会計バグ)。netns/sudo 不要で CI 常設可 |
| fault_inject | null に既知の複製・並べ替え・遅延を決定論的に注入する shim | dedup・重複計数・遅延会計(注入した真値と報告値の一致を検査)。CI 常設可 |

自作の校正機材はこの2つのみで、どちらも通信しない(校正コード自体に通信バグが
混入する余地がない)。ネットワーク層と実線の校正は自作せず外部に委譲する:

- **ネットワーク層(netem 実効値・RTT 零点・pps 天井)**: 標準ツール(ping / iperf3)を
  netns 内で走らせる pre-run gate として実施。ベンチ相当の pps・burst で流し、
  loss/delay の実測が設定値と一致することを確認(v1 の netem limit=1000 型を捕捉)。
  pps 天井は iperf3 経路と benchkit 経路が異なる点を注記の上、rig の floor 指標とする
- **必達会計の実線検証**: TCP 系のランキング参加者(magiconion / websocket)は kernel
  保証により must-deliver=1.0 が真値として既知なので、そのまま校正を兼ねる
- **「素の TCP」参照線**: 専用機材は設けない。TCP 系はランキング内に websocket /
  magiconion がいるので、参照はそちらで足りる

校正は「null(会計)→ fault_inject(dedup・遅延)→ 標準ツール(ネットワーク層)→
TCP 系参加者(実線の必達)」の梯子構造で、fail した段が故障箇所を示す。

## アーキテクチャ

```
rudp-bench/
├── benchspec/            # ワイヤ契約・意味論・バリアプロトコルの仕様書(言語非依存)
├── benchkit/             # 薄い共有 lib (C ABI): payload codec / metrics ring /
│                         #   bin log writer / バリア client / config parse
├── servers/<lib>/        # lib ごとの idiomatic server + client(自前ループ・自前スレッド)
├── orchestrator/         # Go: netns/veth, netem, バリア同期, sweep 計画,
│                         #   validity gate, 集計(曲線+CI), report, publish
├── calibration/          # 校正スイート(理想 transport / 既知損失 / 不変性 / 回帰)
├── third_party/          # submodule(流用)
├── scripts/              # netem / isolation(流用・netns 対応に改修)
└── docs/
```

v1 の `harness/`(runner.cc, adapter.h)と `adapters/` は移植完了後に削除する。

**v1 との同居規則(ごっちゃ防止)**:

- v2 のコードは上記の新規 top-level ディレクトリ(`benchspec/`, `benchkit/`,
  `servers/`, `orchestrator/`, `calibration/`)にのみ置く。v1 の `harness/` /
  `adapters/` / `cmd/rudp-benchctl` / `scripts/` には手を入れない(バグ修正含め凍結)
- ビルドも分離: v2 の C/C++ は独立した CMake ターゲット群(`benchkit`,
  `server_<lib>` 命名)、Go は root go.mod 共有のまま `orchestrator/` パッケージ配下。
  v1 のビルドが v2 の変更で壊れない/その逆もないこと
- `third_party/` submodule と `docs/` は共有。v2 の結果 publish 先は
  `docs/measurements/`(v1 と同じ流儀、report 側に spec version を明記)
- v1 の canonical は v2.0 完了条件の突き合わせ(enet/msquic の v1 結果との整合確認)が
  済むまで再実行可能な状態を保つ

### benchspec: ワイヤ契約

**payload レイアウト**(min 24B、全 profile の payload ≥64B なので充足):

```
offset size field
0      8    seq          (u64 LE, per-conn per-class 連番 = 送信 slot id)
8      8    sched_ts_ns  (u64 LE, この slot を本来送るべきだった予定時刻)
16     8    send_ts_ns   (u64 LE, 実送信直前の時刻。CLOCK_MONOTONIC、プロセス間比較可)
24     1    flags        (bit0=traffic class: 1=must-deliver / 0=loss-tolerant,
                          bit1=measurement-window,
                          bit2=distribution: 0=echo / 1=broadcast)
25     4    origin_id    (送信元の global conn id。常に送信者が記入)
29     3    reserved
32-    pad  指定 payload サイズまで充填
```

**coordinated omission 対策**: 遅延・鮮度の会計は `sched_ts_ns`(送信計画)を起点に
行う。client が詰まって送信が遅れた分・backpressure や coalescing で送られなかった
slot も、受信側に現れない miss として分母に入る(client は未送信 slot を bin log に
記録する)。`send_ts_ns` 起点の値は transport 単体の遅延として診断用に併記。

**workload 意味論**: server は mode を持たない単一プログラムで、受信 payload の
flags だけで振る舞いを決める:

- distribution bit=echo → 送信元にのみ、同一 traffic class で返す
- distribution bit=broadcast → 現在の全接続(origin 含む)へ同一 class で fanout。
  1 メッセージあたりの期待受信数 = 接続数(delivery の分母はここから機械的に決まる)

したがって **profile は client の送信ポリシーの記述にすぎない**(どの class・どの
distribution のメッセージを、何 Hz・何 byte で、何接続から送るか)。同一 server 起動の
まま client 側だけで echo / broadcast / 混合のあらゆる workload を構成できる。

**traffic class(channel 指定の廃止)**: profile が指定するのは transport の channel
ではなく、データの意味論 — `loss-tolerant`(latest-value、落としてよい)と
`must-deliver`(必達)の2クラス。class → 自 transport の機構へのマッピングは
各実装が宣言して開示 metadata に出す:

- RUDP/QUIC 系: loss-tolerant → unreliable/datagram、must-deliver → reliable/stream
- TCP 系(websocket/magiconion)・reliable-only(udt4): 両 class とも reliable stream
  (loss 下の HoL コストが loss-tolerant class の遅延・鮮度に正直に現れる — それが答え)

**app 層 coalescing の扱い(事前確定)**: loss-tolerant class について、**未 submit の
古い update を新しい値で置換・破棄する送信側 coalescing は許可**する(実運用の TCP
サーバが普通に行う設計であり、idiomatic 原則に含まれる)。実施の有無と方式は開示
metadata に出す。must-deliver class の欠落・並べ替えは全 transport で不可。
coalesce された slot は sched_ts 会計上 miss になるが、一次指標を staleness にする
(下記)ことで「状態は新鮮なまま」が正しく評価される。

v1 で `unsupported_unreliable` として排除されていた udt4 も、この仕様では全 profile に
参加できる。計測は per-class 分離ヒストグラム・forward/return 分解(v1 S5)を継承。

**class 別の一次指標**: delivery_ratio だけだと TCP 系は「全部届く(ただし遅れて)」
ため 1.0 になり、RUDP の drop と TCP の HoL 遅延が別指標に散らばって比較不能になる。

- **loss-tolerant → staleness p99**: 受信側で「最新受信 update の経過 age
  (now − sched_ts)」を固定周期でサンプルした percentile。RUDP の drop、TCP の
  HoL 遅延、app 層 coalescing のすべてが同じ物差しに載り、latest-value 意味論を
  正しく評価する
- **must-deliver → deadline hit rate**: profile が定義する締切 D(例: 150ms)以内に
  届いた slot の割合(sched_ts 起点)。従来の delivery_ratio は「最終的に届いたか」の
  診断用に併記

「TCP でいいのはどの条件までか」は staleness の regime 依存カーブとして読める。

**ネットワーク条件は DoE の要因**: TCP 系 vs RUDP の優劣は netem 条件で反転するため、
条件は固定背景ではなくスイープ要因とする。regime は clean(0 loss / ~0ms)、
wired(0.1% / 片道 10ms)、mobile(1-3% / 片道 40ms + jitter + **帯域上限**
(netem rate + queue limit で制約付き last-mile を近似))の3点を基本とする。
AQM/bufferbloat の忠実な再現は非目標(本ワークロードの per-conn 帯域は細く、
支配要因は loss/delay。ただし mobile の帯域上限がないと HoL/CC の結論が
無限帯域前提になるため上限だけは入れる)。

**lifecycle バリアプロトコル(two-phase)**: 各プロセスが自分の起動時刻から窓を
計算する方式を全廃する。orchestrator が control channel(TCP, line-delimited JSON。
**netem を通らない out-of-band 経路** — 被測定経路の delay/loss の影響を受けない)で
全プロセスと接続し:

```
process → orchestrator: HELLO {role, lib, pid, proc_index}
process → orchestrator: READY                          (接続確立・warmup 準備完了)
orchestrator → all:     SCHEDULE {start_at, stop_at, drain_until}   (全員 READY 後)
process → orchestrator: DONE {stats summary}
```

計測窓は CLOCK_MONOTONIC の**絶対時刻 start_at/stop_at** で定義する(同一ホストなので
全プロセスが同一クロックを読む)。SCHEDULE メッセージの TCP 配送ズレは「受信が
start_at より margin をもって先行する」ことで無害化し、各プロセスは受信時の余裕
(barrier skew)を報告、負の skew は validity gate で即 INVALID。バリア受信自体を
窓開始とみなす方式は配送ズレがそのまま窓ズレになるため採らない。
measurement-window bit は start_at/stop_at 準拠で立てる(warmup 混入除外は v1 S5 を継承)。

**conformance**: reference client が各 server 実装を1回の起動で検査する
(echo/broadcast の distribution 意味論、traffic class マッピング、seq/flags の透過)。
新 transport の参加はこれの green が入場条件。

### servers/<lib>: idiomatic 実装の規律

- upstream の公式 example / ドキュメントの推奨に沿って書く。各実装は upstream 参照
  (どの example に倣ったか)を README 1枚で示す
- event loop・スレッド・tick cadence は lib の流儀(yojimbo は固定 tick、msquic は
  callback、enet は service loop)
- benchkit をリンクして codec / metrics / バリアだけ共通化。C# (litenetlib) は
  P/Invoke または仕様再実装(v1 で独立バイナリ方式は実証済み)
- 開示 metadata(cc_algo, thread_model, tuning 項目, backpressure ポリシー,
  max_payload, 暗号)を JSON で自己申告 → 結果表に層別列として出す

### orchestrator

- **ネットワーク**: netns ペア + veth。netem は各側 egress に方向別で指定
  (lo の往復二重適用を廃止。loss/delay は片道の値をそのまま書ける)。
  適用後に実効 qdisc を読み戻して run metadata に記録
- **隔離**: systemd slice / AllowedCPUs / LimitNOFILE は v1 の知見を流用。
  実効値を run metadata に記録
- **資源計測の統一**: orchestrator が全登録 pid の /proc/<pid>/stat|status を
  100ms 周期で外部サンプリング(getrusage vs .NET API の系統差を廃止)。
  CPU 窓はバリアで閉じる(v1 §5.1 の tail 希釈を構造的に解消)
- **sweep**: break 探索は二分探索、publish run のみ full grid。実行順・ブロック化・
  反復は「実験計画(DoE)」節の3原則に従う。resume は
  「結果ファイル内の (run_id, lib) 行の実在」判定(v1 §7.1 の教訓)
- **統計**: 単発 break point 値をやめ、conns × delivery/CPU/RTT の応答曲線 +
  bootstrap CI。capacity@gate は曲線からの導出値。ゲート際の点だけ N を増やす
  適応サンプリング

### client farm(複数プロセス構成)の計測規約

負荷生成は v1 同様マルチプロセスの client farm を前提とする。受信計測の正しさは
次の規約で担保する:

- **窓の一致**: conn は proc に排他分割し、全 proc がバリアで同一計測窓に入る。
  v1 の proc 間 ramp 窓ズレ(dev-notes §1.7-2)は構造的に起きない
- **集計は大域で**: 各 proc は per-class の sent/received カウントと固定 bin
  ヒストグラムだけを bin log に書く。bin 加算は情報損失なしでマージ可能
  (v1 combine_clients の知見を orchestrator に移植、N=1 round-trip 一致テスト継承)。
  broadcast の delivery 分母(Σsent × 総接続数)は **orchestrator が大域計算**し、
  proc 単体では期待受信数を判定しない(v1 §6.1 の outstanding 発散の教訓 —
  benchkit に outstanding ベースの idle 判定のような大域状態依存ロジックを置かない)
- **cross-proc レイテンシ**: echo は同一 proc 往復の RTT。broadcast は送信 proc ≠
  受信 proc になるが、同一ホストの CLOCK_MONOTONIC はプロセス間比較可能なので
  **片道遅延**として有効。レポートは echo RTT と broadcast one-way latency を
  別指標として出す(v1 は同名で混ざっていた)
- **受信健全性の機械判定**: client netns の kernel カウンタ(`Udp: RcvbufErrors/
  InErrors`、TCP 再送)を orchestrator が計測窓前後で delta 取りする。client 側の
  受信溢れ(gns の 8proc 劣化のような farm 起因の delivery 低下)を transport
  非依存に検出でき、netns 分離がここでも効く。benchkit も per-proc の
  recv drain 統計を出す
- **farm sizing は「移植時に1回決めて凍結」**: 測定対象は server capacity であり、
  farm は計測器 — 必要なのは公平性ではなく十分性。farm には 4 物理コア(8 論理)を
  排他で与え(server の 4:1 過剰供給)、その予算内の構成(proc 数 × スレッド)は
  transport ごとに移植時に決める。制約は「farm 内総スレッド数 ≤ farm 論理 CPU 数」。
  十分性は proc 数に対して**単調ではない**(v1: gns media_relay c50 が 4proc=0.97 /
  8proc=0.52 — 盛る方向が安全ではない)ため、proc 数 A/B で server 指標の不変を
  確認してから凍結する。全 transport 一律の折衷値(v1 の echo=8/broadcast=4)には
  戻さない。ランタイムの farm 適応・増強はしない(物理予算は変わらず、複雑さに
  見合わない)
- **farm 律速は break ではなく打ち切り(censored)**: 十分性ゲート(attempted_ratio、
  pacing 遅延、netns 受信 drop delta)は検出器であり、発火した点は
  「farm-limited、capacity > 最後に測れた conns」と表記して server の break として
  表に載せない(v1 で `client_tick` invalid が break point に混ざり server を
  過小評価した誤読の根絶)。4:1 過剰供給下で発火は sweep 最上端の稀事象のはずで、
  頻発する transport は farm 構成を見直す

## 実験計画(DoE)と実行環境(rig)

計測の統計設計は実験計画法の標準語彙で定義する:

| DoE 用語 | 本ベンチでの対応 |
|---|---|
| treatment(処理) | transport(× 設定ポリシー) |
| factor(要因) | profile / conns / netem 条件 / payload |
| block(ブロック) | 1 rig セッション(= 1 クラウドインスタンス、またはホームマシンの1連続実行) |
| replicate(反復) | 独立したブロック。**同一ホスト状態内の反復は replicate と数えない** |
| response(応答) | conns × delivery/CPU/latency の応答曲線 |

**3原則:**

1. **Blocking**: 比較される数字は必ず同一ブロック内で取る。クラウド並列化は
   「1 インスタンス = 1 ブロック = 全 transport の完全な1周」の単位でのみ行う。
   transport ごとにインスタンスを分けるとホスト個体差がランキングに直接混入する(禁止)
2. **Randomization**: ブロック内の transport × run の実行順はシード付き乱数で
   ランダム化し、seed を metadata に記録(v1 の固定順による熱ドリフトの系統化を根絶、
   かつ再現可能)
3. **Replication**: N はブロック数で数える。v1 の「N=3」は同一ホスト状態の3回で
   独立性が弱かった。独立ブロック(例: 別インスタンス5台)の方が統計的に強い

**分析**: transport 間比較は within-block の paired 差分・順位で行い、ブロック間分散は
環境頑健性としてそのまま CI に含める。開発中は少点数・少ブロックのスクリーニング、
publish は複数ブロックの確認実験、と2段階に分ける。

**組み合わせの抑制(full factorial は回さない)**: 要因空間(profile × conns ×
regime)は全格子を埋めず、答えるべき2つの質問が住むスライスだけを測る:

1. **capacity スライス**(Q: server はどこまで張れるか): conns スイープ(二分探索)を
   **wired + mobile の2 regime** で全 profile に対して行う(v1 で loss 条件が break を
   1桁動かした実績 — msquic の CC、udt4 — があるため wired 単独は仮定が強すぎる。
   clean は wired と実質同等なので省く)
2. **boundary スライス**(Q: どの条件まで TCP でいいか): regime スイープを
   **代表 conns 固定**で、latest-value 系 profile(media_relay / game_server)に
   対して行う。staleness p99 が一次指標
3. **交互作用プローブ**: スライス仮定の検証として、capacity の break 付近 × 残 regime
   の数セルを publish 時のみ実測。スライスの結論と矛盾したらその近傍だけ局所的に
   格子を広げる(逐次実験)

これで 1 ブロックあたりの run 数は full factorial の ~1/5(概算 ~300 run)になり、
AWS ブロック並列なら publish の wall-clock は 1 ブロック分で済む。

**rig 抽象**: rig = 実行環境の記述(コア割当・隔離方式・環境 metadata)であり、
orchestrator にとっては設定でしかない。

| rig | 用途 | 備考 |
|---|---|---|
| home(5750GE) | dev / smoke / v2.0 | ARK/Minecraft 同居。ソフト隔離は v1 の知見を流用 |
| aws-metal | publish(ブロック並列) | c7a.metal 系 on-demand。netns/veth はインスタンス内で完結。非 metal を使う場合は /proc/stat の steal delta を validity gate に追加 |

cross-rig 一致(同じ canonical を2つの rig で回してランキングが一致すること)は
校正スイートの頑健性項目とする。v1 では原理的に不可能だった主張。

## validity gate(全 run で機械判定)

人間の違和感検出に頼らない。gate 不合格の run はツールが INVALID を付ける。

- 事前: 残留 qdisc なし / ulimit / 隔離実効値 / netem echo back / 対象 CPU の governor
- 事後: attempted_ratio=1.0(client 律速でない)/ バリア窓の一貫性 /
  server・client の exit 状態 / drain 統計 / client netns の受信 drop カウンタ delta /
  診断列の閾値(recv_drained_p99 大 → RTT 絶対値に注記フラグ)

## 校正スイート(canonical の前提条件、CI 常設)

計測器を「答えの分かっている系」で校正してから未知の系を測る:

1. **会計零点**(null): 通信なしで delivery=1.0 厳密・staleness ≈ サンプル周期。
   benchkit の全集計経路の零点(sudo 不要、CI 常設)
2. **既知故障注入**(fault_inject = null + 決定論的注入): dedup 後 delivery=1.0・
   重複計数=注入率を検査(CI 常設)
3. **netem 実効値検証**(標準ツール): ping / iperf3 をベンチ相当の pps で netns 内に
   流し、loss/delay/RTT の実測が設定値と一致(pre-run gate として毎回実施)
4. **必達会計**(TCP 系参加者): loss 下で must-deliver class の delivery=1.0
   (kernel 保証が真値。tail 十分長の条件で)
5. **不変性**: duration 10s vs 30s で delivery 一致 / client proc 数を変えて server 指標不変 /
   実行順入替えで順位不変
6. **過去の落とし穴の回帰テスト化**: dev-notes §1 の機械化可能項目
   (netem 残留、combined histogram 禁止、fd 上限、…)
7. **外部アンカー**: 公式 perf ツールが存在する lib(msquic secnetperf、quiche/lsquic 同梱
   ツール、gns example)を同条件で走らせ、**adapter 効率比 = 自実装 / 公式ツール** を算出。
   比が閾値(暫定 0.5)を下回る実装は canonical 参加前に要調査
8. **定常監視**: 1conn・netem なしの pps 天井(floor=iperf3 比)、strace -c による
   syscalls/msg
9. **cross-rig 一致**: 同一 canonical を home / aws-metal の2 rig で実行し、
   transport の順位が一致すること(環境頑健性)

## break 原因ラベル(必須)

canonical の各 break point には根拠となる診断値を紐づけた原因ラベルを必須化:

`server_cpu_saturation` / `library_protocol_limit` / `impl_limited`(perf プロファイルで
servers/<lib> のグルー側フレームが cycles の閾値超)/ `unsupported` / `unknown`

break 点では自動で perf record を取り、シンボルを glue / library / benchkit / kernel に
分類して比率を出す。`unknown` の残数がそのまま残作業バックログ。

## 収束条件(Definition of Done)

1. 校正スイートが同一 commit で green
2. 全 break point から `unknown` ラベルが消えている
3. 連続 2 回の canonical が全指標 IQR 内で一致

## v1 からの流用

| 流用するもの | 形 |
|---|---|
| workload 4 profile(media_relay / game_server / reliable_echo / echo) | そのまま(netem 値は片道指定に読み替え) |
| per-channel histogram / forward・return 分解 / 診断列の設計 | benchspec/benchkit に仕様として吸収 |
| netem / systemd 隔離スクリプトと運用知見 | netns 対応に改修して流用 |
| third_party submodule 群 | そのまま |
| publish フロー(dated dir + current.md) | orchestrator に移植 |
| dev-notes | 校正スイートの回帰テスト仕様書として参照し続ける |
| benchctl (Go) のコード | orchestrator の土台として部分流用 |

v1 の published 結果は epoch v1 としてアーカイブし、v2 とは比較しない。

## 初期スコープ v2.0(コンパクト版)

最初から全幅を作らない。**構造要素(バリア同期・netns/veth・外部 /proc サンプラ・
benchkit)は削らず**、横幅を削る:

| 軸 | v2.0 | 後続 |
|---|---|---|
| transport | enet + msquic + magiconion | 必須セット残り 3(gns, litenetlib, websocket) |
| profile | echo(mixed)+ 縮小 broadcast(media 型、少 conns)の2本 | 残り profile |
| ネット条件 | wired + mobile の2 regime | 3 regime(capacity/boundary スライス方式) |
| conn sweep | 1 / 50 / 500 程度の3点 | full sweep |
| 統計 | N=3 中央値のみ | 曲線 + bootstrap CI、適応サンプリング |
| 校正 | 会計零点(null)/ 故障注入 / netem 実効値(ping/iperf3)/ 必達会計(TCP 系)/ duration 不変性 の5本 | 回帰テスト群、pps 天井、syscalls/msg |
| validity gate | qdisc echo back / attempted_ratio / exit 状態 | 全 gate |
| 外部アンカー | msquic secnetperf を手動1回 | 自動化・他 lib |
| conformance / perf 分類 | 手動確認 | 自動化 |

選定理由: enet(単一スレッド service loop の代表)、msquic(内部スレッド + callback の
代表で v1 で最も事故った lib)、magiconion(TCP 系・.NET・traffic class マッピングの
代表で、本命の比較対象)。設計を壊しうる3方向の極端を最初に踏むことで
benchspec/benchkit の設計ミスを早期に炙り出す。疎通・会計デバッグは null(通信なし)が
担い、netns/veth/netem の配管は ping/iperf3 の pre-run gate が transport 到着前に検証する。

縮小 broadcast を v2.0 に含める理由: 契約の難所(broadcast の分母会計、cross-proc
one-way、staleness 指標、TCP 系の coalescing)は echo では一切通らない。難所を
未検証のまま凍結すると量産フェーズで仕様破壊が起きる(v1 の教訓)。

**v2.0 の完了条件**: 校正5本 green + enet/msquic の echo 結果が v1 published と
矛盾しない(または差分が説明可能)+ magiconion が外部の常識値(gRPC ベンチ等)と
桁で矛盾しない + 縮小 broadcast で staleness / coalescing / 分母会計の経路が検証済み。
ここで benchspec/benchkit を凍結し、以降は移植の量産フェーズ。

## 後続フェーズ

1. 必須セット残りの移植(gns → litenetlib → websocket)。追加候補プール
   (kcp, slikenet, udt4, yojimbo, quiche, lsquic)は必要になったときに入場経路を通す
2. 残り 3 profile + full sweep + 校正/gate/アンカーの拡充
   - aws-metal rig 追加(AMI 化・ブロック並列 dispatch・結果回収)。publish は
     以後ブロック並列の確認実験で行う
3. conformance 全 green → v1 harness/adapters/自作実装 削除
4. 曲線 + CI レポート実装、canonical v2 初回 publish

## 未確定 / 要ユーザー判断

- websocket の実装選定(uWebSockets / ASP.NET Core / その他)
- magiconion の構成詳細(serializer、Kestrel 設定、HTTP/2 のみか HTTP/3 も見るか)
- litenetlib / magiconion の benchkit: P/Invoke か C# 再実装か
- 外部アンカー(校正 §7)をどの lib まで整備するか
- `library-default` 併走の範囲(全 profile か代表点のみか)
