/*
 * ccnx_tables.c - PIT / FIB / CS 実装
 *
 * B-283 A層。ccnx_name_t 鍵の純データ構造。UDP を知らない(DESIGN §3)。
 */
#include "ccnx_tables.h"
#include <string.h>

void ccnx_tables_init(ccnx_tables_t *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
}

/* =====================================================================
 * FIB
 * ===================================================================== */
static int fib_find_exact(const ccnx_tables_t *t, const ccnx_name_t *prefix) {
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++)
        if (t->fib[i].in_use && ccnx_name_equal(&t->fib[i].prefix, prefix))
            return i;
    return -1;
}

int ccnx_fib_add(ccnx_tables_t *t, const ccnx_name_t *prefix, uint16_t nexthop_port) {
    if (!t || !prefix) return CCNX_ERR_ARG;
    int idx = fib_find_exact(t, prefix);
    if (idx < 0) {
        for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++) {
            if (!t->fib[i].in_use) { idx = i; break; }
        }
        if (idx < 0) return CCNX_ERR_FULL;
        t->fib[idx].in_use = 1;
        t->fib[idx].prefix = *prefix;
        t->fib[idx].n_nexthops = 0;
    }
    ccnx_fib_entry_t *e = &t->fib[idx];
    for (int j = 0; j < e->n_nexthops; j++)
        if (e->nexthops[j] == nexthop_port) return CCNX_OK; /* 重複は無視 */
    if (e->n_nexthops >= CCNX_MAX_NEXTHOPS) return CCNX_ERR_FULL;
    e->nexthops[e->n_nexthops++] = nexthop_port;
    return CCNX_OK;
}

int ccnx_fib_lookup(const ccnx_tables_t *t, const ccnx_name_t *name,
                    uint16_t *out_ports, int max_ports) {
    if (!t || !name || !out_ports || max_ports <= 0) return 0;
    int best = -1;
    int best_len = -1;
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++) {
        if (!t->fib[i].in_use) continue;
        if (!ccnx_name_is_prefix(&t->fib[i].prefix, name)) continue;
        int plen = t->fib[i].prefix.seg_count;   /* 最長一致 = seg_count 最大 */
        if (plen > best_len) { best_len = plen; best = i; }
    }
    if (best < 0) return 0;
    const ccnx_fib_entry_t *e = &t->fib[best];
    int n = e->n_nexthops < max_ports ? e->n_nexthops : max_ports;
    for (int j = 0; j < n; j++) out_ports[j] = e->nexthops[j];
    return n;
}

/* RFC 8569 §10.3.1: 経路の有無は「実際に転送に使う集合(=最長一致 nexthop)から
 * 到来元を除いてなお非空か」で判断する。旧実装は前方一致するエントリの存在だけを見て
 * いたため、唯一の nexthop が Interest の到来元だった場合に Interest Return を返さず
 * 黒穴化していた(E-3)。lookup 結果に一本化してこれを解消する。 */
int ccnx_fib_has_route(const ccnx_tables_t *t, const ccnx_name_t *name,
                       uint16_t exclude_port) {
    if (!t || !name) return 0;
    uint16_t ports[CCNX_MAX_NEXTHOPS];
    int n = ccnx_fib_lookup(t, name, ports, CCNX_MAX_NEXTHOPS);
    for (int i = 0; i < n; i++)
        if (ports[i] != exclude_port) return 1;
    return 0;
}

/* RFC 8569 §2.4.5(1) の前ホップ検査用: port が FIB の nexthop に掲載されているか。 */
int ccnx_fib_is_nexthop(const ccnx_tables_t *t, uint16_t port) {
    if (!t) return 0;
    for (int i = 0; i < CCNX_MAX_FIB_ENTRIES; i++) {
        if (!t->fib[i].in_use) continue;
        for (int j = 0; j < t->fib[i].n_nexthops; j++)
            if (t->fib[i].nexthops[j] == port) return 1;
    }
    return 0;
}

/* =====================================================================
 * PIT
 * ===================================================================== */
