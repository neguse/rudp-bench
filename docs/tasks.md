# Tasks

`docs/adapter-audit.md` の改善で、adapter 変更だけでは完了しない残作業。

- [ ] `coop_rudp` core に per-conn close/abort と max retransmit/idle timeout を追加し、crashed peer の reliable retransmit queue を解放できるようにする。
- [ ] 全 native adapter で `setsockopt(SO_RCVBUF/SO_SNDBUF)` 後に `getsockopt` で実値を確認し、sysctl clamp を run metadata または diagnostics に出す。
- [ ] `msquic` datapath の UDP socket buffer 要求を現実的な値にし、SO_SNDBUF も明示設定する。adapter から制御できない場合は vendored MsQuic patch として管理する。
- [ ] `udt4` adapter が UDT 内部の EXP timeout / broken 状態を `is_connected()` と `send()` に反映する。
- [ ] `raknet`/`slikenet` の `RAKPEER_USER_THREADED=1` でも recv thread が生成される問題を vendored SLikeNet 側で修正し、adapter の abandon leak 回避を不要にする。
- [ ] adapter inbox / pending queue が無制限の経路に上限と backpressure を入れる: `gns` inbox, `msquic` inbox, `lsquic` stream `pending_writes`, `udt4` adapter `out_pending`。
- [ ] `quiche` stream path の partial write を破棄せず、残りを pending 化するか明示 backpressure として返す。
- [ ] 固定 RTO かつ backoff なしの adapter（`coop_rudp`, `apex_rudp`, `mini_rudp`, `yojimbo`）に RTT ベース RTO/backoff を入れるか、固定 RTO を使うベンチ前提を結果 metadata に明示する。
- [ ] `mini_rudp` の per-packet 専用 ACK を piggyback / cumulative ACK に置き換え、ACK traffic と syscall 数を下げる。
- [ ] ベンチ用に輻輳制御を上書きしている adapter（`kcp` nocwnd, `msquic` BBR, `quiche` BBRv2, `lsquic` BBR, `udt4` BenchCCC）に library-default variant を追加し、結果解釈で区別できるようにする。
- [ ] `gns` の 1000conn global lock collapse を poll group 分割などで再評価し、暗号なし/暗号あり variant の差も metadata に出す。
