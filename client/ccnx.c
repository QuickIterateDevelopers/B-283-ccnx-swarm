/*
 * ccnx.c - Portal core(Cefore の portal/face API 相当)
 *
 * B-283 A層。codec(8609) + ccnx_tables(PIT/FIB/CS) + transport(UDP) を束ね、
 * ccnx.h のアプリ向け API を実装する。「フォワーダ内蔵の完全な CCNx ノード」1個。
 * ドメイン(ロボ/救助/星)を一切知らない。ワイヤは WIRE.md に厳密準拠。
 *
 * 参照: RFC 8569/8609, docs/DESIGN.md §4.1, docs/PROTOCOL.md
 */
#include "ccnx.h"
#include "ccnx_tables.h"
#include "transport.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 登録プレフィックス(アプリのプロデューサ登録) */
typedef struct {
    int                 in_use;
    ccnx_name_t         prefix;
    ccnx_on_interest_fn cb;
    void               *ctx;
} ccnx_reg_t;

/* 自 express_interest の応答待ち(名前→content コールバック) */
typedef struct {
    int                in_use;
    ccnx_name_t        name;
    ccnx_on_content_fn cb;
    void              *ctx;
    uint64_t           expire_ms;
} ccnx_pending_t;

struct ccnx_portal {
    ccnx_face_t        face;
    ccnx_tables_t      tables;

    ccnx_reg_t         regs[CCNX_MAX_FIB_ENTRIES];
    ccnx_pending_t     pending[CCNX_MAX_PIT_ENTRIES];

    ccnx_on_content_fn default_on_content;
    void              *default_on_content_ctx;

    int                running;
};

/* --- 単調時刻(ms)。表は時計を持たないので Portal が供給 --- */
static uint64_t now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
    return (uint64_t)time(NULL) * 1000ULL;
}

/* =====================================================================
 * ライフサイクル
 * ===================================================================== */
ccnx_portal_t *ccnx_portal_open(const char *bind_addr, uint16_t listen_port,
                                uint16_t tap_port) {
    ccnx_portal_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (ccnx_face_open(&p->face, bind_addr, listen_port, tap_port) != 0) {
        free(p);
        return NULL;
    }
    ccnx_tables_init(&p->tables);
    p->running = 0;
    return p;
}

void ccnx_portal_close(ccnx_portal_t *p) {
    if (!p) return;
    ccnx_face_close(&p->face);
    free(p);
}

void ccnx_set_default_on_content(ccnx_portal_t *p, ccnx_on_content_fn cb, void *ctx) {
    if (!p) return;
    p->default_on_content = cb;
    p->default_on_content_ctx = ctx;
}

/* =====================================================================
 * FIB / プレフィックス登録
 * ===================================================================== */
int ccnx_register_prefix(ccnx_portal_t *p, const char *prefix_uri,
                         ccnx_on_interest_fn cb, void *ctx) {
    if (!p || !prefix_uri) return CCNX_ERR_ARG;
    ccnx_name_t pre;
    if (ccnx_name_from_uri(&pre, prefix_uri) != 0) return CCNX_ERR_URI;
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++) {
        if (p->regs[i].in_use && ccnx_name_equal(&p->regs[i].prefix, &pre)) {
            p->regs[i].cb = cb;
            p->regs[i].ctx = ctx;
            return CCNX_OK;
        }
    }
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++) {
        if (!p->regs[i].in_use) {
            p->regs[i].in_use = 1;
            p->regs[i].prefix = pre;
            p->regs[i].cb = cb;
            p->regs[i].ctx = ctx;
            return CCNX_OK;
        }
    }
    return CCNX_ERR_FULL;
}

int ccnx_fib_add_route(ccnx_portal_t *p, const char *prefix_uri, uint16_t nexthop_port) {
    if (!p || !prefix_uri) return CCNX_ERR_ARG;
    ccnx_name_t pre;
    if (ccnx_name_from_uri(&pre, prefix_uri) != 0) return CCNX_ERR_URI;
    return ccnx_fib_add(&p->tables, &pre, nexthop_port);
}

/* 登録プレフィックスの中で name に最長一致するものを返す(index / -1) */
static int reg_match(const ccnx_portal_t *p, const ccnx_name_t *name) {
    int best = -1, best_len = -1;
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++) {
        if (!p->regs[i].in_use) continue;
        if (!ccnx_name_is_prefix(&p->regs[i].prefix, name)) continue;
        int l = p->regs[i].prefix.seg_count;
        if (l > best_len) { best_len = l; best = i; }
    }
    return best;
}

