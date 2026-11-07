// SUT の共通部分。負荷の生成、計測、orchestrator とのやり取りはここが持ち、
// ライブラリごとの層は bk_transport を埋めるだけにする。
//
// ライブラリの層は service() の中から bk_sut_on_* を呼んでイベントを返す。
// 共通部分はスレッド安全でないため、bk_sut_on_* は service() を呼んだスレッドから
// だけ呼ぶこと(別スレッドでイベントが届くライブラリは、層の中で溜めて渡す)。
#ifndef BK_SUT_H
#define BK_SUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bk_sut bk_sut;

typedef struct {
  // orchestrator の run config の transport と同じ名前
  const char *name;
  // --describe の class_mapping, coalescing, cc_algo, thread_model,
  // encryption, tuning を並べた JSON の断片(外側の {} なし)
  const char *describe;
  size_t max_payload;

  // server: port で待ち受ける。接続には 0..max_conns-1 の番号を振る。
  void *(*listen)(bk_sut *sut, uint16_t port, int max_conns);
  // client: conns 本の接続を始める。接続の番号は 0..conns-1。
  void *(*connect)(bk_sut *sut, const char *host, uint16_t port, int conns);
  // ライブラリに渡せたら 0。data はこの呼び出しの間だけ有効。
  int (*send)(void *t, int conn, bool must_deliver, const void *data,
              size_t len);
  // 1 tick 分の送信を渡し終えたときに呼ぶ。要らないライブラリは NULL にする。
  void (*flush)(void *t);
  // 送受信を進める。イベントがなければ最大 timeout_ns 待つ。失敗は -1。
  int (*service)(void *t, uint64_t timeout_ns);
  // 送信待ちの量。ライブラリの公開 API で取れなければ NULL にする。
  // 単位はライブラリが返すもの(bytes / segments など)を send_queue_unit に書く。
  int64_t (*send_queue)(void *t, int conn);
  const char *send_queue_unit;
  void (*close)(void *t);
} bk_transport;

void bk_sut_on_connect(bk_sut *sut, int conn);
void bk_sut_on_receive(bk_sut *sut, int conn, const void *data, size_t len);
void bk_sut_on_disconnect(bk_sut *sut, int conn);

// --host の値。server でも指定すれば取れる(指定がなければ NULL)。
// 自分のアドレスを知る必要がある server 用(yojimbo の接続トークンなど)。
const char *bk_sut_host(const bk_sut *sut);

// argv[1] が server / client / --describe。戻り値は main の終了コード。
int bk_sut_main(int argc, char **argv, const bk_transport *transport);

#ifdef __cplusplus
}
#endif

#endif  // BK_SUT_H