int ccnx_pit_insert(ccnx_tables_t *t, const ccnx_name_t *name,
                    uint16_t requester_port, uint64_t expire_ms,
                    uint64_t now_ms, uint8_t hop_limit,
                    ccnx_pit_insert_res_t *res) {
    if (res) {
        res->index = -1;
        res->aggregated = 0;
        res->retransmit = 0;
        res->larger_hop_limit = 0;
    }
    if (!t || !name) return CCNX_ERR_ARG;

    /* 有効(未失効)な同名 PIT が在れば統合(Interest 集約, RFC 8569 §2.4.2)。
     * 失効済みエントリは "valid pending Interest" ではないので不在扱いし、
     * ここで遅延削除する(W-3)。 */
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (!t->pit[i].in_use) continue;
        if (!ccnx_name_equal(&t->pit[i].name, name)) continue;
        if (now_ms >= t->pit[i].expire_ms) { t->pit[i].in_use = 0; continue; }
        ccnx_pit_entry_t *e = &t->pit[i];
        if (res) {
            res->index = i;
            res->aggregated = 1;
            /* §2.4.2: 既存 pending より大きい HopLimit なら MUST forward(V-3) */
            if (hop_limit > e->max_hop_limit) res->larger_hop_limit = 1;
        }
        if (expire_ms > e->expire_ms) e->expire_ms = expire_ms;
        if (hop_limit > e->max_hop_limit) e->max_hop_limit = hop_limit;
        for (int j = 0; j < e->n_requesters; j++) {
            if (e->requesters[j] == requester_port) {
                /* §2.4.2: 同一前ホップからの2回目以降の受信は MUST forward(V-2) */
                if (res) res->retransmit = 1;
                return i;
            }
        }
        if (e->n_requesters < CCNX_MAX_NEXTHOPS)
            e->requesters[e->n_requesters++] = requester_port;
        return i;
    }
    /* 新規 */
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (t->pit[i].in_use) continue;
        ccnx_pit_entry_t *e = &t->pit[i];
        e->in_use = 1;
        e->name = *name;
        e->n_requesters = 1;
        e->requesters[0] = requester_port;
        e->expire_ms = expire_ms;
        e->max_hop_limit = hop_limit;
        if (res) res->index = i;
        return i;
    }
    return CCNX_ERR_FULL;
}

int ccnx_pit_find(const ccnx_tables_t *t, const ccnx_name_t *name, uint64_t now_ms) {
    if (!t || !name) return -1;
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (!t->pit[i].in_use) continue;
        if (now_ms >= t->pit[i].expire_ms) continue;   /* 失効は不在扱い(W-3) */
        if (ccnx_name_equal(&t->pit[i].name, name)) return i;
    }
    return -1;
}

int ccnx_pit_consume(ccnx_tables_t *t, int index,
                     uint16_t *out_requesters, int max) {
    if (!t || index < 0 || index >= CCNX_MAX_PIT_ENTRIES) return CCNX_ERR_ARG;
    ccnx_pit_entry_t *e = &t->pit[index];
    if (!e->in_use) return CCNX_ERR_ARG;
    int n = e->n_requesters;
    if (out_requesters && max > 0) {
        int c = n < max ? n : max;
        for (int j = 0; j < c; j++) out_requesters[j] = e->requesters[j];
        n = c;
    }
    e->in_use = 0;
    return n;
}

int ccnx_pit_expire(ccnx_tables_t *t, uint64_t now_ms) {
    if (!t) return 0;
    int cleared = 0;
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++) {
        if (t->pit[i].in_use && now_ms >= t->pit[i].expire_ms) {
            t->pit[i].in_use = 0;
            cleared++;
        }
    }
    return cleared;
}

int ccnx_tbl_pit_count(const ccnx_tables_t *t) {
    if (!t) return 0;
    int c = 0;
    for (int i = 0; i < CCNX_MAX_PIT_ENTRIES; i++)
        if (t->pit[i].in_use) c++;
    return c;
}

/* =====================================================================
 * CS
 * ===================================================================== */
int ccnx_cs_insert(ccnx_tables_t *t, const ccnx_name_t *name,
                   const uint8_t *payload, uint16_t payload_len,
                   uint64_t expire_ms) {
    if (!t || !name) return CCNX_ERR_ARG;
    if (payload_len > CCNX_MAX_PAYLOAD) return CCNX_ERR_ARG;

    int idx = -1;
    /* 同名は上書き */
    for (int i = 0; i < CCNX_MAX_CS_ENTRIES; i++) {
        if (t->cs[i].in_use && ccnx_name_equal(&t->cs[i].name, name)) { idx = i; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < CCNX_MAX_CS_ENTRIES; i++)
            if (!t->cs[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return CCNX_ERR_FULL;

    ccnx_cs_entry_t *e = &t->cs[idx];
    e->in_use = 1;
    e->name = *name;
    e->payload_len = payload_len;
    if (payload_len > 0 && payload)
        memcpy(e->payload, payload, payload_len);
    e->expire_ms = expire_ms;
    return CCNX_OK;
}

int ccnx_cs_find(const ccnx_tables_t *t, const ccnx_name_t *name, uint64_t now_ms) {
    if (!t || !name) return -1;
    for (int i = 0; i < CCNX_MAX_CS_ENTRIES; i++) {
        if (!t->cs[i].in_use) continue;
        if (now_ms >= t->cs[i].expire_ms) continue;   /* 失効は無視 */
        if (ccnx_name_equal(&t->cs[i].name, name)) return i;
    }
    return -1;
}

int ccnx_cs_expire(ccnx_tables_t *t, uint64_t now_ms) {
    if (!t) return 0;
    int cleared = 0;
    for (int i = 0; i < CCNX_MAX_CS_ENTRIES; i++) {
        if (t->cs[i].in_use && now_ms >= t->cs[i].expire_ms) {
            t->cs[i].in_use = 0;
            cleared++;
        }
    }
    return cleared;
}

int ccnx_tbl_cs_count(const ccnx_tables_t *t) {
    if (!t) return 0;
    int c = 0;
    for (int i = 0; i < CCNX_MAX_CS_ENTRIES; i++)
        if (t->cs[i].in_use) c++;
    return c;
}
