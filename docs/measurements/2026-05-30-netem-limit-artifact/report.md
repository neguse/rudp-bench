# conns スケール律速の切り分け: netem queue 上限アーティファクト + 二段ボトルネック

**測定日:** 2026-05-30
**目的:** `2026-05-29-scale-sweep` で観測された「conns を上げると delivery が崩れる」現象について、
律速が **client か server か** を切り分ける。relief test（片側の資源を増やして指標が動くか）で実測する。

**結論（先出し）:**
1. 既存 scale-sweep の中 conns 域（〜600）の崩壊は **library 性能ではなく `netem` の queue 上限 `limit 1000` の溢れ**（計測アーティファクト）。
2. netem を解放すると、最高負荷（1000 conns）でのみ **本物の server CPU 律速**が現れる。
3. **client は「attempted_ratio=1.0 を満たす資源を与えれば」非律速。** 過少資源だと client が生成律速になりうる（gns 2論理コアで attempted_ratio=0.68）。十分資源下では gns 4→6論理コアで delivery 不変（=server 律速）。relief test は attempted_ratio=1.0 を前提に読むこと。
4. gns が「スケールする」真因は2つ: (a) 輻輳制御/pacing が小キュー溢れを避ける、(b) server がマルチスレッドで複数コアを使う。
5. ⇒ **`scripts/netem.sh` / `set_loss.sh` を既定 `limit=100000` に修正済み。`results/scale_mix_*` は limit 1000 で取得＝汚染、再取得が必要。**

## 根本原因: netem の既定 queue limit = 1000 packets

`netem.sh apply` は `tc qdisc add dev lo root netem $args` で `limit` を指定しておらず、netem 既定の
**1000 packets** が効いていた。lo egress は送受双方を運ぶので、offered ≈ `conns × (rate_r+rate_u) × 2` pkt/s、
delay ~30ms → キュー常駐パケット数 ≈ `offered × 0.03`。これが 1000 を超えると netem が**問答無用で drop** する。
これは意図した delay+loss エミュレーションの一部ではなく、**queue サイズのアーティファクト**。

`tc -s qdisc show dev lo` は累計 14.4M 送信中 **9.4M drop（65%）** を示していた。

## セットアップ

- ホスト: Ryzen 7 PRO 5750GE（8 physical / 16 logical, SMT 2/core）。ARK・Minecraft 同居機。
- CPU 隔離: 実験のため `systemctl set-property --runtime {system,user,init}.slice AllowedCPUs=0-3,8-11`
  でベンチ用に物理コア 4-7 を確保（ゲームは 0-3 へ退避）。`run_phase1_quick.sh --isolate=systemd` で
  server/client を bench-*.slice に分離。**終了後 0-5,8-13 に復元**。
- **idle policy: `adaptive`**。`spin` は無負荷でも常時ビジースピンするので `cpu_pct=100%` が飽和判定に使えない。
  CPU を律速根拠にするなら adaptive 必須（本調査で最初に踏んだ罠）。
- netem: `apply 25 5 1`（片道 25ms, jitter 5ms, loss 1%）。`limit` を 1000 / 100000 で振って比較。
- 共通: enet / gns、rate-r=50 + rate-u=50、size=64B、duration=20s、warmup=2s、mode=echo。
- server: 物理コア7（CPU 7,15）固定。client: multi-proc farm（conns を proc 間で均等分割、`combine_clients.py` で集約）。

## 実験と結果

### 実験A — client を緩める（enet, conns=600, limit=1000）

負荷固定で client の物理コアを 1→2→3（CPU 200→600%）に増やす。

| client コア | client CPU | server CPU(spin) | delivery(中央値) | attempted_ratio |
|---|---|---|---|---|
| 1 phys | 200% | 100%* | 0.478 | 1.0000 |
| 2 phys | 400% | 100%* | 0.460 | 1.0000 |
| 3 phys | 600% | 100%* | 0.452 | 1.0000 |

