/*
 * ccnx.h - A層(プロトコルスタック)単一公開ヘッダ / libccnx.a の唯一の入口
 *
 * B-283 CCNx セマンティック群ロボ通信デモ。
 * ドメイン(ロボ/救助/星)を一切知らない。RFC 8569(Semantics)/8609(TLV) の
 * フォワーダ + フェイス + アプリ向け Portal API のみを提供する。
 * B層(robot.c 等)はこのヘッダの関数だけを呼ぶ。ワイヤのバイトには触れない。
 *
 * 本ヘッダを include すれば「ワイヤコーデック(codec.h)」と「Portal API」の
 * 両方の宣言が揃う。実装は次の3翻訳単位に分割される(責務境界=DESIGN §3):
 *   codec.*     : 8609 バイト ⇄ ccnx_msg_t のみ
 *   ccnx_tables.*: ccnx_name_t を鍵にした PIT/FIB/CS のみ
 *   transport.* : UDP バイト送受信のみ
 * ccnx.c(Portal) がこの3者を束ね本 API を実装する。
 *
 * ★契約: 本 API の下でワイヤに出るバイト列は WIRE.md に厳密に従う。
 *   C版 libccnx.a と Go版 package ccnx は同一メッセージで完全バイト一致すること
 *   (INTEROP.md の合否条件)。それが独立2実装の相互運用=RFC忠実の物証である。
 *
 * 参照: RFC 8609 (~/Downloads/B-283-CCNx/rfc8609.txt)
 *       RFC 8569 (~/Downloads/B-283-CCNx/rfc8569.txt)
 */
#ifndef CCNX_H
#define CCNX_H

#include <stdint.h>
#include <stddef.h>

#include "codec.h"   /* ワイヤコーデック(ccnx_name_t/ccnx_msg_t/encode/decode)を取り込む */

/* =====================================================================
 * 1. Portal 定数(WIRE.md / PROTOCOL.md と一致)
 * ===================================================================== */
#define CCNX_DEFAULT_HOP_LIMIT     8       /* Interest 既定 HopLimit */
#define CCNX_DEFAULT_LIFETIME_MS   4000    /* Interest Lifetime(ローカル PIT タイマ。ワイヤには出さない) */
#define CCNX_DEFAULT_FRESHNESS_MS  10000   /* Content Object の CS 保持時間 */
#define CCNX_MAX_FIB_ENTRIES       64
#define CCNX_MAX_PIT_ENTRIES       256
#define CCNX_MAX_CS_ENTRIES        256
#define CCNX_MAX_NEXTHOPS          16      /* 1 FIB エントリあたりの nexthop ポート数 */
#define CCNX_URI_MAX               512     /* URI 文字列バッファ既定サイズ */

/* API 返り値(負値=失敗)。0 以上=成功(件数やバイト数を返す関数はその値) */
#define CCNX_OK                     0
#define CCNX_ERR_ARG               (-1)
#define CCNX_ERR_FULL              (-2)    /* テーブル/nexthop 満杯 */
#define CCNX_ERR_URI               (-3)    /* URI パース失敗 */
#define CCNX_ERR_IO                (-4)    /* ソケット I/O 失敗 */
#define CCNX_ERR_DECODE            (-5)    /* 受信バイトが 8609 として不正 */
#define CCNX_ERR_NOMATCH           (-6)    /* FIB 一致なし */

/* =====================================================================
 * 2. Portal ハンドル(不透明)
 *    face(UDP ソケット) + PIT/FIB/CS + コールバック表を内包する。
 *    = 「フォワーダ内蔵の完全な CCNx ノード」1個。
 * ===================================================================== */
typedef struct ccnx_portal ccnx_portal_t;

/* on_interest: 登録プレフィックスに一致する Interest が到来したとき呼ばれる。
 *   name        : 到来した Interest の完全名(読み取り専用)
 *   payload/len : Interest ペイロード(無ければ len=0)
 *   ctx         : register_prefix で渡した任意ポインタ
 * ハンドラ内で ccnx_publish() を呼べば要求元へ Content Object を返せる。 */
typedef void (*ccnx_on_interest_fn)(ccnx_portal_t *p,
                                    const ccnx_name_t *name,
                                    const uint8_t *payload, uint16_t payload_len,
                                    void *ctx);

/* on_content: 自分が express した Interest を満たす Content Object が到来したとき、
 * および傍受(overhear)した Content Object を CS に載せたときに呼ばれる。
 *   is_overheard: 0=自 Interest への応答 / 1=傍受(選抜・claim 監視に使う) */
typedef void (*ccnx_on_content_fn)(ccnx_portal_t *p,
                                   const ccnx_name_t *name,
                                   const uint8_t *payload, uint16_t payload_len,
                                   int is_overheard, void *ctx);

