# 2026-11-07 劣悪ベンチ(既定から変えた設定)

既定の設定で不合格の理由がはっきりしたライブラリについて、ゲームで実際にそう使う設定に変えたものを、[劣悪ベンチ](../../README.md#劣悪ベンチ)の全条件で 1 回ずつ測った記録。
既定の設定の結果は [2026-11-07(ライブラリの既定の設定)](2026-11-07-badnet-local.md) と [2026-11-07(追加の 5 ライブラリ)](2026-11-07-badnet-added.md) にある。

## 測った設定

| treatment | 既定からの変更 | 理由 |
|---|---|---|
| `gns-timeout30` | `k_ESteamNetworkingConfig_TimeoutConnected` を 10 秒から 30 秒に | トンネルやエレベーター、Wi-Fi と携帯回線の切り替えで回線が十数秒切れても、接続を保ちたいゲームで使う |
| `slikenet-timeout30` | `SetTimeoutTime` を 10 秒から 30 秒に | 同上 |
| `yojimbo-timeout30` | `ClientServerConfig::timeout` を 10 秒から 30 秒に | 同上 |
| `quiche-drop-old-dgram` | LT の datagram を送信待ちに足す前に、まだ送っていない datagram を `quiche_conn_dgram_purge_outgoing` で捨てる | LT は最新の値が届けばよいので、送れずに残った古い値は捨ててよい(`msquic-cancel-on-blocked` と同じ) |

## 実行条件

| 項目 | 値 |
|---|---|
| commit | `6661a66`(SUT)。orchestrator は、ライブラリが断った送信を判定に含めた `e60913c` |
| ライブラリ | 既定の設定の記録と同じ(GNS `a24b43d`、SLikeNet v.0.1.3、yojimbo v1.9.1、quiche 0.30.0)、Linux 6.18 |
| 実行 | `scripts/badnet/gen.py` → `sudo scripts/badnet/run.sh` → `scripts/badnet/analyze.py` |
| CPU | bench.slice(CPU 3-7,11-15) |
| 検証 | 判定に使った run はすべて `verdict=VALID` |

## 結果

| 条件 | gns-timeout30 | slikenet-timeout30 | yojimbo-timeout30 | quiche-drop-old-dgram |
|---|---|---|---|---|
| base | PASS | PASS | PASS | PASS |
| loss1 | PASS | PASS | PASS | PASS |
| loss2 | PASS | PASS | PASS | PASS |
| loss5 | PASS | PASS | PASS | PASS |
| loss10 | PASS | PASS | PASS | PASS |
| loss20 | PASS | PASS | PASS | FAIL C3 |
| jitter30-loss2 | PASS | FAIL C3 | PASS | PASS |
| burst5x10 | PASS | PASS | PASS | PASS |
| congest1m | FAIL C3 | FAIL C3 | FAIL C3 | FAIL C3 |
| outage5 | PASS | PASS | PASS | FAIL C4 |
| outage15 | PASS | PASS | PASS | FAIL C4 |
| loss20-then-clean | PASS | PASS | PASS | PASS |

- 切断までの時間を 30 秒にした 3 つは、outage15 で切れなくなり、断が明けてから 0.2〜0.7 秒で戻った(MD 遅延の最大 15204〜15729 ms)。再送の待ち時間が倍々に伸びないので、断の後すぐに戻る。他の条件の合否は既定の設定と同じ。
- `quiche-drop-old-dgram` は、congest1m の下りの LT staleness p99 が 28312 ms から 590 ms に縮んだが、回線の待ち行列(約 1 秒)の分が残り不合格。ロスが多いと、送れずに捨てる LT の分だけ到達率が下がり、loss20 で不合格になった。回線断の戻りの遅れ(C4)は既定と同じ。

### 主要な数値(上り / 下り、ms)

| 条件 | 指標 | gns-timeout30 | slikenet-timeout30 | yojimbo-timeout30 | quiche-drop-old-dgram | quiche(既定) |
|---|---|---|---|---|---|---|
| outage15 | MD 遅延 max | 15204 / 15204 | 15729 / 15204 | 15204 / 15204 | 16777 / 16777 | 16777 / 16777 |
| loss20 | 下り LT 到達率 | 0.789 | 0.802 | 0.724 | 0.679 | 0.802 |
| loss20 | 下り LT staleness p99 | 197 | 205 | 213 | 311 | 786 |
| congest1m | 下り LT staleness p99 | 1376 | 655 | 819 | 590 | 28312 |
| congest1m | 下り MD 遅延 p99 | 2228 | 2359 | 1376 | 1180 | 1507 |

### 境目の再測定

基準値の ±10% 以内で合否が変わりうるセルを測り直した。合否が割れたセルはなかった。

| セル | 1 回目 | 2 回目以降 | 扱い |
|---|---|---|---|
| loss20 yojimbo-timeout30 | PASS(下り LT 到達率 0.724、基準 0.720) | PASS(0.726) | PASS |
| loss20 gns-timeout30 | PASS(0.789) | PASS(0.790) | PASS |
| loss20 quiche-drop-old-dgram | FAIL C3(0.679) | FAIL(0.670) | FAIL |
| outage15 gns-timeout30 / slikenet-timeout30 / yojimbo-timeout30 | PASS(最大 15204 / 15729 / 15204 ms、基準 16000 ms) | PASS(15204 / 15729 / 15204 ms) | PASS |
| outage15 quiche-drop-old-dgram | FAIL C4(16777 ms) | FAIL(16777 ms) | FAIL |
| loss5 / loss10 / loss20-then-clean の yojimbo-timeout30 と quiche-drop-old-dgram、jitter30-loss2 の yojimbo-timeout30 | PASS | PASS | PASS |

yojimbo-timeout30 の loss20 は 2 回とも合格したが、既定の yojimbo は 3 回とも 0.710〜0.719 で不合格だった。切断までの時間は loss20 の振る舞いに関係しない。どちらも、1024 B を超えるパケットが 2 つの断片に分かれる仕組みで、下りの LT 到達率の期待値が (1 − 0.2)(1 − 0.1) = 0.720 と基準値に一致する([追加の 5 ライブラリの記録の yojimbo の節](2026-11-07-badnet-added.md#yojimbo))。合否の差は偶然による。

結果は `results-v2/badnet-20261107-{variants,near,coopfix}`(リポジトリには入れていない)。

## 不合格の仕組み

timeout30 の 3 つの不合格(congest1m、slikenet の jitter30-loss2)は、既定の設定と同じ仕組みによる(各ライブラリの節を参照)。切断までの時間はこれらに関係しない。

### quiche-drop-old-dgram

出典は `third_party/quiche/quiche/src/`。

| 不合格 | 仕組み | 出典 |
|---|---|---|
| loss20 C3(下り LT 到達率 0.679 / 0.670) | `quiche_conn_dgram_purge_outgoing` は、送信待ちの datagram のうち条件に合うものを除く。ロス 20% では CUBIC の cwnd が小さくなり、送れないまま次の LT が来る。そのたびに古い LT を捨てるので、既定の quiche(送信待ちに溜めて遅れて届ける、到達率 0.802、staleness p99 786 ms)より到達率が下がり、staleness は 311 ms に縮む | `dgram.rs:95-101`, `recovery/congestion/cubic.rs:375-379` |
| congest1m C3(下り LT staleness p99 590 ms、MD p99 1180 ms) | datagram の送信待ちは伸びなくなったが、CUBIC は待ち行列が溢れて落ちるまで送る量を減らさないので、回線の待ち行列(約 1 秒)は埋まったままになる | `recovery/congestion/cubic.rs:355-399` |
| outage5 / outage15 C4 | 既定の quiche と同じ。probe timeout が倍々に伸びる | `recovery/congestion/recovery.rs:458`, `:486-488` |
