/*
 * ccnx_tables.h - PIT / FIB / CS (RFC 8569 フォワーダの三表)
 *
 * B-283 A層。ccnx_name_t を鍵にした純データ構造。UDP もソケットも知らない。
 * 時刻は呼び出し側から絶対 ms(単調)で与えられる(表は時計を持たない)。
 * ドメイン(ロボ/救助)非依存。DESIGN §3 の責務境界を越えない。
 */
#ifndef CCNX_TABLES_H
#define CCNX_TABLES_H

#include "ccnx.h"   /* CCNX_MAX_* 定数・CCNX_* エラーコード・ccnx_name_t を取り込む */
#include <stdint.h>

/* PIT が要求元を表す特別値: 0 = ローカルアプリ(自 express_interest 起点)。
 * UDP ポートは 1..65535 なので 0 と衝突しない。 */
#define CCNX_LOCAL_REQUESTER 0

typedef struct {
    int          in_use;
    ccnx_name_t  name;
    uint16_t     requesters[CCNX_MAX_NEXTHOPS]; /* 要求元ポート(0=ローカル) */
    int          n_requesters;
    uint64_t     expire_ms;                     /* 絶対 ms。超過で失効 */
    /* この PIT について「転送済み」の最大 HopLimit(RFC 8569 §2.4.2)。
     * これを上回る HopLimit の Interest は集約せず必ず転送しなければならない。 */
    uint8_t      max_hop_limit;
} ccnx_pit_entry_t;

typedef struct {
    int          in_use;
    ccnx_name_t  prefix;
    uint16_t     nexthops[CCNX_MAX_NEXTHOPS];
    int          n_nexthops;
} ccnx_fib_entry_t;

typedef struct {
    int          in_use;
    ccnx_name_t  name;
    uint8_t      payload[CCNX_MAX_PAYLOAD];
    uint16_t     payload_len;
    uint64_t     expire_ms;
} ccnx_cs_entry_t;

typedef struct {
    ccnx_pit_entry_t pit[CCNX_MAX_PIT_ENTRIES];
    ccnx_fib_entry_t fib[CCNX_MAX_FIB_ENTRIES];
    ccnx_cs_entry_t  cs[CCNX_MAX_CS_ENTRIES];
} ccnx_tables_t;

/* ccnx_pit_insert の結果。RFC 8569 §2.4.2 の「集約してよいか / 必ず転送すべきか」判定に使う。 */
typedef struct {
    int index;            /* PIT index / 失敗時 -1 */
    int aggregated;       /* 1 = 有効(未失効)な同名 PIT が既に在った */
    int retransmit;       /* 1 = 同一前ホップからの再送 → §2.4.2 MUST forward */
    int larger_hop_limit; /* 1 = 転送済み最大 HopLimit を上回る → §2.4.2 MUST forward */
} ccnx_pit_insert_res_t;

void ccnx_tables_init(ccnx_tables_t *t);

/* --- FIB --- */
/* prefix に nexthop_port を追加(既存 prefix には足す、無ければ新規)。0/失敗負値 */
int  ccnx_fib_add(ccnx_tables_t *t, const ccnx_name_t *prefix, uint16_t nexthop_port);
/* name に最長一致する FIB エントリの nexthop 群を out へ。返り値=nexthop 数(0=一致無)。 */
int  ccnx_fib_lookup(const ccnx_tables_t *t, const ccnx_name_t *name,
                     uint16_t *out_ports, int max_ports);
/* name の最長一致 nexthop 集合から exclude_port を除いてなお転送先が在るか(1/0)。
 * RFC 8569 §10.3.1: 唯一の経路が Interest の到来元しか無い場合は「経路なし」として
 * 扱い、黒穴化せず T_RETURN_NO_ROUTE を返す。除外不要なら exclude_port に 0
 * (CCNX_LOCAL_REQUESTER。実 UDP ポートと衝突しない)を渡す。 */
int  ccnx_fib_has_route(const ccnx_tables_t *t, const ccnx_name_t *name,
                        uint16_t exclude_port);
/* port が FIB のいずれかのエントリに nexthop として掲載されているか(1/0)。
 * RFC 8569 §2.4.5(1) の前ホップ検査(Content Object の到来 face 検証)に使う。 */
int  ccnx_fib_is_nexthop(const ccnx_tables_t *t, uint16_t port);

/* --- PIT --- */
/* name を PIT へ登録。有効(未失効)な同名が在れば requester を統合(Interest 集約)。
 * now_ms で失効判定し、失効済みエントリは不在扱い(遅延削除)する(§2.4.2 "valid
 * pending Interest")。hop_limit は当該 Interest の受信 HopLimit。
 * res != NULL なら集約可否の判定材料(ccnx_pit_insert_res_t)を書き込む。
 * 返り値=PIT index / 満杯は負値。 */
int  ccnx_pit_insert(ccnx_tables_t *t, const ccnx_name_t *name,
                     uint16_t requester_port, uint64_t expire_ms,
                     uint64_t now_ms, uint8_t hop_limit,
                     ccnx_pit_insert_res_t *res);
/* name に一致する有効(未失効)な PIT を探す(CO 満足用、完全一致)。index / -1。 */
int  ccnx_pit_find(const ccnx_tables_t *t, const ccnx_name_t *name, uint64_t now_ms);
/* index の要求元一覧を out へ取り出しエントリを解放。返り値=要求元数 / 負値。 */
int  ccnx_pit_consume(ccnx_tables_t *t, int index,
                      uint16_t *out_requesters, int max);
/* 期限切れ PIT を掃除。掃除件数を返す。 */
int  ccnx_pit_expire(ccnx_tables_t *t, uint64_t now_ms);
/* 表レベル計数(Portal 公開 API ccnx_pit_count とは別名で衝突回避) */
int  ccnx_tbl_pit_count(const ccnx_tables_t *t);

/* --- CS --- */
/* name の CO を CS へ挿入(同名は上書き)。0/負値。 */
int  ccnx_cs_insert(ccnx_tables_t *t, const ccnx_name_t *name,
                    const uint8_t *payload, uint16_t payload_len,
                    uint64_t expire_ms);
/* name に完全一致し未失効な CS を探す。index / -1。 */
int  ccnx_cs_find(const ccnx_tables_t *t, const ccnx_name_t *name, uint64_t now_ms);
int  ccnx_cs_expire(ccnx_tables_t *t, uint64_t now_ms);
int  ccnx_tbl_cs_count(const ccnx_tables_t *t);

#endif /* CCNX_TABLES_H */
