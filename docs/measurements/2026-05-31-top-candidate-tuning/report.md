# Top-candidate tuning: enet/kcp/gns/litenetlib の改善余地を実装して実測

**測定日:** 2026-05-31
**目的:** 「最強狙える群」(enet, kcp, gns, litenetlib) を並列分析して見つけた改善余地のうち、高確信・adapter 完結・セマンティクス維持のものを実装し、効果を v3(改善前)と同条件で測って定量化する。

## 実装した改善

- **gns**: per-conn の `ReceiveMessagesOnConnection` を **1 PollGroup + `ReceiveMessagesOnPollGroup`** に置換。per-conn 経路は毎 tick 1000回の global tables lock + 各 conn の m_pLock を取り、単一 GNS service thread の decrypt と正面衝突していた(1000崩壊の主因)。
- **kcp**: stock `ikcp_nodelay` が interval<10ms を 10ms に clamp して L9 の 5ms 指定を無効化していた。**submodule を変えず `kcp->interval` を直接代入**して真の 5ms 化。窓 256→512。
- **enet**: host の peerCount を実 conn 数に縮小(`Adapter::hint_connections`)。enet は毎 flush で全 peer スロットを線形走査するため、4095 固定は conn 非依存の固定 CPU 税だった。
- **litenetlib**: 保留(天井は server でなく client の per-conn 2スレッド。`StartInManualMode` がレバーだが polling syscall とのトレードオフで純益不確実、delivery は既に最強)。

## 結果（N=3 中央値、valid run のみ。v3=改善前 / v4=改善後）

| lib | conns | v3 dr | v4 dr | Δdr | v3 cpu% | v4 cpu% | 判定 |
|---|---|---|---|---|---|---|---|
| enet | 200 | 0.990 | 0.989 | -0.001 | 61.6 | **45.3** | CPU 大幅減 |
| enet | 600 | 0.990 | 0.991 | +0.000 | 95.8 | 93.5 | 同等 |
| enet | 1000 | 0.373 | **0.439** | **+0.066** | 99.2 | 97.9 | 小勝(飽和点後退) |
| kcp | 200 | 0.992 | 0.992 | +0.000 | 35.7 | 37.1 | 同等 |
| kcp | 600 | 0.925 | **0.896** | **-0.030** | 90.9 | 91.2 | **悪化** |
| kcp | 1000 | 0.709 | **0.773** | **+0.064** | 88.6 | 89.0 | 小勝 |
| gns | 200 | 0.992 | 0.992 | +0.000 | 138.0 | **91.1** | CPU 大幅減 |
| gns | 600 | 0.993 | 0.992 | -0.001 | 178.7 | 160.7 | 同等(CPU減) |
| gns | 1000 | 0.565 | **0.866** | **+0.301** | 189.3 | 181.4 | **大勝** |
| litenetlib | 200 | 0.994 | 0.993 | -0.001 | 122.3 | 121.7 | 不変(王者) |
| litenetlib | 600 | 0.994 | 0.994 | -0.000 | 172.5 | 171.2 | 不変 |
| litenetlib | 1000 | ✗tick | ✗tick | — | — | — | client 2物理コアで under-provision(既知) |

## 解釈（盛らない）

- **gns = 大勝(本命的中)**: 1000conn **0.565 → 0.866 (+0.30)**。PollGroup でロック競合を潰した効果が予想どおり出た。崩壊が解消し、1000でも 0.87 を保つスケーラに化けた。加えて **CPU が全域で低下**(200conn 138→91%)＝ロック嵐を消した副次効果。**1000conn ランキングを激変させた**:
  - 改善前: litenetlib(0.994) > kcp(0.709) > gns(0.565) > enet(0.373)
  - 改善後: litenetlib(0.994) > **gns(0.866)** > kcp(0.773) > enet(0.439)
  - gns が最下位グループから 2位に。マルチスレ暗号付きで 0.87 は強い。
- **enet = 小勝**: 1000conn +0.066。固定税(全 peer スロット走査)を消した分の頭打ち緩和。delivery への寄与は控えめだが、**CPU が顕著に下がった**(200conn 61.6→45.3%)＝予測どおり「飽和点を後ろにずらす」方向。単スレ 1レーンの天井は残るので 1000 で 0.44 止まり。
- **kcp = 痛み分け**: 1000 +0.064 だが **600 が -0.030 悪化**。5ms 化(flush 倍増)＋窓512 が、最悪点(1000)は救うが中域(600)では retx churn か窓ダイナミクスで逆効果。**clean win ではない**。分析が予測した 0.85+ には届かず(0.773)。単スレ ARQ の構造天井が近い。残るレバー(input 後の即 ACK・poll 位相分散)は未実装。
- **litenetlib = 不変**: 触っていないので当然。1000 は client 2物理コアで invalid のまま(server 限界ではなく測定コスト、既知)。

## 結論

- **delivery を本当に動かせたのは gns(大勝)と enet(小勝)**。kcp は痛み分け、litenetlib は据え置き。
- 「改善余地ないか」の答え: **gns に大きな余地があった(PollGroup)、enet に中程度(peerCount)、kcp は小さく頭打ち、litenetlib は delivery 余地なし(measurement-cost のみ)**。
- 最強候補の地図が更新された: 高負荷の絶対王者は依然 litenetlib だが、**gns が PollGroup で本物の 2位スケーラに浮上**。単スレ勢(kcp/enet)は 1000 で 0.4-0.8 に留まり、1レーン/ARQ の構造天井が見える。

## 次の宿題

- kcp 600 の回帰を切り分け(窓512 を 256 に戻して 5ms だけにするか)＋未実装レバー(即 ACK・poll 位相分散)。
- gns をさらに(RecvBufferMessages 緩和・echo 送信バッチ)で 0.866→0.9+ に届くか。
- litenetlib の `StartInManualMode` を別途実装し、client 2物理コアで 1000 を valid 測定 → server の真の崩壊点(2000+)を初観測。

## 生データ

`results/remeasure_v4/`(改善後) と `results/remeasure_v3/`(改善前)＝.gitignore。中央値は [`data/summary.csv`](data/summary.csv)。集計は `scripts/aggregate_runs.py`。