/* =====================================================================
 * pending(自 Interest の応答待ち)
 * ===================================================================== */
static void pending_add(ccnx_portal_t *p, const ccnx_name_t *name,
                        ccnx_on_content_fn cb, void *ctx, uint64_t expire) {
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (p->pending[i].in_use && ccnx_name_equal(&p->pending[i].name, name)) {
            p->pending[i].cb = cb;
            p->pending[i].ctx = ctx;
            p->pending[i].expire_ms = expire;
            return;
        }
    }
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (!p->pending[i].in_use) {
            p->pending[i].in_use = 1;
            p->pending[i].name = *name;
            p->pending[i].cb = cb;
            p->pending[i].ctx = ctx;
            p->pending[i].expire_ms = expire;
            return;
        }
    }
}

static int pending_take(ccnx_portal_t *p, const ccnx_name_t *name,
                        ccnx_on_content_fn *cb, void **ctx) {
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (p->pending[i].in_use && ccnx_name_equal(&p->pending[i].name, name)) {
            *cb = p->pending[i].cb;
            *ctx = p->pending[i].ctx;
            p->pending[i].in_use = 0;
            return 1;
        }
    }
    return 0;
}

/* =====================================================================
 * 送出 API
 * ===================================================================== */
int ccnx_express_interest(ccnx_portal_t *p, const char *name_uri,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t hop_limit, uint32_t lifetime_ms,
                          ccnx_on_content_fn cb, void *ctx) {
    if (!p || !name_uri) return CCNX_ERR_ARG;
    if (payload_len > CCNX_MAX_PAYLOAD) return CCNX_ERR_ARG;

    ccnx_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.packet_type = CCNX_PT_INTEREST;
    msg.hop_limit = (hop_limit == 0) ? CCNX_DEFAULT_HOP_LIMIT : hop_limit;
    if (ccnx_name_from_uri(&msg.name, name_uri) != 0) return CCNX_ERR_URI;
    if (payload && payload_len > 0) {
        memcpy(msg.payload, payload, payload_len);
        msg.payload_len = payload_len;
    }

    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    int len = ccnx_encode(&msg, buf, sizeof(buf));
    if (len < 0) return CCNX_ERR_ARG;

    uint16_t ports[CCNX_MAX_NEXTHOPS];
    int nports = ccnx_fib_lookup(&p->tables, &msg.name, ports, CCNX_MAX_NEXTHOPS);
    if (nports == 0) return CCNX_ERR_NOMATCH;

    uint32_t life = (lifetime_ms == 0) ? CCNX_DEFAULT_LIFETIME_MS : lifetime_ms;
    uint64_t t_now = now_ms();
    uint64_t exp = t_now + life;

    /* PIT にローカル要求として登録(CO 到来時にアプリへ返す径路) */
    ccnx_pit_insert_res_t pr;
    ccnx_pit_insert(&p->tables, &msg.name, CCNX_LOCAL_REQUESTER, exp,
                    t_now, msg.hop_limit, &pr);
    pending_add(p, &msg.name, cb, ctx, exp);

    for (int i = 0; i < nports; i++)
        (void)ccnx_face_send(&p->face, ports[i], buf, (size_t)len);

    return len;
}

