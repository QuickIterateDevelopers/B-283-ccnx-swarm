/*
 * robot.c - B層 ロボプロセス main → 実行ファイル swarm_robot。
 *
 * B-283 CCNx セマンティック群ロボ通信デモ / C B層。
 * A層 libccnx.a(ccnx.h)をリンクし、その API だけを経由する(DESIGN §5)。
 *
 * 起動引数(PROTOCOL §5.3, 順序自由・キー付き):
 *   swarm_robot --id <i> --port <9200+i> --tap <9300> \
 *               --peers <9200,9201,...> --zone <A-2> --x <f> --y <f> \
 *               --field 40x30 --n <N>
 *
 * ループ: 実クロックで dt 算出 → ×倍率で sim_dt →
 *         ccnx_tick(受信/コールバック) → robot_update(挙動) → 状態 emit。
 * SIGTERM は既定動作(即終了)で受ける = 故障注入(PROTOCOL §5.1)。ハンドラは置かない。
 *
 * stdout には状態 JSON 行のみ(PROTOCOL §5.1)。デバッグは stderr。
 */
#include "ccnx.h"
#include "behavior.h"
#include "movement.h"
#include "state_emit.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_TIMEOUT_MS   50      /* 20Hz(位置更新周期, PROTOCOL §2) */

/* 救助対象化シグナル(改修1): SIGUSR1 で「停止・生存(NEEDS_RESCUE)」に入る。
   SIGTERM(故障=即死)とは別系統。ハンドラはフラグを立てるだけ(async-signal-safe)。 */
static volatile sig_atomic_t g_needs_rescue = 0;
static void on_sigusr1(int sig) { (void)sig; g_needs_rescue = 1; }

static double g_time_scale = 3.0; /* デモ時間倍率 ×3(実時間トグルは env で 1.0) */

static double now_mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static long long now_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* "40x30" → w,h */
static void parse_field(const char *s, double *w, double *h) {
    double a = 40, b = 30;
    if (s) sscanf(s, "%lfx%lf", &a, &b);
    *w = a; *h = b;
}

/* "9200,9201,..." → peers[] */
static int parse_peers(const char *s, uint16_t *peers, int cap) {
    int n = 0;
    if (!s) return 0;
    const char *p = s;
    while (*p && n < cap) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v > 0 && v < 65536) peers[n++] = (uint16_t)v;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return n;
}

static const char *arg_val(int argc, char **argv, const char *key) {
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    const char *s_id    = arg_val(argc, argv, "--id");
    const char *s_port  = arg_val(argc, argv, "--port");
    const char *s_tap   = arg_val(argc, argv, "--tap");
    const char *s_peers = arg_val(argc, argv, "--peers");
    const char *s_zone  = arg_val(argc, argv, "--zone");
    const char *s_x     = arg_val(argc, argv, "--x");
    const char *s_y     = arg_val(argc, argv, "--y");
    const char *s_field = arg_val(argc, argv, "--field");
    const char *s_n     = arg_val(argc, argv, "--n");

    if (!s_id || !s_port) {
        fprintf(stderr,
            "usage: swarm_robot --id <i> --port <p> --tap <t> --peers <l> "
            "--zone <z> --x <f> --y <f> --field 40x30 --n <N>\n");
        return 2;
    }

    int      id   = atoi(s_id);
    uint16_t port = (uint16_t)atoi(s_port);
    uint16_t tap  = (uint16_t)(s_tap ? atoi(s_tap) : 0);
    int      n    = s_n ? atoi(s_n) : 1;
    double   x    = s_x ? atof(s_x) : 0.0;
    double   y    = s_y ? atof(s_y) : 0.0;
    double   fw, fh; parse_field(s_field, &fw, &fh);
    const char *zone = s_zone ? s_zone : "";

    uint16_t peers[ROBOT_MAX_PEERS];
    int n_peers = parse_peers(s_peers, peers, ROBOT_MAX_PEERS);

    const char *env_scale = getenv("ROBOT_TIME_SCALE");
    if (env_scale) {
        double sc = atof(env_scale);
        if (sc > 0) g_time_scale = sc;
    }

    ccnx_portal_t *portal = ccnx_portal_open("127.0.0.1", port, tap);
    if (!portal) {
        fprintf(stderr, "robot %d: ccnx_portal_open(:%u) failed\n", id, port);
        return 1;
    }

    /* 星の位置表をロード(supervisor 生成の共有ファイル。全ロボ同一で位置一致)。 */
    const char *s_stars = arg_val(argc, argv, "--stars");
    int nstar = behavior_load_stars(s_stars);
    if (nstar < 0)
        fprintf(stderr, "robot %d: stars ロード失敗(%s)\n", id, s_stars ? s_stars : "(none)");
    else
        fprintf(stderr, "robot %d: stars %d 個ロード\n", id, nstar);

    robot_t robot;
    robot_init(&robot, portal, id, zone, x, y, fw, fh, n, peers, n_peers, port);
    robot.time_scale = g_time_scale;   /* focus 指令の実秒→sim 秒換算に使う */
    if (robot_register(&robot) < 0) {
        fprintf(stderr, "robot %d: robot_register failed\n", id);
        ccnx_portal_close(portal);
        return 1;
    }

    /* SIGUSR1 = 救助対象化(停止・生存)。SIGTERM は既定(即死=故障)のまま触らない。 */
    signal(SIGUSR1, on_sigusr1);

    state_emitter_t emitter;
    state_emitter_init(&emitter);

    /* 起動 emit(spawn) */
    robot.pending_event = "spawn";

    double last = now_mono_s();
    for (;;) {
        double t0 = now_mono_s();
        double real_dt = t0 - last;
        last = t0;
        if (real_dt < 0) real_dt = 0;
        if (real_dt > 0.5) real_dt = 0.5;      /* 大ギャップは丸める */
        double sim_dt = real_dt * g_time_scale;

        robot.sim_now_s += sim_dt;

        /* A層: 受信 → decode → PIT/FIB/CS → コールバック(on_interest/on_content) */
        ccnx_tick(portal, TICK_TIMEOUT_MS);

        /* 救助対象化シグナル(SIGUSR1)を検知 → 停止(NEEDS_RESCUE)。 */
        if (g_needs_rescue) {
            g_needs_rescue = 0;
            robot_become_casualty(&robot);
        }

        /* B層: 挙動 1 反復 */
        robot_update(&robot, sim_dt);

        /* 状態 emit(on-change + 500ms ハートビート) */
        state_view_t v;
        v.id         = robot.id;
        v.t          = now_wall_ms();
        v.x          = robot.mv.pos.x;
        v.y          = robot.mv.pos.y;
        v.zone       = robot.zone;
        v.state      = robot_state_name(robot.state);
        v.partner_id = robot.partner_id;
        v.casualty_id = robot.task_casualty_id; /* carrier が搬送中の救助対象 id(改修1) */
        v.pit_count  = ccnx_pit_count(portal);
        v.fib_count  = ccnx_fib_count(portal);
        v.cs_count   = ccnx_cs_count(portal);
        v.carrying   = robot.carrying;
        v.event      = robot.pending_event ? robot.pending_event : "";

        if (state_emit(&emitter, &v))
            robot.pending_event = "";          /* イベントは 1 回だけ */
    }

    /* 到達しない(SIGTERM 即死)。到達路のため後始末を残す。 */
    ccnx_portal_close(portal);
    return 0;
}
