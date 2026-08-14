/*
 * state_emit.h - B層: 状態 JSON を stdout に出す(PROTOCOL.md §2.1)。
 *
 * B-283 CCNx セマンティック群ロボ通信デモ / C B層。
 * robot.c が 1 反復ごとにスナップショット(state_view_t)を渡す。本モジュールは
 *   - 離散フィールド(state/partner/carrying/counts)が変化した
 *   - event が非空
 *   - 前回 emit から 500ms 経過(ハートビート)
 * のいずれかで 1 行 JSON を stdout に書く(改行区切り, UTF-8)。
 * ★stdout には state 行しか出さない(PROTOCOL §5.1)。デバッグは stderr へ。
 * behavior.c の内部型に依存しない(view 経由で疎結合)。
 */
#ifndef ROBOT_STATE_EMIT_H
#define ROBOT_STATE_EMIT_H

typedef struct {
    int          id;
    long long    t;          /* emit 時刻: Unix epoch ms(PROTOCOL §0) */
    double       x, y;       /* pos(m) */
    const char  *zone;       /* 担当ゾーン。未割当は "" */
    const char  *state;      /* PROTOCOL §2 の綴り */
    int          partner_id; /* 無ければ -1 */
    int          casualty_id;/* carrier が搬送中の救助対象ロボ id。無ければ -1(改修1) */
    int          pit_count, fib_count, cs_count;
    int          carrying;   /* 0/1 -> false/true */
    const char  *event;      /* PROTOCOL §2.2 の値集合。無ければ "" */
} state_view_t;

typedef struct {
    long long last_emit_t;
    int       have_last;
    char      last_state[24];
    int       last_partner;
    int       last_casualty;
    int       last_carrying;
    int       last_pit, last_fib, last_cs;
    double    last_x, last_y;   /* 直近 emit した位置(移動検出用) */
} state_emitter_t;

/* 静止時のハートビート。移動中は下の 2 定数で高頻度化して描画を滑らかに保つ。 */
#define STATE_HEARTBEAT_MS 500
/* 移動中の最小 emit 間隔(ms)。ロボの主ループは 50ms tick(20Hz)なので、これを
   tick 未満にして「移動中は毎 tick 送る」= 実効 20Hz。8体×20Hz=160行/s で軽量。
   ブラウザは 60fps rAF でこのサンプル間を補間し 1px 単位で滑らかに描く。 */
#define STATE_MOVE_EMIT_MS 40
/* この距離(m)以上動いたら「移動中」とみなし位置を送る。静止時の氾濫を防ぐ。 */
#define STATE_MOVE_EPS_M   0.03

void state_emitter_init(state_emitter_t *se);

/* 条件成立時に 1 行 JSON を stdout に書く。書いたら 1、抑制したら 0 を返す。 */
int  state_emit(state_emitter_t *se, const state_view_t *v);

#endif /* ROBOT_STATE_EMIT_H */
