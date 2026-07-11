// benchkit — rudp-bench v3 の計測共有ライブラリ(C ABI)
//
// 契約は ../benchspec/README.md が唯一の真実源。この header はその C 表現。
// スレッド安全性: 全 API は非スレッド安全。呼び出し側(通常は各 proc の計測スレッド
// 1本)が直列化する。
#ifndef BENCHKIT_H
#define BENCHKIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- clock -----------------------------------------------------------------

// CLOCK_MONOTONIC を ns で返す。同一ホスト内でプロセス間比較可能。
uint64_t bk_now_ns(void);

// ---- payload ---------------------------------------------------------------

// benchspec の 32B ヘッダ。wire 上は little-endian 固定。
#define BK_HEADER_SIZE 32
#define BK_MIN_PAYLOAD BK_HEADER_SIZE

// flags bits
#define BK_FLAG_MUST_DELIVER (1u << 0)  // 0 なら loss-tolerant
#define BK_FLAG_MEASURE      (1u << 1)  // 計測窓内の送信
#define BK_FLAG_BROADCAST    (1u << 2)  // 0 なら echo
#define BK_FLAG_DIRECTION_SHIFT 3u
#define BK_FLAG_DIRECTION_MASK  (3u << BK_FLAG_DIRECTION_SHIFT)

// v3 traffic direction。0 は v2 room-relay payload と同じ wire 表現。
typedef enum {
  BK_DIRECTION_ROOM_RELAY = 0,
  BK_DIRECTION_CLIENT_TO_SERVER = 1,
  BK_DIRECTION_SERVER_TO_CLIENT = 2,
} bk_direction;

#define BK_FLAG_DIRECTION(direction)                                      \
  ((uint8_t)(((uint8_t)(direction)) << BK_FLAG_DIRECTION_SHIFT))
#define BK_FLAGS_DIRECTION(flags)                                         \
  ((bk_direction)(((flags) & BK_FLAG_DIRECTION_MASK) >>                    \
                  BK_FLAG_DIRECTION_SHIFT))

#define BK_TRAFFIC_ID_ROOM_RELAY          0u
#define BK_TRAFFIC_ID_AUTHORITATIVE_INPUT 1u
#define BK_TRAFFIC_ID_AUTHORITATIVE_STATE 2u

typedef struct {
  uint64_t seq;          // slot id。(logical stream, target) ごとに 1 起点
  uint64_t sched_ts_ns;  // 本来送るべきだった予定時刻
  uint64_t send_ts_ns;   // 実送信直前の時刻
  uint8_t flags;
  uint32_t origin_id;    // 送信元 global conn id(常に記入)
  uint8_t traffic_id;    // v3 logical traffic id。v2 room-relay は 0
} bk_header;

// buf(len >= BK_HEADER_SIZE)にヘッダを書く。pad 領域には触れない。
// 戻り値 0=ok / -1=len 不足。
int bk_payload_write(void *buf, size_t len, const bk_header *h);
// 戻り値 0=ok / -1=len 不足または reserved bit 違反。
int bk_payload_read(const void *buf, size_t len, bk_header *h);
// 通常 payload の offset 32 以降を header から決まる byte pattern で埋める。
// environment-baseline / room-relay / authoritative input で使用し、受信側は
// validate して payload corruption と未初期化領域を検出する。
int bk_payload_fill_body(void *buf, size_t len, const bk_header *h);
int bk_payload_validate_body(const void *buf, size_t len, const bk_header *h);

// authoritative server-to-client state body(v3)。offset 32 から、この
// target 宛 state に反映済みの client input seq を little-endian で置く。
#define BK_AUTHORITATIVE_STATE_MIN_PAYLOAD (BK_HEADER_SIZE + 8u)
int bk_authoritative_state_write_applied_input_seq(void *buf, size_t len,
                                                   uint64_t seq);
int bk_authoritative_state_read_applied_input_seq(const void *buf, size_t len,
                                                  uint64_t *out);
// offset 40 以降を target global conn id 固有の pattern で埋める。
int bk_authoritative_state_fill_target_pad(void *buf, size_t len,
                                           uint32_t target_id);
int bk_authoritative_state_validate_target_pad(const void *buf, size_t len,
                                               uint32_t target_id);

// ---- control channel(orchestrator との UDS 接続)--------------------------