int ccnx_publish(ccnx_portal_t *p, const char *name_uri,
                 const uint8_t *payload, uint16_t payload_len,
                 uint32_t freshness_ms) {
    if (!p || !name_uri) return CCNX_ERR_ARG;
    if (payload_len > CCNX_MAX_PAYLOAD) return CCNX_ERR_ARG;

    ccnx_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.packet_type = CCNX_PT_CONTENT_OBJECT;
    if (ccnx_name_from_uri(&msg.name, name_uri) != 0) return CCNX_ERR_URI;
    if (payload && payload_len > 0) {
        memcpy(msg.payload, payload, payload_len);
        msg.payload_len = payload_len;
    }

    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    int len = ccnx_encode(&msg, buf, sizeof(buf));
    if (len < 0) return CCNX_ERR_ARG;

    uint32_t fresh = (freshness_ms == 0) ? CCNX_DEFAULT_FRESHNESS_MS : freshness_ms;
    uint64_t t_now = now_ms();
    ccnx_cs_insert(&p->tables, &msg.name, msg.payload, msg.payload_len,
                   t_now + fresh);

    /* 送出先 = 待機中 PIT の要求元 ∪ FIB nexthop(announce/傍受)。重複排除。 */
    uint16_t dsts[CCNX_MAX_NEXTHOPS * 2];
    int nd = 0;

    int pi = ccnx_pit_find(&p->tables, &msg.name, t_now);
    if (pi >= 0) {
        uint16_t reqs[CCNX_MAX_NEXTHOPS];
        int nr = ccnx_pit_consume(&p->tables, pi, reqs, CCNX_MAX_NEXTHOPS);
        for (int i = 0; i < nr; i++) {
            if (reqs[i] == CCNX_LOCAL_REQUESTER) continue; /* 自分宛は送らない */
            if (nd < (int)(sizeof(dsts)/sizeof(dsts[0]))) dsts[nd++] = reqs[i];
        }
    }
    uint16_t fibp[CCNX_MAX_NEXTHOPS];
    int nf = ccnx_fib_lookup(&p->tables, &msg.name, fibp, CCNX_MAX_NEXTHOPS);
    for (int i = 0; i < nf; i++) {
        int dup = 0;
        for (int j = 0; j < nd; j++) if (dsts[j] == fibp[i]) { dup = 1; break; }
        if (!dup && nd < (int)(sizeof(dsts)/sizeof(dsts[0]))) dsts[nd++] = fibp[i];
    }

    for (int i = 0; i < nd; i++)
        (void)ccnx_face_send(&p->face, dsts[i], buf, (size_t)len);

    return len;
}

/* =====================================================================
 * 受信処理(1 datagram)
 * ===================================================================== */
