/*
 * state_emit.c - state_emit.h の実装。
 * PROTOCOL §2.1 のキー順・型どおりに 1 行 JSON を組んで stdout へ。
 */
#include "state_emit.h"
#include <stdio.h>
#include <string.h>

void state_emitter_init(state_emitter_t *se) {
    se->last_emit_t = 0;
    se->have_last = 0;
    se->last_state[0] = '\0';
    se->last_partner = -2;   /* 初回必ず「変化」扱いにする番兵 */
    se->last_casualty = -2;
    se->last_carrying = -1;
    se->last_pit = se->last_fib = se->last_cs = -1;
    se->last_x = se->last_y = 0.0;
}

static int discrete_changed(const state_emitter_t *se, const state_view_t *v) {
    if (!se->have_last) return 1;
    if (strncmp(se->last_state, v->state, sizeof se->last_state) != 0) return 1;
    if (se->last_partner  != v->partner_id) return 1;
    if (se->last_casualty != v->casualty_id) return 1;
    if (se->last_carrying != (v->carrying ? 1 : 0)) return 1;
    if (se->last_pit != v->pit_count) return 1;
    if (se->last_fib != v->fib_count) return 1;
    if (se->last_cs  != v->cs_count)  return 1;
    return 0;
}

int state_emit(state_emitter_t *se, const state_view_t *v) {
    const char *event = v->event ? v->event : "";
    int has_event = event[0] != '\0';
    int changed = discrete_changed(se, v);
    long long dt = v->t - se->last_emit_t;
    int heartbeat = !se->have_last || dt >= STATE_HEARTBEAT_MS || dt < 0;

    /* 移動中は位置を高頻度に送り描画を連続化する:
       前回 emit から EPS 以上動き、かつ MOVE_EMIT_MS 以上経っていれば送る。
       静止中は動かないので発火せず、ハートビート(500ms)に落ちる。 */
    double mdx = v->x - se->last_x, mdy = v->y - se->last_y;
    int moved = se->have_last
             && (mdx*mdx + mdy*mdy) >= (STATE_MOVE_EPS_M * STATE_MOVE_EPS_M)
             && dt >= STATE_MOVE_EMIT_MS;

    if (!has_event && !changed && !heartbeat && !moved) return 0;

    const char *zone = v->zone ? v->zone : "";
    /* PROTOCOL §2.1 のキー順を厳密に踏襲(pos は object, carrying は bool) */
    printf("{\"type\":\"state\",\"id\":%d,\"t\":%lld,"
           "\"pos\":{\"x\":%.2f,\"y\":%.2f},\"zone\":\"%s\","
           "\"state\":\"%s\",\"partner_id\":%d,\"casualty_id\":%d,"
           "\"pit_count\":%d,\"fib_count\":%d,\"cs_count\":%d,"
           "\"carrying\":%s,\"event\":\"%s\"}\n",
           v->id, v->t,
           v->x, v->y, zone,
           v->state, v->partner_id, v->casualty_id,
           v->pit_count, v->fib_count, v->cs_count,
           v->carrying ? "true" : "false", event);
    fflush(stdout);

    /* 直近スナップショットを更新 */
    strncpy(se->last_state, v->state, sizeof se->last_state - 1);
    se->last_state[sizeof se->last_state - 1] = '\0';
    se->last_partner = v->partner_id;
    se->last_casualty = v->casualty_id;
    se->last_carrying = v->carrying ? 1 : 0;
    se->last_pit = v->pit_count;
    se->last_fib = v->fib_count;
    se->last_cs = v->cs_count;
    se->last_x = v->x;
    se->last_y = v->y;
    se->last_emit_t = v->t;
    se->have_last = 1;
    return 1;
}