typedef struct {
  uint64_t start_at_ns;
  uint64_t stop_at_ns;
  uint64_t drain_until_ns;
} bk_schedule;

typedef struct bk_control bk_control;

// path が NULL なら環境変数 BENCH_CONTROL_SOCK を使う。失敗時 NULL。
bk_control *bk_control_connect(const char *path);
void bk_control_close(bk_control *c);

// hello を送る。role は "server" / "client"。戻り値 0=ok / -1=err。
int bk_control_hello(bk_control *c, const char *role, const char *transport,
                     int proc_index);
// ready を送る(server は conns=0)。
int bk_control_ready(bk_control *c, int conns);
// schedule をブロッキングで待ち、受信直後に sched_ack(margin 付き)を返信する。
// margin_ns = start_at − 受信時刻(負になりうる。gate 判定は orchestrator 側)。
int bk_control_wait_schedule(bk_control *c, bk_schedule *out);
// schedule の非ブロッキング確認。受信済みなら sched_ack を返信して 1、未着なら 0、
// エラーは -1。自前の event loop を持つ server は ready 後、これを service の合間に
// 呼ぶこと(wait_schedule でブロックすると接続受付が止まり、client が ready に
// なれず全体がデッドロックする)。
int bk_control_poll_schedule(bk_control *c, bk_schedule *out);
// done を送る。stats_json は bk_metrics_dump_json の出力等。
int bk_control_done(bk_control *c, const char *stats_json);

// ---- 定常判定つき warmup(benchspec v2)------------------------------------
//
// schedule は暫定窓(start_at = ready + warmup 上限)として届く。client は
// 受信直後から送信を開始し、累積送受カウントを周期報告(rate)する。
// orchestrator が全 client の定常を検出すると確定窓(window)が届くので、
// ローカルの schedule と plan の計測窓を差し替える。window が届かないまま
// 暫定 start_at に達したら暫定窓のまま計測に入る。

// rate を送る(sent/received は累積の生カウント)。戻り値 0=ok / -1=err。
int bk_control_send_rate(bk_control *c, uint64_t sent, uint64_t received);
// window の非ブロッキング確認。受信したら io を確定窓で上書きし window_ack を
// 返信して 1。未着 0 / エラー -1。schedule 受信後にのみ呼ぶこと。
int bk_control_poll_window(bk_control *c, bk_schedule *io);

// ---- 送信計画(slot planner)------------------------------------------------

// 1 stream = 1 (traffic_id, direction, class, distribution, rate) の送信系列。
// room-relay client は conn ごとに plan を持つ。authoritative server は
// server tick の slot を1つ生成し、各 target を別 logical slot として会計する。
typedef struct {
  bool must_deliver;
  bool broadcast;
  uint8_t traffic_id;
  bk_direction direction;
  uint64_t interval_ns;  // 送信間隔(rate の逆数)
} bk_stream;

typedef struct {
  uint64_t sched_ts_ns;
  uint64_t seq;      // stream 内 1 起点連番(= slot id)
  int stream_index;  // どの bk_stream か
  uint8_t flags;     // class/dist に加え、sched が計測窓内なら BK_FLAG_MEASURE
  uint8_t traffic_id;
} bk_slot;

typedef struct bk_plan bk_plan;

// streams は copy される。start_ns から各 stream が interval 刻みで slot を生む。
// measure_start/stop は BK_FLAG_MEASURE の判定にのみ使う。
bk_plan *bk_plan_new(const bk_stream *streams, int n_streams, uint64_t start_ns,
                     uint64_t measure_start_ns, uint64_t measure_stop_ns);
void bk_plan_free(bk_plan *p);
// 計測窓を差し替える(benchspec v2 の window 受信時)。measure bit は slot 生成時
// 評価なので、旧窓にまだ達していなければ遡及の不整合は起きない。
void bk_plan_set_window(bk_plan *p, uint64_t measure_start_ns,
                        uint64_t measure_stop_ns);

// now までに due になった slot を1つ返す(なければ false)。due が複数あれば
// sched_ts 順。呼び出し側は send するか、coalesce するなら未送信として
// bk_metrics_on_slot(submitted=false) に回す。
bool bk_plan_next(bk_plan *p, uint64_t now_ns, bk_slot *out);
// 次の slot の sched_ts(idle sleep の目安)。slot がもうなければ UINT64_MAX。
uint64_t bk_plan_peek_ns(const bk_plan *p);

