# unknown ledger

異常・疑問・改善候補の記帳所。**現 epoch では追わず**ここに積む。
done の定義 = このファイルが空(全行が解決済みとして削除される)。
改善系の作業は「どのセル群に効くか」を宣言できれば即着手可、
できなければここに記帳して寝かせる。

| # | 事象 | 発見 epoch | 状態 |
|---|---|---|---|
| 2 | iperf3 の loss 実測が低 loss 率(0.1%)で +0.2pt 程度上振れ(終端 in-flight 計上疑い)。1% 以上では一致 | v2.0 | 記帳 — gate 許容内。低 loss regime を将来使うときに再確認 |
| 4 | coalesced slot の会計(attempted gate との衝突)は FireAndForget で顕在化しなくなったが契約上は未解決 | v2.0 | 記帳 — 本物の coalesce が観測されたら slots_coalesced を契約化(version 2) |
| 5 | farm 受信 rcvbuf: enet client は 4MB 明示(broadcast fanout 高 pps で kernel 既定が溢れ UDP drop delta 発火)。他 transport は同シグナル発火時に同処置 | E2 | 記帳 — 発火した transport から順次。server 側は触らない |
| 6 | farm CPU 律速の機械判定が未実装(attempted 低下は backpressure と区別できないため metric gate 委譲に変更)。sampler の client CPU を使った判別が改善候補 | E2 | 記帳 — E3 の validity gate 拡充で検討 |
| 7 | C# 系の wired capacity ~60-68 集中は **client-farm 律速と確定**(client 単独で完結する sched latency p99 が 819ms-1.4s、CPU は全プロセス <40% で暇。~16-17 conns/proc の負荷非依存天井 = Workstation GC / ThreadPool hill-climbing の形。pacing gate 追加で censored 化済み)。A/B 順: ①client に DOTNET_gcServer=1 ②client_procs 4→8 ③ThreadPool.SetMinThreads ④litenetlib pump 分割 | E2 | **解決(farm 構成凍結)** — A/B 結果: gcServer は magiconion に有効(天井2倍)、procs 4→8 で両者2倍(conns/proc がスケール変数)、SetMinThreads で websocket 4倍(ThreadPool hill-climbing が主因)。凍結構成 = 全 transport procs 8 + C# client に gcServer=1 + MinThreads(4×cores)。farm 天井: mo (128,256] / ws (256,512] — 全 archetype 主張範囲を被覆。GC カウンタ計装は E3 候補 |
| 8 | enet client が c800 超で `enet_host_service` エラー → exit 1(wired echo c802/896/1024)。直前 conns で quality break 済みのため capacity の結論には影響しないが、client 側の回復可能性を調査 | E2 | 記帳 |
| 9 | litenetlib farm 律速(UDP drop)は buffer では説明できない(LiteNetLib 既定 SocketBufferSize=1MB)。manual-mode pump が 32 manager/proc を回す構造の draining 律速の疑い | E2 | 記帳 — #7 と合わせて診断 |
| 11 | enet の loss 最悪点崩壊(br 98→7)は **packet throttle と機構確定**(source cite: throttle は reliable ACK でのみ加速するが md は 1Hz、減速は 2/32 per bad RTT sample → burst loss で 32→0 に滑落し unreliable の ~97% をライブラリが破棄)。cc_algo 開示と README 化済み。tuned-disclosed 変種 = enet_peer_throttle_configure(accel=32, decel=0)、upstream doc 根拠あり | E2 | 開示済み — 検証実験(throttle pin での1点再測)は tuned variant 実装時に |
| 12 | enet packet throttle は時定数が run duration 級(数十秒で 32→0)のため、loss セルの結果が duration 依存 — 10s screening では崩壊前、65-120s では崩壊後を測る。capacity(loss-worst 7/5)と boundary(健全)の乖離はこれで説明 | E2 | 記帳 — E3 では enet の loss セルに定常性チェック(前半/後半の p99 比較)を入れる |
| 13 | gns client は app スレッドが送信 pacing と poll-group 全 drain を兼ねるため、broadcast(受信 conns² スケール)で pacing stall → farm 打ち切り(procs 4/8 とも ≥64@br)。echo は 4 procs で正直な break(683)。E3 候補: drain budget/tick の導入(instrument 開示) or farm コア増(aws-metal) | E2 | 記帳 — E2 は下限表記で publish |
| 14 | enet の wired br 際(c~100)の delivery が run 間で二峰性(0.50 / 0.89 / 0.998 を同一条件で観測)。接続ストーム直後の cold-start(warmup 1s が conns に対して薄い)+ throttle 整定の疑い。capacity の際の精度に影響 | E3 | 記帳 — warmup の conns スケール(例: max(1s, conns×20ms))を次のブロック世代で検討。プロトコル変更なので replicate 一式の再測とセット |
| 15 | sentinel の基準が単一ブロック(101)参照。N=3 の median/IQR を基準にすれば magiconion video のような IQR 際の偽 drift が減る | E3 | 記帳 — aggregate 出力を sentinel の reference に使えるようにする |
