# battle — 各ライブラリ最強実装の対戦表と運用

目的: tune-to-plateau 済みの各 transport 実装を wired-v3 で測り、
raw_udp 天井比の対戦表を作る。運用ルールはルート CLAUDE.md の
「バトル(wired 測定)の運用ルール」節が正。本ドキュメントは
対戦表・手順・読み方の注意・学び・TODO のハブ。

## 対戦表(running)

天井 = [ceiling 基準線](measurements/2026-07-10-ceiling-wired-v3.md)。

| transport | r20p128 | r20p1000 | r60p200 | セッション記録 |
|---|---|---|---|---|
| raw_udp(天井) | 76 | 75 | 46 | [ceiling](measurements/2026-07-10-ceiling-wired-v3.md) |
| enet | **191**(251%) | **≥75**(天井律速) | **96**(209%) | [battle-enet](measurements/2026-07-10-battle-enet.md) |
| gns | **88**(116%) | **65**(87%) | **49**(107%) | [battle-gns](measurements/2026-07-10-battle-gns.md) |
| msquic | **158**(208%) | **55**(73%) | **107**(233%) | [battle-msquic](measurements/2026-07-10-battle-msquic.md) |
| litenetlib | **≥128**(farm censored) | **72**(96%) | **≥64**(farm censored) | [battle-litenetlib](measurements/2026-07-10-battle-litenetlib.md) |
| magiconion | — | — | — | 未 |
| websocket | — | — | — | 未 |

## セッション手順

1. **契約**: 開始前に「予算 N 分 / 打ち切り M 分 / アウトプット」を合意
2. **アンカー**: raw_udp r20p128 c68(0.9×天井)を 1 点 probe。
   前セッションと乖離したら停止して調査
3. **本測定**: `sudo scripts/run-sweep.sh <config>`。
   config は `sweep-wired-v3.json` の該当 transport ブロック +
   ceiling と同じ workloads/conns/netem
4. **perf**: 破断点直下で server に attach(診断専用 run — perf は SUT を
   摂動させるので capacity 主張から除外)。adapter 自前コードの % で
   plateau を判定。**診断 config は duration を 30s に延長する**(steady
   早期成立だと run が ~20s で終わり attach が間に合わない — litenetlib
   セッションで実証)
5. **記録**: `docs/measurements/<date>-battle-<lib>.md` + 本表更新 + commit

## 読み方の注意(学び)

- **raw_udp 天井は「coalesce しない transport の天井」**。ENet のような
  MTU merge を持つ transport は小 payload で天井を 2 倍以上超えうる
  (syscall 数が 1/6〜1/9 になるため)。天井比 >100% は異常ではない。
  逆に **100% 張り付きは「ハーネス律速」の証拠**で、そのセルは ≥N と読む
- **client_stall 帰属**(farm CPU idle なのに sched p99 超過)が break に
  併記されたセルは、farm 側要因の可能性をゲートが開示している状態。
  capacity は下限気味に読む
- **境界セルは ±1 点 flap する**(ledger #18 と同族)。capacity±1 は解像度の内
- **perf attach は観測効果がある**: 診断 run は capacity 判定から除外する
- **破断の形も特性**: enet/gns は staleness の緩やかな破断、msquic は
  delivery 0 への崖(QUIC datagram の CC キュー意味論)。capacity の 1 行では
  見えない差なので、記録には break 原因を必ず残す
- sweep は同一 output_dir の結果を **cache して再実行しない** — 再測は
  dir を変えるか消す

## 運用の学び(踏んだ罠)

- **rig の CPU 隔離下では素の shell(sudo 含む)からベンチを起動できない**
  (user/system slice が OS コアに閉じ込められ taskset が EINVAL)。
  必ず `scripts/run-sweep.sh` / `run-sentinel.sh`(bench.slice transient
  unit)経由。dev-notes §2 の既知の罠
- **C# は Release で測る**(quick 用 config が Debug を指していた事故 →
  全 config を Release に統一済み、commit 29b9f03)。C/C++ の build dir は
  CMakeCache の `CMAKE_BUILD_TYPE=Release` を確認してから
- **loopback の quick run は正しさ確認まで**。farm と server が CPU を
  取り合う構成では c64 級のスケール挙動は判定不能(C# 系で実証)。
  スケールの数字は必ず隔離 rig で
- **drain/service budget は「イベント数」でなく「仕事量」で bound する**:
  broadcast 1 イベントの仕事は O(conns)。固定イベント budget は高 conns で
  制御チャネルを飢えさせる(raw_udp server・enet server で同じ病気を
  踏んだ — ledger #20、commit 2f374b1 / bbe2885)。**新しい adapter を
  書くときは server 側の drain budget を最初から入れること**
- **「proc_index=0」の INVALID は server の可能性がある**(server の hello
  も proc_index=0)。invalid メッセージに role= を追加済みだが、古いログを
  読むときは注意

## TODO キュー(バトル)

1. ~~ceiling 確定~~(済 — 76/75/46)
2. ~~enet~~(済 — 191 / ≥75 / 96)
3. ~~gns~~(済 — 88 / 65 / 49)
4. ~~msquic~~(済 — 158 / 55 / 107)
5. ~~litenetlib~~(済 — ≥128 / 72 / ≥64、2 セル farm censored)
6. **magiconion セッション**(次)
7. websocket セッション
8. **clocksource 修正セッション**(ledger #21、ユーザー判断 GO 済みの
   方針 A: 全ライブラリを同一条件で揃えた後に実施)—
   dmesg で TSC 無効化理由確認 → boot param/BIOS → 再起動 →
   ceiling 再測 → 必要なら全 transport 再測。全天井が一律に上がる見込み
9. **loss1 regime の対戦表**(enet throttle 無効化 #11/#12 の効果検証は
   loss 下でしか見えない。wired-v3 表が揃った後に同型のセッションで)
10. r20p1000 級の「天井律速」セルの真値化: farm 増強(procs/コア)か
    天井そのものの改善(#21 で上がる可能性)後に再測
11. litenetlib の farm censored 2 セル(r20p128 c256 / r60p200 c128)の真値化:
    farm の pump drain スループットが構造限界(rcvbuf 4MB 化でも drops 残存)。
    farm 増強か LNL vendor 化(SocketBufferSize 定数の変更)が必要
12. 対戦表完成後: 総括レポート(v3 report への組み込み or 独立 doc)

計測器・rig の個別 TODO は [ledger](ledger.md) が正(#5 farm rcvbuf、
#6 farm 律速判定、#18 sentinel probe マージン、#21 clocksource など)。
