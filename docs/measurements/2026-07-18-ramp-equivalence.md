# ramp screening 等価性検証(ADR-0005 enabler #4)— 判定: 置換不可

- campaign: `c20260718-054711`(coordinator smoke)/ `c20260718-060820`(本検証)
- rig: c8g.8xlarge spot ×1(us-west-2、bundle de0d5ae、boot gate PASS、
  doctor ok / calibration passed)
- 条件: ref-room-wan の screening 版(scenario/netem/SLO は ADR-0004 の
  ref-room-wan-v1 と同値、duration 10s。preset は ramp/steady_warmup を pin する
  ため screening は preset 外で条件を複製)。config は
  `scripts/fleet/queues/ramp-eq*/`
- 実行: campaign.sh の run-kind job(server 2vCPU=CPU2-3、farm CPU8-31、
  計測器は CPU4-7 に taskset)

## 判定

**現実装の ramp(client_procs=1 固定)は fanout(room)系 scenario の
2 倍刻み screening を置換できない。** reference campaign / A/A の screening は
2 倍刻みのまま設計する。

根拠(すべて同一ホスト・同一 farm 条件):

| 方式 | 結果 |
|---|---|
| 2x-step(enet、procs=1)| c4–c64 PASS/VALID。c128 以上は farm rcvbuf 溢れで INVALID(drops 650@c128 → 51 万@c256)= 正しく censored |
| ramp(enet、procs=1、4→512 step12)| 2/2 INVALID。break 判定は score 148 / 160(MD eventual 1 packet 欠け)だが、その level 帯は step 側で farm drops が出る領域 — **score は farm 汚染の疑いが濃く、SUT の境界として採用できない** |

ramp は break 後も total_conns まで登り続けるため、終端の桁外れ過負荷で
(a) farm socket 溢れ、(b) netem evidence 採取の窓超過(ledger #23)を
必ず踏む。step 側は同じ farm 不足を per-point gate が INVALID として開示した。
方法論の器差以前に、**単一 proc の farm が fanout の screening 範囲を
カバーできない**ことが決定的。

ramp を将来使うなら最低限: multi-proc 対応(または farm-valid 上限で
total_conns を cap)+ per-level の farm drop gate + 終端過負荷での
evidence 採取見直し。加えて gns/msquic は ramp 未実装(ledger #22)。

## 付随データ

- raw_udp × ref-room-wan(procs=8、2x-step): c4–c512 全点 FAIL/VALID。
  raw_udp は MD を非信頼 datagram で送る設計(adapter README の開示どおり)で、
  loss 1% regime では MD eventual=1.0 が構造的に不成立。**wan regime の
  anchor/equivalence 設計に raw_udp の MD 付き scenario は使えない**
  (workload 系か lan regime を使う)
- netem 実効値 gate の transient flap 1 件(enet c64 attempt-1: ping avg
  369ms、attempt-2 で PASS)。retry で吸収された

## 今回の計測器修正・知見(詳細は ledger)

- #23: 過負荷点の netem evidence 窓超過 INVALID flap → run job は計測器を
  空き bench CPU へ taskset(coordinator 対処)。ramp 終端の再発は kernel 側
  疑いで未解決
- #24: boot.sh に rmem/wmem 引き上げが無い → host prep で暫定 sysctl
- #25: enet client の room_relay が multi-proc farm で実行不能
  (expect 登録が proc ローカル)→ 凍結 farm 構成(procs=8)と矛盾、要修正
- run-kind job の失敗時診断回収(diag-attempt-N)を dispatch に追加

## 穴(pre-registration どおり打ち切り)

- enet-step-c0256 / c0512: 20 分 deadline 到達で未完了。原因は既知
  (farm rcvbuf 溢れの領域)で、再実行しても censored になるだけのため追わない