static void handle_interest(ccnx_portal_t *p, const ccnx_msg_t *msg,
                            uint16_t src_port, const uint8_t *raw, size_t rawlen) {
    uint64_t t_now = now_ms();

    /* 0) RFC 8569 §2.4.1: "If received HopLimit is 0, optionally send Interest Return
     *    (HopLimit Exceeded), and discard Interest."
     *    本デモのローカルアプリは API 直結でワイヤを通らないため、ここに届く Interest は
     *    全て他フォワーダ由来。CS を引く前に破棄する(破棄すべきパケットで CS を引いて
     *    応答するのは条文に反する)。任意の Return は Go 実装と揃えて送出する。 */
    if (msg->hop_limit == 0) {
        if (raw && rawlen >= CCNX_FIXED_HDR_LEN && rawlen <= 0xFFFF) {
            uint8_t ret[CCNX_MAX_DATAGRAM];
            memcpy(ret, raw, rawlen);
            ret[1] = CCNX_PT_RETURN;                      /* PacketType のみ書換え */
            ret[5] = CCNX_T_RETURN_LIMIT_EXCEEDED;        /* ReturnCode = 0x02 */
            ccnx_face_send(&p->face, src_port, ret, rawlen);
        }
        return;
    }

    /* 1) CS ヒットなら即座に満たす(キャッシュ応答)。
     *    §2.4.1: 減算後 HopLimit=0 でも CS からの応答は許される(MAY)。 */
    int ci = ccnx_cs_find(&p->tables, &msg->name, t_now);
    if (ci >= 0) {
        ccnx_msg_t co;
        memset(&co, 0, sizeof(co));
        co.packet_type = CCNX_PT_CONTENT_OBJECT;
        co.name = msg->name;
        co.payload_len = p->tables.cs[ci].payload_len;
        if (co.payload_len > 0)
            memcpy(co.payload, p->tables.cs[ci].payload, co.payload_len);
        uint8_t ob[CCNX_MAX_PAYLOAD + 512];
        int ol = ccnx_encode(&co, ob, sizeof(ob));
        if (ol > 0) (void)ccnx_face_send(&p->face, src_port, ob, (size_t)ol);
        return;
    }

    /* 2) 自登録プレフィックスに一致 = プロデューサ。PIT に載せ on_interest 起動。
     *    §2.4.1: 減算後 HopLimit=0 でもローカルアプリへの配送は許される(MAY)。 */
    int ri = reg_match(p, &msg->name);
    if (ri >= 0) {
        uint64_t exp = t_now + CCNX_DEFAULT_LIFETIME_MS;
        ccnx_pit_insert_res_t pr;
        ccnx_pit_insert(&p->tables, &msg->name, src_port, exp,
                        t_now, msg->hop_limit, &pr);
        if (p->regs[ri].cb)
            p->regs[ri].cb(p, &msg->name,
                           msg->payload_len ? msg->payload : NULL,
                           msg->payload_len, p->regs[ri].ctx);
        return;
    }

    /* 3) 純フォワーダ: FIB 最長一致で転送。
     *    §10.3.1: 到来元を除いた nexthop が空なら「経路なし」として Interest Return を
     *    返す(唯一の経路が送信元向きのときに黒穴化しない)。 */
    if (!ccnx_fib_has_route(&p->tables, &msg->name, src_port)) {
        /* NO_ROUTE: Interest Return を送り返す(WIRE §1.3、バイト長不変) */
        if (raw && rawlen >= CCNX_FIXED_HDR_LEN && rawlen <= 65535) {
            uint8_t ret[CCNX_MAX_PAYLOAD + 512];
            if (rawlen <= sizeof(ret)) {
                memcpy(ret, raw, rawlen);
                ret[1] = CCNX_PT_RETURN;   /* PacketType -> Interest Return */
                ret[5] = 0x01;             /* ReturnCode = T_RETURN_NO_ROUTE */
                (void)ccnx_face_send(&p->face, src_port, ret, rawlen);
            }
        }
        return;
    }

    /* §2.4.1: "If the decremented HopLimit equals 0, the Interest MUST NOT be forwarded
     * to another forwarder." → 他フォワーダへ出せるのは受信 HopLimit >= 2 のときだけ。
     * (受信 1 → 減算後 0 は送出不可。CS 応答とローカル配送は上の 1)2) で済ませてある。) */
    if (msg->hop_limit < 2) return;

    /* Interest 集約(§2.4.2)。ただし以下は集約せず MUST forward:
     *   - 同一前ホップからの再送(retransmit)
     *   - 既存 pending より大きい HopLimit(larger_hop_limit)
     * 失効済み PIT は "valid pending Interest" ではないので集約対象にしない(W-3)。 */
    uint64_t exp = t_now + CCNX_DEFAULT_LIFETIME_MS;
    ccnx_pit_insert_res_t pr;
    ccnx_pit_insert(&p->tables, &msg->name, src_port, exp,
                    t_now, msg->hop_limit, &pr);
    if (pr.aggregated && !pr.retransmit && !pr.larger_hop_limit) return;

    /* HopLimit 減算して nexthop へ転送(src へは戻さない=split horizon) */
    ccnx_msg_t fwd = *msg;
    fwd.hop_limit = (uint8_t)(msg->hop_limit - 1);
    uint8_t fb[CCNX_MAX_PAYLOAD + 512];
    int fl = ccnx_encode(&fwd, fb, sizeof(fb));
    if (fl < 0) return;

    uint16_t ports[CCNX_MAX_NEXTHOPS];
    int nports = ccnx_fib_lookup(&p->tables, &msg->name, ports, CCNX_MAX_NEXTHOPS);
    for (int i = 0; i < nports; i++) {
        if (ports[i] == src_port) continue;
        (void)ccnx_face_send(&p->face, ports[i], fb, (size_t)fl);
    }
}