client CPU を3倍にしても delivery は不変（微減）。attempted_ratio は常に 1.0（client はオファー負荷を出し切る）。
→ client は send 側も recv 側もボトルネックでない。
*注: idle=spin のため server cpu=100% は飽和ではなくスピン。また limit=1000 では netem が壁なので「不変」は当然で、
**真の server 律速 regime での client 不変性は実験Eで別途確認**（下記）。

### 実験B — server を緩める（enet, conns=600, limit=1000）

client 固定で server を 1台 → 2台（別物理コア・別ポート、conns を半々）。

| 構成 | delivery(中央値) |
|---|---|
| 1 server | 0.442 |
| 2 server | 0.434 |

server を倍にしても不変。→ limit=1000 では server 台数も効かない（netem キューが壁だから）。

### 実験C2 — conns スイープ（enet, limit=1000, idle=adaptive, 実CPU）

| conns | キュー必要 inflight | limit | delivery | server CPU(実) |
|---|---|---|---|---|
| 100 | 600 | 1000 | 0.990 | 43% |
| 150 | 900 | 1000 | 0.991 | 52% |
| 200 | 1200 | 1000 | 0.911 | 60% |
| 300 | 1800 | 1000 | 0.664 | 65% |
| 600 | 3600 | 1000 | 0.434 | 71% |

delivery は inflight < 1000 で ~0.99、**1000 を超えた瞬間（150→200 の間）に崩れ始める**。BDP=limit クロスオーバーと完全一致。
崩壊点でも server CPU は飽和していない（最悪71%）。→ 中 conns の壁は CPU ではなく netem キュー。

### 実験D — netem limit 1000 vs 100000（enet/gns, idle=adaptive, client 3物理コア固定）

| lib | conns | delivery @limit1000 | delivery @limit100000 | server CPU(実) 1000→100k |
|---|---|---|---|---|
| enet | 600 | 0.45 | **0.99** ✅回復 | 77→94% |
| enet | 1000 | 0.32 | 0.21 | 81→**97%（1コア飽和）** |
| gns | 600 | 0.76 | **0.99** ✅回復 | 165→176% |
| gns | 1000 | 0.42 | **0.55** | 173→**184%（≈2コア）** |

- **600 conns: netem キューが壁だった** → limit 解放で両者 0.99 に回復（確定）。limit=1000 で gns(0.76)>enet(0.45) なのは gns の CC が小キュー溢れを避けるから。
- **1000 conns: 本物の server CPU 律速** → enet は単一スレッドで1コア飽和→0.21、gns はマルチスレッドで ~2コア使い→0.55。ここで gns のスケール優位（server 並列）が効く。
- enet 1000 が limit 上げで 0.32→0.21 と悪化するのは、小キューが offered を絞って server を守っていた逆説（reliable 輻輳崩壊の緩和）。

### 実験E — client 論理コア 2/4/6 で gns@1000(limit=100000) が不変か（真の server 律速 regime での client 切り分け）

server を物理コア7に固定し、client の論理コアだけ 2→4→6 と振る（conns=1000, limit=100000, idle=adaptive, N=3）。

| lib | client論理コア | delivery(中央値) | server CPU(実) | client CPU | attempted_ratio |
|---|---|---|---|---|---|
| gns | 2 (1phys) | 0.86 | 178% | 200% | **0.68** |
| gns | 4 (2phys) | 0.559 | 184% | 399% | 1.0000 |
| gns | 6 (3phys) | 0.558 | 184% | 596% | 1.0000 |
| enet | 2 (1phys) | 0.59 | 98% | 197% | 1.0000 |
| enet | 4 (2phys) | 0.37 | 98% | 386% | 1.0000 |
| enet | 6 (3phys) | 0.19 | 97% | 572% | 1.0000 |