// ---- metrics ---------------------------------------------------------------

// 会計は benchspec「計測の定義」に従う:
// - 分母は slot(未送信 slot も miss として数える)
// - 重複判定は (local conn, origin_id, traffic_id, direction, class,
//   seq) の初観測のみ集計
// - staleness: 同じ flow key ごとに最新受信 update の age をサンプル
// - deadline hit: recv_ts − sched_ts <= deadline の slot 割合(must-deliver)
// - measurement bit の立った message のみ集計対象
typedef struct {
  // latest-flow tracking の origin 上限。legacy aggregate delivery は
  // total_conns を知らない旧 client の互換性のため、範囲外 origin も会計する。
  uint32_t max_origin_id;
  uint64_t deadline_ns;          // must-deliver の締切 D
  uint64_t staleness_period_ns;  // staleness サンプル周期(既定 10ms)
  // 1 metrics instance が latest-flow 会計する local conn 数の上限。
  // 0 は local conn を1 flowへ集約する legacy mode。dedup key の
  // local_index は維持されるので、room-relay の旧 caller は0のまま使える。
  uint32_t max_local_index;
} bk_metrics_config;

typedef struct bk_metrics bk_metrics;

bk_metrics *bk_metrics_new(const bk_metrics_config *cfg);
void bk_metrics_free(bk_metrics *m);

// 送信側: slot の結果を記録する。submitted=false は coalesce/backpressure 等で
// transport に渡さなかった slot(分母には入る)。
void bk_metrics_on_slot(bk_metrics *m, const bk_header *h, bool submitted);
// 受信側: payload を parse 済みの header と受信時刻を渡す。
// local_index は受信した自 proc 内 conn の 0 起点 index。broadcast では同一
// メッセージの複製が自 proc の複数 conn に届くため、重複判定は
// (local_index, origin, traffic_id, direction, class, seq) で行う
// (受信側 conn を含めないと
// 正当な複製が duplicate 扱いになり delivery が壊れる)。
void bk_metrics_on_recv(bk_metrics *m, uint32_t local_index, const bk_header *h,
                        uint64_t recv_ts_ns);
// loss-tolerant の受信予定 flow を計測窓前に登録する。
// authoritative client は全 local conn に対し server origin を登録する。
// 初回 update が届かない間も first_sched_ts_ns 起点の age を
// staleness に入れ、未受信 flow が分布から消えるのを防ぐ。
// 戻り値: 0=ok / -1=local_index または origin_id が config 範囲外。
int bk_metrics_expect_latest(bk_metrics *m, uint32_t local_index,
                             uint32_t origin_id, uint8_t traffic_id,
                             bk_direction direction,
                             uint64_t first_sched_ts_ns);
// staleness サンプラを駆動する。受信ループから高頻度で呼んでよい
// (staleness_period_ns 未満の呼び出しは内部で間引く)。
// 計測窓 [start_at, stop_at) 内でのみ呼ぶこと。warmup 中(measured update が
// まだない)や drain 中(送信停止後で age が伸びるだけ)のサンプルは分布を汚染する。
void bk_metrics_tick(bk_metrics *m, uint64_t now_ns);

// 集計結果を JSON で path に書く(orchestrator がマージする)。
// metrics JSON version 2。内容: legacy class 別 aggregate に加え、
// traffic[] に (traffic_id, direction, class) 別 counts / deadline /
// latency / update-gap / staleness、および送信/受信の生カウント。
// raw.timestamp_order_violations は unique measured receive のうち
// sched_ts <= send_ts <= recv_ts を満たさない件数。
// ヒストグラムは HDR 方式(log2 メジャー + 16 線形サブビン、範囲 1us〜100s)、
// percentile は nearest-rank ceil(count*p)。bin 配列ごと出力し、加算マージ可能。
int bk_metrics_dump_json(const bk_metrics *m, const char *path);