/* =====================================================================
 * 3. ライフサイクル(Cefore portal の connect/disconnect 相当)
 * ===================================================================== */

/* listen_port に UDP バインドした Portal を1個生成。失敗時 NULL。
 * bind_addr=NULL は "127.0.0.1"。tap_port>0 なら送出する全 8609 datagram の
 * 複製をそのポートへも送る(Go ワイヤ観測=ライブ相互運用点。PROTOCOL §5)。 */
ccnx_portal_t *ccnx_portal_open(const char *bind_addr, uint16_t listen_port,
                                uint16_t tap_port);
void           ccnx_portal_close(ccnx_portal_t *p);

/* on_content の既定ハンドラ(傍受 Content Object を受ける)を1本設定。
 * 個々の express_interest でも per-Interest ハンドラを指定できる。 */
void ccnx_set_default_on_content(ccnx_portal_t *p, ccnx_on_content_fn cb, void *ctx);

/* =====================================================================
 * 4. FIB / プレフィックス登録
 * ===================================================================== */

/* プレフィックスを自ノードの担当として登録し、一致 Interest 到来時の
 * ハンドラを結ぶ(アプリ登録=Cefore の prefix registration 相当)。
 *   prefix_uri: "/area/A-2" 等。空 "/" は全名前(デフォルトルート)。 */
int ccnx_register_prefix(ccnx_portal_t *p, const char *prefix_uri,
                         ccnx_on_interest_fn cb, void *ctx);

/* プレフィックス → nexthop(peer の UDP ポート)の FIB 転送経路を追加。
 * 最長一致(ccnx_name_is_prefix)で選ばれた全 nexthop へ Interest を転送する。
 * 同一プレフィックスに複数回呼べば nexthop が足される(最大 CCNX_MAX_NEXTHOPS)。 */
int ccnx_fib_add_route(ccnx_portal_t *p, const char *prefix_uri, uint16_t nexthop_port);

/* =====================================================================
 * 5. アプリ向け送出 API(Portal core = Cefore の express/publish 相当)
 * ===================================================================== */

/* Interest を送出。名前を組み立て 8609 encode し、FIB 最長一致の nexthop へ送る。
 * PIT に登録し、lifetime_ms 経過で自動失効。応答が来たら cb を呼ぶ。
 *   payload=NULL,payload_len=0 で無ペイロード。hop_limit=0 は既定値に補正。
 *   lifetime_ms=0 は CCNX_DEFAULT_LIFETIME_MS。cb=NULL は既定ハンドラを使う。
 * 返り値: 送出バイト数 / 負値=失敗。 */
int ccnx_express_interest(ccnx_portal_t *p, const char *name_uri,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t hop_limit, uint32_t lifetime_ms,
                          ccnx_on_content_fn cb, void *ctx);

/* Content Object を発行。CS に格納し、待機中の PIT を満たすと同時に、
 * 全既知 nexthop(または直近の要求元)へ送出する。傍受選抜・claim announce の主手段。
 *   freshness_ms=0 は CCNX_DEFAULT_FRESHNESS_MS。
 * 返り値: 送出バイト数 / 負値=失敗。 */
int ccnx_publish(ccnx_portal_t *p, const char *name_uri,
                 const uint8_t *payload, uint16_t payload_len,
                 uint32_t freshness_ms);

/* =====================================================================
 * 6. 実行ループ(Cefore の cef_client_read + poll 相当)
 * ===================================================================== */

/* 1 反復: UDP を最大 timeout_ms 待って受信し、decode → PIT/FIB/CS 処理 →
 * 該当コールバック起動。加えて PIT/CS の期限切れを掃除する。
 *   timeout_ms=0 は非ブロッキング(即時 return)。
 * 返り値: このティックで処理した datagram 数 / 負値=失敗。
 * B層は「毎ティック ccnx_tick → 自挙動更新 → 状態 emit」を回す。 */
int ccnx_tick(ccnx_portal_t *p, uint32_t timeout_ms);

/* ccnx_stop が呼ばれるまで ccnx_tick(既定 timeout)を回し続ける。
 * 単純なノードで使う。robot.c は挙動更新を挟むため tick を直接回す。 */
int  ccnx_run(ccnx_portal_t *p);
void ccnx_stop(ccnx_portal_t *p);

/* =====================================================================
 * 7. テーブル内観(状態 emit 用。PROTOCOL §2 の pit_count/fib_count/cs_count)
 * ===================================================================== */
int ccnx_pit_count(const ccnx_portal_t *p);
int ccnx_fib_count(const ccnx_portal_t *p);
int ccnx_cs_count(const ccnx_portal_t *p);

/* listen_port の取得(状態 topology 用) */
uint16_t ccnx_portal_port(const ccnx_portal_t *p);

#endif /* CCNX_H */