**重要な発見（当初の「client は常に非律速」は誤り）:**
- **gns は 2論理コアだと client が生成律速** — attempted_ratio=0.68、つまり 1000conn 分のオファー負荷を出し切れていない（gns client は暗号化+内部スレッドで重い）。delivery 0.86 が高いのは offered が少なく server が楽なだけで、良い結果ではない。**過少資源の client は律速になりうる。**
- **client を 4論理コアにすると attempted_ratio=1.0 に回復**し delivery は 0.56 に落ちて server 律速へ。**4論理 → 6論理 で delivery 0.559→0.558、server CPU 184% と完全に不変** → client が十分な資源を持つ条件下で server 律速が確定。
- ⇒ **正しい結論: client が非律速と言えるのは「attempted_ratio=1.0 を満たすだけの資源を与えた上で」のみ。relief test は attempted_ratio=1.0 を前提条件として確認しないと誤読する。** gns でこれを満たすのは 4論理コア(2物理)以上。
- enet は 2論理コアでも attempted_ratio=1.0（軽い）。server は全レベルで 97-98%（単一コア飽和）。ただし delivery は client コアを増やすほど 0.59→0.19 と悪化 — client が throughput 律速だからではなく、**client proc/コアが増えるほど送信がバースト化し、飽和した単一スレッド server がより多く drop する（reliable 輻輳崩壊も悪化）**ため。server 律速だが offered のバースト形状で delivery が動く。単純な「不変」線にはならない点に注意。

## 過去の誤り（再発防止）

1. **SMT 誤読**: pin `7,15` / `6,14` を「2物理コア」と読んだが、5750GE では各々 **1物理コアの SMT 2スレッド**。
2. **spin の 100% を飽和と誤読**: `--idle=spin` は常時ビジースピン → `cpu_pct=100%` は飽和の証拠にならない。
   これを根拠に一度「server 律速」と誤結論した。CPU を語るときは必ず `--idle=adaptive`。

## 対処（コミット対象）

- `scripts/netem.sh`: 既定 `limit=100000`（5th 引数で上書き可）に修正＋理由コメント。
- `scripts/set_loss.sh`: 同上（3rd 引数）。
- lo の生 netem は `limit 100000 delay 25ms 5ms loss 1%` で張ったまま（1000 に戻さない）。
- **`harness/runner.cc`: validity ゲートから `tick_gap_p99 <= budget` を除去**。functional correctness
  （attempted_ratio≥0.99 ∧ accepted_ratio≥0.99）だけで判定し、tick_gap は診断専用に。コメントが元々
  「tick_gap は pass/fail ゲートにすべきでない」と書いていたのにコードが反していた不整合の修正。
  これがないと、負荷を出し切り全受理された高 conns gns/msquic 実行がループジッタだけで valid=0 にされ測定不能だった。
- **`scripts/bench_isolate.sh`: client を 1物理コア(6,14)→2物理コア(5,6,13,14)**。load generator が律速に
  ならないよう過剰供給する。1物理コアでは gns@1000 が attempted_ratio=0.68（負荷生成律速）で valid=0 になり
  測定不能だった。server は phys 7（1コア）のまま=測定変数。
- **`results/scale_mix_*` および `docs/measurements/2026-05-29-scale-sweep` の結論は limit 1000 汚染。再取得が必要。**

### 対処後の検証（実験F: gns@1000, client 2物理コア, limit=100000, idle=adaptive, N=2）

| run | valid | delivery | attempted_ratio | tick_gap_p99 |
|---|---|---|---|---|
| gns_fix_c1000_r1 | **1 (ok)** | 0.570 | 1.0000 | 32ms |
| gns_fix_c1000_r2 | **1 (ok)** | 0.551 | 1.0000 | 28ms |

修正前は同条件が全て valid=0（client_tick）で測定不能。修正後は client が負荷を出し切り（attempted_ratio=1.0）、
tick_gap が大きくても無効化されず、server 律速の数値 delivery≈0.55 が valid=1 として取得できる。

## 生データ

各ランの canonical / diagnostics / scenarios / raw は `results/exp_{a,b,c2,d,d2,e}/`（`results/` は .gitignore のため
未追跡）。要点の中央値は版管理対象として [`data/summary.csv`](data/summary.csv) に焼き込んである。