static void handle_content(ccnx_portal_t *p, const ccnx_msg_t *msg, uint16_t src_port) {
    uint64_t t_now = now_ms();

    /* RFC 8569 §2.4.5(1)(RECOMMENDED)前ホップ検査。
     * 本実装は §2.4.5(4) から意図的に逸脱して無条件キャッシュしているため、その補償制御
     * として「FIB に nexthop として掲載されているポート(= Interest を送り出しうる face)」
     * 以外から来た CO は信用しない。信用しない CO は
     *   - CS に入れない(汚染防止)
     *   - PIT を満たさない/下流へ転送しない(偽装応答の注入防止)
     * とし、アプリ層の非請求 announce 径路(overheard)にだけ渡す。 */
    int from_fib_nexthop = ccnx_fib_is_nexthop(&p->tables, src_port);

    /* CS 格納(傍受キャッシュ。二重取り回避の基盤) */
    if (from_fib_nexthop)
        ccnx_cs_insert(&p->tables, &msg->name, msg->payload, msg->payload_len,
                       t_now + CCNX_DEFAULT_FRESHNESS_MS);

    int pi = from_fib_nexthop ? ccnx_pit_find(&p->tables, &msg->name, t_now) : -1;
    if (pi >= 0) {
        uint16_t reqs[CCNX_MAX_NEXTHOPS];
        int nr = ccnx_pit_consume(&p->tables, pi, reqs, CCNX_MAX_NEXTHOPS);
        int delivered_local = 0;
        uint8_t ob[CCNX_MAX_PAYLOAD + 512];
        int ol = -1;
        for (int i = 0; i < nr; i++) {
            if (reqs[i] == CCNX_LOCAL_REQUESTER) {
                /* 自 express の応答: pending コールバックへ */
                ccnx_on_content_fn cb = NULL;
                void *ctx = NULL;
                if (pending_take(p, &msg->name, &cb, &ctx)) {
                    if (cb) cb(p, &msg->name,
                               msg->payload_len ? msg->payload : NULL,
                               msg->payload_len, 0 /*not overheard*/, ctx);
                    else if (p->default_on_content)
                        p->default_on_content(p, &msg->name,
                               msg->payload_len ? msg->payload : NULL,
                               msg->payload_len, 0, p->default_on_content_ctx);
                }
                delivered_local = 1;
            } else {
                /* 下流要求元へ CO を返送 */
                if (ol < 0) ol = ccnx_encode(msg, ob, sizeof(ob));
                if (ol > 0) (void)ccnx_face_send(&p->face, reqs[i], ob, (size_t)ol);
            }
        }
        (void)delivered_local;
        return;
    }

    /* PIT 不一致(または前ホップ非信頼)= 傍受(overhear)。
     * 選抜 accept / star claim / bin moved など非請求 announce の監視径路。 */
    if (p->default_on_content)
        p->default_on_content(p, &msg->name,
                              msg->payload_len ? msg->payload : NULL,
                              msg->payload_len, 1 /*overheard*/,
                              p->default_on_content_ctx);
}

/* =====================================================================
 * 実行ループ
 * ===================================================================== */
int ccnx_tick(ccnx_portal_t *p, uint32_t timeout_ms) {
    if (!p) return CCNX_ERR_ARG;

    int processed = 0;
    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    uint16_t src_port = 0;

    /* 最初の1個は timeout まで待つ。以降は非ブロッキングで在るだけ捌く */
    uint32_t to = timeout_ms;
    for (;;) {
        int n = ccnx_face_recv(&p->face, to, buf, sizeof(buf), &src_port);
        if (n <= 0) break;             /* 0=データ無 / <0=エラー */
        to = 0;                        /* 2 個目以降は待たない */

        ccnx_msg_t msg;
        int dl = ccnx_decode(buf, (size_t)n, &msg);
        if (dl < 0) continue;          /* 不正 8609 はスキップ(A層はここで死なない) */
        processed++;

        if (msg.packet_type == CCNX_PT_INTEREST)
            handle_interest(p, &msg, src_port, buf, (size_t)n);
        else if (msg.packet_type == CCNX_PT_CONTENT_OBJECT)
            handle_content(p, &msg, src_port);
        /* Interest Return(0x02)は v1 では上位へ通知せず握り潰す */
    }

    /* 期限切れ掃除(PIT/CS/pending) */
    uint64_t t = now_ms();
    ccnx_pit_expire(&p->tables, t);
    ccnx_cs_expire(&p->tables, t);
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++)
        if (p->pending[i].in_use && t >= p->pending[i].expire_ms)
            p->pending[i].in_use = 0;

    return processed;
}

int ccnx_run(ccnx_portal_t *p) {
    if (!p) return CCNX_ERR_ARG;
    p->running = 1;
    while (p->running) {
        int r = ccnx_tick(p, 100);
        if (r < 0) return r;
    }
    return CCNX_OK;
}

void ccnx_stop(ccnx_portal_t *p) {
    if (p) p->running = 0;
}

/* =====================================================================
 * 内観
 * ===================================================================== */
int ccnx_pit_count(const ccnx_portal_t *p) {
    return p ? ccnx_tbl_pit_count(&p->tables) : 0;
}
int ccnx_fib_count(const ccnx_portal_t *p) {
    if (!p) return 0;
    int c = 0;
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++)
        if (p->tables.fib[i].in_use) c++;
    return c;
}
int ccnx_cs_count(const ccnx_portal_t *p) {
    return p ? ccnx_tbl_cs_count(&p->tables) : 0;
}
uint16_t ccnx_portal_port(const ccnx_portal_t *p) {
    return p ? p->face.listen_port : 0;
}