// 主要値の取り出し(テスト・自己判定用)。
// 全カウントは生値であり、benchkit は fanout の期待受信数を計算しない
// (大域分母は orchestrator が計算する — benchspec「client farm の計測規約」)。
// broadcast の期待受信数 = (slots - slots_broadcast) + slots_broadcast × 総接続数。
typedef struct {
  uint64_t slots;             // 計測窓内の総 slot 数(送信側の生カウント)
  uint64_t slots_broadcast;   // うち BK_FLAG_BROADCAST の slot 数
  uint64_t submitted;         // transport に渡した数
  uint64_t delivered_unique;  // 初観測受信数
  uint64_t duplicates;        // 重複受信数
  uint64_t deadline_hit;      // 締切内到達数(must-deliver 用)
  uint64_t expected_flows;    // expect_latest 登録(旧モードは初受信で自動)
  uint64_t observed_flows;    // 計測 bit 受信を1回以上観測した flow
  uint64_t never_received_flows;  // expected_flows - observed_flows
} bk_class_counts;

// must_deliver=true/false それぞれの counts を返す。
void bk_metrics_counts(const bk_metrics *m, bool must_deliver,
                       bk_class_counts *out);
// staleness の percentile(ns)。p は 0.0-1.0。サンプルがなければ 0。
uint64_t bk_metrics_staleness_pctl(const bk_metrics *m, double p);
// update gap((origin, class) の latest-value が前進した受信同士の間隔)の
// percentile(ns)。「ロス/HoL 事象1回あたりの空白時間」に対応する
// 事象アライン指標。窓全体の staleness p99 より事象数に対して安定。
// class 別(混合にすると must-deliver の送信間隔が p99 を支配する)
uint64_t bk_metrics_update_gap_pctl(const bk_metrics *m, bool must_deliver,
                                    double p);
// sched 起点 latency percentile(ns)。class 別。
uint64_t bk_metrics_latency_pctl(const bk_metrics *m, bool must_deliver,
                                 double p);
// v3 traffic series の分離取得。該当 series があれば 0、無ければ
// -1(counts は 0 クリア)。legacy bk_metrics_counts/pctl は全 traffic
// を class だけで合算した互換 view。
int bk_metrics_traffic_counts(const bk_metrics *m, uint8_t traffic_id,
                              bk_direction direction, bool must_deliver,
                              bk_class_counts *out);
uint64_t bk_metrics_traffic_staleness_pctl(const bk_metrics *m,
                                           uint8_t traffic_id,
                                           bk_direction direction, double p);
uint64_t bk_metrics_traffic_update_gap_pctl(const bk_metrics *m,
                                            uint8_t traffic_id,
                                            bk_direction direction,
                                            bool must_deliver, double p);
uint64_t bk_metrics_traffic_latency_pctl(const bk_metrics *m,
                                         uint8_t traffic_id,
                                         bk_direction direction,
                                         bool must_deliver, double p);
// traffic 固有 deadline。未設定は bk_metrics_config.deadline_ns。
// 計測を記録する前に設定すること。
int bk_metrics_set_traffic_deadline(bk_metrics *m, uint8_t traffic_id,
                                    bk_direction direction,
                                    uint64_t deadline_ns);
// 生カウント(計測 bit 無関係の累積)。rate 報告(benchspec v2)用。
// 不要な出力は NULL でよい。
void bk_metrics_raw_counts(const bk_metrics *m, uint64_t *slots,
                           uint64_t *submitted, uint64_t *recv_measured,
                           uint64_t *recv_unmeasured);

// ---- 定常判定つき warmup の client ループヘルパ(benchspec v2)--------------

// rate の周期送信(250ms)と window の受信を1呼び出しにまとめる。
// sent/received は bk_metrics_raw_counts の submitted / (recv_measured +
// recv_unmeasured) の累積値。metrics をロックで守る実装(msquic 等)でも
// 使えるよう、値渡しにしてある(呼び出し側が自分の同期規約で読む)。
// window 受信時は schedule をその場で確定窓に上書きして 1 を返すので、
// 呼び出し側は自分の全 plan に bk_plan_set_window を適用すること。
// 窓確定後(window 受信 or 暫定 start_at 到達)は何もしない。
// 戻り値: 1=window 受信(schedule 更新済み)/ 0=継続 / -1=制御チャネルエラー。
typedef struct {
  uint64_t next_rate_ns;  // 0 なら初回呼び出し時に now+interval で初期化
  bool window_final;
} bk_steady;

int bk_steady_tick(bk_steady *st, bk_control *c, uint64_t sent,
                   uint64_t received, bk_schedule *schedule, uint64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif  // BENCHKIT_H
