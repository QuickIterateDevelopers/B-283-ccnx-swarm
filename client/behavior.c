/*
 * behavior.c - B層 ロボ挙動の実装(状態機械 + CCNx 相互作用)。
 *
 * A層は ccnx.h の関数のみ経由(codec/tables/transport を直接触らない, DESIGN §5)。
 * 名前空間: PROTOCOL §1 / payload: §1.1 / 状態機械: §2 / event: §2.2。
 *
 * ★世界レイアウト(ゾーン/病院/回収箱/星)は本デモの固定盤面。
 *   Go supervisor が browser へ流す topology(PROTOCOL §3.1)は本テーブルと
 *   同一座標でなければ Canvas 描画と C の座標が食い違う。両者はこの定数に収束させる。
 */
#include "behavior.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ===================================================================
 * 調停パラメータ(PROTOCOL §2 の時定数)
 * =================================================================== */
#define STAR_RADIUS_M        1.0    /* 探知円 = 半径1.0m(マス2mの1/2)。徘徊中に星を拾いやすく */
#define RESCUE_BACKOFF_MAX_MS 2000.0 /* 距離比例バックオフ上限 */
#define LOAD_STAR_S          1.0    /* STAR_LOADING / UNLOADING */
#define LOAD_COOP_S           3.0    /* COOP_LOADING / UNLOADING */
#define POSE_INTERVAL_S       0.5    /* 搬送中の pose announce 周期(担ぎ手2機のみ高頻度) */
#define POSE_TIMEOUT_S        2.0    /* この間 partner pose 途絶 → 相方 lost */
#define RECRUIT_RETRY_S       2.0    /* recruit Interest 再送周期 */
#define RECRUIT_FAIL_S        15.0   /* 補充できなければ FAILED */
#define CLAIM_TTL_S            2.5    /* 占有の寿命。放置された占有は速やかに解放(18台で詰まらせない) */
#define SYNC_SETTLE_S          0.4    /* 到着後、present(到着順)が固まるまでの待ち。ほぼ同時到着でも順位を安定させる */
#define REPAIR_WAIT_S          15.0   /* 病院に置き去り→復活までの待ち(sim15秒 = 実時間約5秒@3倍速) */
#define TEAM_SEP_M             1.4    /* 同チーム分離距離 = 0.7マス(マス2m)。これ未満なら少しずつ離れる */
#define TEAM_SEP_PUSH_MPS      0.9    /* 分離の押し出し速度(弱め。到着や星拾いを妨げない) */
#define TEAM_SEP_GOAL_GUARD_M  1.5    /* 目標がこの距離より近い時は分離しない(到着優先) */
#define POSE_BEACON_S          6.0    /* 位置ビーコン周期(sim秒 ≒ 実2秒)。分離用途なので粗くて良い */
#define STRETCHER_LEN_M        1.8    /* 担架の長さ。搬送中の頭側-足側の距離 */
#define BIN_FAR_M              10.0   /* 箱と残存星群の重心がこれ以上離れたら箱を引っ越す */
#define BIN_MOVE_COOLDOWN_S    30.0   /* 箱の引っ越しクールダウン(sim秒) */
#define FAIL_COOLDOWN_S        240.0  /* 復活後この間は自動故障の抽選対象外(同じ子の連続故障=偏り見えを防ぐ) */

/* event 値(PROTOCOL §2.2。これ以外を出さない) */
#define EV_NONE        ""
#define EV_SPAWN       "spawn"
#define EV_PATROL      "patrol"
#define EV_STAR_CLAIM "star_claim"
#define EV_STAR_PICK  "star_pickup"
#define EV_STAR_DROP  "star_dropoff"
#define EV_BID_START   "bid_start"
#define EV_ACCEPT_SENT "accept_sent"
#define EV_SELECTED    "selected"
#define EV_WITHDRAWN   "withdrawn"
#define EV_PAIR_FORMED "pair_formed"
#define EV_PICKUP_ARR  "pickup_arrive"
#define EV_SYNC_WAIT   "sync_wait"
#define EV_COOP_LOAD   "coop_load"
#define EV_COOP_DROP   "coop_dropoff"
#define EV_PARTNER_LOST "partner_lost"
#define EV_RECRUITING  "recruiting"
#define EV_RECRUITED   "recruited"
#define EV_FAILED      "failed"
#define EV_NEEDS_RESCUE "needs_rescue"   /* 救助対象化(停止) */
#define EV_REPAIRED    "repaired"        /* 病院搬送完了で復活 */
#define EV_GOING_HOME  "going_home"      /* おうちに戻れ */

/* ===================================================================
 * 固定盤面(40m×30m, PROTOCOL §0/§3.1)
 * 改修1: 患者はゾーン中心ではなく「止まった故障ロボ(casualty)」の実座標。
 *        ゾーンは /area/{zone} 登録と topology 描画にのみ使い、救助位置には使わない。
 * =================================================================== */

static const char  *HOSPITAL_ID = "hospital-1";
static const vec2_t HOSPITAL     = {20.0, 15.0};   /* フィールド中央 */
static const vec2_t HOME         = {1.5, 1.5};   /* ControlCenter(おうち)。browser ctlStation と一致 */

/* ★ supervisor.go の topology.bins と同一座標(3個)に収束させること。
   食い違うと「画面に星置き場が無い所へ下ろす」不整合になる。 */
static const vec2_t BINS[] = { {2.0, 28.0}, {38.0, 28.0}, {38.0, 2.0} };   /* 赤=左下 青=右下 緑=右上 */
static const int    N_BINS = (int)(sizeof BINS / sizeof BINS[0]);

/* 星の位置表。supervisor 生成の stars ファイルから起動時にロードする。
   添字 = 星 id(/star/{id}/claim と対応)。未ロード時は 0 個。 */
static vec2_t STAR[ROBOT_MAX_STAR];
static int    N_STAR = 0;

int behavior_load_stars(const char *path) {
    if (!path) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int id, maxid = -1; double x, y;
    while (fscanf(fp, "%d %lf %lf", &id, &x, &y) == 3) {
        if (id < 0 || id >= ROBOT_MAX_STAR) continue;   /* 範囲外は捨てる */
        STAR[id].x = x; STAR[id].y = y;
        if (id > maxid) maxid = id;
    }
    fclose(fp);
    N_STAR = maxid + 1;   /* id は 0..maxid の連番前提 */
    return N_STAR;
}

/* ===================================================================
 * 小ユーティリティ
 * =================================================================== */
static const char *STATE_NAMES[ST_NSTATES] = {
    "IDLE", "PATROL", "STAR_CLAIMING", "STAR_ENROUTE", "STAR_LOADING",
    "STAR_CARRYING", "STAR_UNLOADING", "BIDDING", "SELECTED", "PAIRING",
    "ENROUTE_PICKUP", "SYNC_WAIT", "COOP_LOADING", "COOP_CARRYING",
    "COOP_UNLOADING", "RECRUITING", "WITHDRAWN", "FAILED", "NEEDS_RESCUE",
    "REPAIRING", "GOING_HOME", "BIN_CARRYING",
};

const char *robot_state_name(robot_state_t s) {
    if (s < 0 || s >= ST_NSTATES) return "IDLE";
    return STATE_NAMES[s];
}

static long long wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 簡易 xorshift(徘徊/ジッタ。id 毎に独立系列) */
static unsigned rng_next(unsigned *s) {
    unsigned x = *s ? *s : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static double rng_unit(unsigned *s) { return (rng_next(s) & 0xFFFFFF) / (double)0x1000000; }

static void set_state(robot_t *r, robot_state_t st, const char *ev) {
    r->state = st;
    r->state_since_s = r->sim_now_s;
    if (ev && ev[0]) r->pending_event = ev;
}

static void new_wander_target(robot_t *r);   /* 前方宣言(on_content の復活処理で使用) */

static int is_committed(robot_state_t s) {
    /* 救助に既に従事中(=新 rescue の入札に参加しない, 搬送中入札不参加) */
    return s == ST_SELECTED || s == ST_PAIRING || s == ST_ENROUTE_PICKUP ||
           s == ST_SYNC_WAIT || s == ST_COOP_LOADING || s == ST_COOP_CARRYING ||
           s == ST_COOP_UNLOADING || s == ST_RECRUITING;
}

/* ===================================================================
 * 盤面ルックアップ
 * =================================================================== */
/* チーム = id/3 の連番グループ(#0-2=赤 / #3-5=青 / #6-8=緑)。
   星は自チームの箱にだけ入れる(チーム対抗戦)。UI の teamOf と同式に保つこと。 */
static int team_of_id(int id) {
    return (id / 3) % N_BINS;
}
static int team_bin(const robot_t *r) {
    return team_of_id(r->id);
}

/* エリア集中指令(/command/focus)が有効か。有効な間は矩形外の星を対象にしない。 */
static int focus_active(const robot_t *r) {
    return r->sim_now_s < r->focus_until_s;
}
static int star_in_focus(const robot_t *r, int i) {
    if (!focus_active(r)) return 1;
    return STAR[i].x >= r->focus_x0 && STAR[i].x <= r->focus_x1 &&
           STAR[i].y >= r->focus_y0 && STAR[i].y <= r->focus_y1;
}

/* ===================================================================
 * 名前/ペイロード ヘルパ(A層 ccnx_name_t 経由。ワイヤには触れない)
 * =================================================================== */
static int seg_str(const ccnx_name_t *n, uint16_t i, char *out, size_t cap) {
    return ccnx_name_get_seg(n, i, out, cap);
}
static int seg_eq(const ccnx_name_t *n, uint16_t i, const char *s) {
    char b[CCNX_MAX_SEG_LEN];
    if (seg_str(n, i, b, sizeof b) < 0) return 0;
    return strcmp(b, s) == 0;
}
static int seg_int(const ccnx_name_t *n, uint16_t i) {
    char b[CCNX_MAX_SEG_LEN];
    if (seg_str(n, i, b, sizeof b) < 0) return -1;
    return atoi(b);
}

/* payload(非 NUL 終端)から "key":number を取り出す。found=1/0。 */
static double json_num(const uint8_t *pl, uint16_t len, const char *key, int *found) {
    char buf[512];
    if (found) *found = 0;
    if (!pl || len == 0) return 0.0;
    size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, pl, n);
    buf[n] = '\0';
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    char *p = strstr(buf, pat);
    if (!p) return 0.0;
    p += strlen(pat);
    if (found) *found = 1;
    return atof(p);
}

/* ===================================================================
 * present 集合 = casualty に到着した機(到着順)。accepted_ids[] を流用。
 * 到着通知(arrived)を受けた順=到着順で積む。移動時間で自然に間隔が空くので
 * 全機が同じ順序を得る(= 先頭2台が carrier で全機一致、3体化しない)。
 * =================================================================== */
static void present_reset(robot_t *r) { r->accepted_count = 0; }

static int present_index(const robot_t *r, int id) {
    for (int i = 0; i < r->accepted_count; i++)
        if (r->accepted_ids[i] == id) return i;
    return -1;
}
/* 到着通知を (id, 到着ts) で記録。ts は wall clock(全プロセス共通)なので、
   ほぼ同時到着でも全機が同じ順序を得られる。 */
static void present_add(robot_t *r, int id, double ts) {
    if (id < 0) return;
    int k = present_index(r, id);
    if (k >= 0) { if (ts < r->accepted_d[k]) r->accepted_d[k] = ts; return; }
    if (r->accepted_count < ROBOT_MAX_ACCEPT) {
        r->accepted_d[r->accepted_count] = ts;
        r->accepted_ids[r->accepted_count++] = id;
    }
}
/* 到着 ts 昇順(タイは id 昇順)で先頭 n 台の id を out[] に返す。全機一致の順序。 */
static int present_topn(const robot_t *r, int n, int *out) {
    int idx[ROBOT_MAX_ACCEPT], m = r->accepted_count;
    for (int i = 0; i < m; i++) idx[i] = i;
    for (int i = 0; i < m; i++) {
        int best = i;
        for (int j = i + 1; j < m; j++) {
            double tj = r->accepted_d[idx[j]], tb = r->accepted_d[idx[best]];
            if (tj < tb || (tj == tb && r->accepted_ids[idx[j]] < r->accepted_ids[idx[best]]))
                best = j;
        }
        int t = idx[i]; idx[i] = idx[best]; idx[best] = t;
    }
    int k = 0;
    for (int i = 0; i < m && k < n; i++) out[k++] = r->accepted_ids[idx[i]];
    return k;
}

/* ===================================================================
 * 送出ラッパ(ccnx.h の API のみ)
 * =================================================================== */
static void publish_claim(robot_t *r, int star_id) {
    char name[CCNX_URI_MAX], pl[128];
    snprintf(name, sizeof name, "/star/%d/claim", star_id);
    snprintf(pl, sizeof pl, "{\"id\":%d,\"star\":%d,\"t\":%lld}",
             r->id, star_id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* 星を拾った報告(Content Object)。observer が既定で ControlCenter 宛(toctl)に描くので
   「拾った瞬間の報告パケット」が可視化される。RFC: A層 publish のみ、ワイヤは不変。 */
static void publish_collected(robot_t *r, int star_id) {
    char name[CCNX_URI_MAX], pl[128];
    snprintf(name, sizeof name, "/star/%d/collected", star_id);
    snprintf(pl, sizeof pl, "{\"id\":%d,\"star\":%d,\"t\":%lld}",
             r->id, star_id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* 星を星置き場に置いた報告(Content Object)。observer が ControlCenter 宛に描くので
   「置いた瞬間の報告パケット」が可視化される。 */
static void publish_deposited(robot_t *r, int star_id) {
    char name[CCNX_URI_MAX], pl[128];
    snprintf(name, sizeof name, "/star/%d/deposited", star_id);
    snprintf(pl, sizeof pl, "{\"id\":%d,\"star\":%d,\"t\":%lld}",
             r->id, star_id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* ゴミ箱お引っ越し告知: 自チームの箱を置いた新位置を全体へ撒く(CO announce)。
   同チーム機は傍受で投函先を更新、Go observer が payload を decode して UI の箱も動く。 */
static void publish_bin_moved(robot_t *r) {
    char name[CCNX_URI_MAX], pl[128];
    int team = team_bin(r);
    snprintf(name, sizeof name, "/bin/%d/moved", team);
    snprintf(pl, sizeof pl, "{\"team\":%d,\"x\":%.2f,\"y\":%.2f,\"by\":%d,\"t\":%lld}",
             team, r->bin_pos.x, r->bin_pos.y, r->id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* ゴミ箱お引っ越しの着手宣言: 「この箱はいま私が運びます」を announce。
   僚機は傍受でクールダウンし、二重運搬(UI で箱が飛び回る)を防ぐ。 */
static void publish_bin_claim(robot_t *r) {
    char name[CCNX_URI_MAX], pl[96];
    int team = team_bin(r);
    snprintf(name, sizeof name, "/bin/%d/claim", team);
    snprintf(pl, sizeof pl, "{\"team\":%d,\"by\":%d,\"t\":%lld}", team, r->id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* casualty が撒く「助けて！」ビーコン(Interest)。自分の位置を Payload(8609)で運ぶ。
   聞いた近くの機が向かう。周期的に撒くので取りこぼしなく応答が集まる。 */
static void publish_help(robot_t *r) {
    char name[CCNX_URI_MAX], pl[160];
    snprintf(name, sizeof name, "/task/rescue/robot/%d/carriers/%d", r->id, r->task_n);
    snprintf(pl, sizeof pl, "{\"robot\":%d,\"x\":%.2f,\"y\":%.2f,\"dest\":\"%s\"}",
             r->id, r->mv.pos.x, r->mv.pos.y, HOSPITAL_ID);
    ccnx_express_interest(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl),
                          8, 4000, NULL, r);
}

/* 到着通知: casualty に着いた機が「着いたよ」を撒く。先着機はこれを受けて相方を知る。 */
static void publish_arrived(robot_t *r) {
    char name[CCNX_URI_MAX], pl[96];
    snprintf(name, sizeof name, "/task/rescue/robot/%d/carriers/%d/arrived/%d",
             r->task_casualty_id, r->task_n, r->id);
    snprintf(pl, sizeof pl, "{\"id\":%d,\"t\":%lld}", r->id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* ペア確定「君に決めた！一緒にやろう！」= 先着(a)が2番目(b)を指名し全機へ撒く。
   b は自分が選ばれたと知り、a と組む。向かっていた他の機は「もう来なくていい」で撤退。 */
static void publish_pair(robot_t *r, int a, int b) {
    char name[CCNX_URI_MAX], pl[96];
    snprintf(name, sizeof name, "/task/rescue/robot/%d/carriers/%d/pair",
             r->task_casualty_id, r->task_n);
    snprintf(pl, sizeof pl, "{\"a\":%d,\"b\":%d}", a, b);
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* 搬送完了 → casualty 復活通知。casualty(生存・listening)が on_content(傍受)で
   /robot/{casualtyId}/repaired を受けたら PATROL へ復帰する(改修1)。 */
static void publish_repaired(robot_t *r) {
    char name[CCNX_URI_MAX], pl[128];
    snprintf(name, sizeof name, "/robot/%d/repaired", r->task_casualty_id);
    snprintf(pl, sizeof pl, "{\"id\":%d,\"by\":%d,\"t\":%lld}",
             r->task_casualty_id, r->id, wall_ms());
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

static void publish_pose(robot_t *r) {
    char name[CCNX_URI_MAX], pl[128];
    snprintf(name, sizeof name, "/robot/%d/pose", r->id);
    snprintf(pl, sizeof pl,
             "{\"id\":%d,\"x\":%.1f,\"y\":%.1f,\"hd\":%.2f,\"v\":%.1f}",
             r->id, r->mv.pos.x, r->mv.pos.y, r->mv.heading,
             r->carrying ? ROBOT_SPEED_CARRY : ROBOT_SPEED_NOMINAL);
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

/* 搬送完了 通知: /task/rescue/.../done を系へ撒く(CO announce)。 */
static void publish_done(robot_t *r) {
    char name[CCNX_URI_MAX], pl[128];
    snprintf(name, sizeof name, "/task/rescue/robot/%d/carriers/%d/done",
             r->task_casualty_id, r->task_n);
    if (r->partner_id >= 0)
        snprintf(pl, sizeof pl, "{\"casualty\":%d,\"by\":[%d,%d]}",
                 r->task_casualty_id, r->id, r->partner_id);
    else
        snprintf(pl, sizeof pl, "{\"casualty\":%d,\"by\":[%d]}", r->task_casualty_id, r->id);
    ccnx_publish(r->portal, name, (const uint8_t *)pl, (uint16_t)strlen(pl), 0);
}

static void express_recruit(robot_t *r) {
    /* 1機分の補充: 生存機が救助機構を再利用して carriers/1 を送る(DESIGN §7)。
       casualty 位置(x,y)を Payload に載せ、新 carrier が現場へ向かえるようにする。 */
    char name[CCNX_URI_MAX], pl[192];
    snprintf(name, sizeof name, "/task/rescue/robot/%d/carriers/1", r->task_casualty_id);
    snprintf(pl, sizeof pl,
             "{\"src\":\"%d\",\"dest\":\"%s\",\"robot\":%d,\"x\":%.2f,\"y\":%.2f,\"t\":%lld}",
             r->id, r->task_dest, r->task_casualty_id,
             r->patient_pos.x, r->patient_pos.y, wall_ms());
    ccnx_express_interest(r->portal, name, (const uint8_t *)pl,
                          (uint16_t)strlen(pl), 0, 0, NULL, r);
}

/* ===================================================================
 * A層コールバック(ctx = robot_t*)
 * =================================================================== */

/* /area/{zone}: 担当登録の受け皿。プレゼンス問い合わせに応答するだけ(名前空間は増やさない)。 */
static void on_area_interest(ccnx_portal_t *p, const ccnx_name_t *name,
                             const uint8_t *payload, uint16_t payload_len, void *ctx) {
    (void)p; (void)name; (void)payload; (void)payload_len; (void)ctx;
    /* v1: 応答不要(担当判定は rescue 側で行う)。登録のみで存在を示す。 */
}

/* /robot/{id}/pose 宛 Interest への応答経路: 要求されたら自 pose CO を publish する。
   隊形維持そのものは周期 pose announce(publish + 傍受)で成立しており、現行シナリオでは
   この pose 要求 Interest を出す送出側が存在しない(= 本ハンドラは呼ばれない)。 */
static void on_pose_interest(ccnx_portal_t *p, const ccnx_name_t *name,
                             const uint8_t *payload, uint16_t payload_len, void *ctx) {
    (void)p; (void)name; (void)payload; (void)payload_len;
    robot_t *r = (robot_t *)ctx;
    publish_pose(r);
}

/* /command/home: 「おうちに戻れ」= ControlCenter へ帰還。救助従事中/救助対象は継続(見捨てない)。 */
static void on_home_interest(ccnx_portal_t *p, const ccnx_name_t *name,
                             const uint8_t *payload, uint16_t payload_len, void *ctx) {
    (void)p; (void)name; (void)payload; (void)payload_len;
    robot_t *r = (robot_t *)ctx;
    if (is_committed(r->state) || r->state == ST_NEEDS_RESCUE) return;
    if (r->target_star >= 0 && r->target_star < ROBOT_MAX_STAR)
        r->star_claimed_by[r->target_star] = -1;   /* 占有を手放す */
    r->carrying = 0;
    r->target_star = -1;
    movement_set_target(&r->mv, HOME.x, HOME.y);
    set_state(r, ST_GOING_HOME, EV_GOING_HOME);
}

/* /command/focus/{zone}: 「今から dur 秒間このエリアの星だけ取れ」。
   エリア矩形は Payload の x0,y0,x1,y1(m)。ゾーン地図は持たず Payload を正とする。 */
static void on_focus_interest(ccnx_portal_t *p, const ccnx_name_t *name,
                              const uint8_t *payload, uint16_t payload_len, void *ctx) {
    (void)p; (void)name;
    robot_t *r = (robot_t *)ctx;
    int f0, f1, f2, f3;
    double x0 = json_num(payload, payload_len, "x0", &f0);
    double y0 = json_num(payload, payload_len, "y0", &f1);
    double x1 = json_num(payload, payload_len, "x1", &f2);
    double y1 = json_num(payload, payload_len, "y1", &f3);
    if (!f0 || !f1 || !f2 || !f3) return;
    int fd;
    double dur = json_num(payload, payload_len, "dur", &fd);
    if (!fd || dur <= 0) dur = 10.0;

    r->focus_x0 = x0; r->focus_y0 = y0;
    r->focus_x1 = x1; r->focus_y1 = y1;
    double ts = (r->time_scale > 0) ? r->time_scale : 1.0;
    r->focus_until_s = r->sim_now_s + dur * ts;   /* dur は実秒 → sim 秒へ換算 */

    /* エリア外の星を狙っている最中なら仕切り直す(占有も手放す)。運搬中は投函まで継続。 */
    if (r->target_star >= 0 && r->target_star < ROBOT_MAX_STAR &&
        !star_in_focus(r, r->target_star) &&
        (r->state == ST_STAR_CLAIMING || r->state == ST_STAR_ENROUTE)) {
        r->star_claimed_by[r->target_star] = -1;
        r->target_star = -1;
        new_wander_target(r);
        set_state(r, ST_PATROL, EV_PATROL);
    } else if (r->state == ST_PATROL) {
        new_wander_target(r);   /* 徘徊目標をエリア内へ切替 */
    }
}

/* /task/rescue/robot/{casualtyId}/carriers/{n}: 救助/補充 Interest(改修1)。
   患者 = 止まった故障ロボ(casualty)。その現在位置は Interest Payload の x,y で運ばれる
   (8609 Payload TLV, RFC準拠。裏チャネルは使わない)。 */
static void on_rescue_interest(ccnx_portal_t *p, const ccnx_name_t *name,
                               const uint8_t *payload, uint16_t payload_len, void *ctx) {
    (void)p;
    robot_t *r = (robot_t *)ctx;

    /* segs: task/rescue/robot/{casualtyId}/carriers/{n} */
    int casualty = seg_int(name, 3);
    if (casualty < 0) return;
    int n = seg_int(name, 5);
    if (n < 1) return;

    /* 自分が救助対象なら停止して待つだけ(自分は運べない・仲裁もしない)。 */
    if (casualty == r->id) {
        if (r->state != ST_NEEDS_RESCUE) robot_become_casualty(r);
        return;
    }

    /* 既に救助従事中/入札中/救助対象は不参加。 */
    if (is_committed(r->state) || r->state == ST_BIDDING || r->state == ST_NEEDS_RESCUE) return;

    /* casualty 位置を Payload の x,y から読む。無ければ無効な救助として無視。 */
    int fx = 0, fy = 0;
    double cx = json_num(payload, payload_len, "x", &fx);
    double cy = json_num(payload, payload_len, "y", &fy);
    if (!fx || !fy) return;
    vec2_t patient = { cx, cy };

    /* dest(搬送先)を payload から。無ければ hospital-1 既定。 */
    r->task_dest[0] = '\0';
    if (payload && payload_len) {
        char buf[256];
        size_t bl = payload_len < sizeof buf - 1 ? payload_len : sizeof buf - 1;
        memcpy(buf, payload, bl); buf[bl] = '\0';
        char *q = strstr(buf, "\"dest\":\"");
        if (q) {
            q += strlen("\"dest\":\"");
            char *e = strchr(q, '"');
            if (e && (size_t)(e - q) < sizeof r->task_dest) {
                size_t dl = (size_t)(e - q);
                memcpy(r->task_dest, q, dl); r->task_dest[dl] = '\0';
            }
        }
    }
    if (r->task_dest[0] == '\0')
        snprintf(r->task_dest, sizeof r->task_dest, "%s", HOSPITAL_ID);

    /* 箱の運搬中に救助へ回る場合は「箱は元の場所のまま」を告知して追跡を解除させる。 */
    if (r->state == ST_BIN_CARRYING) publish_bin_moved(r);

    /* 星運搬中でも中断可能(救助 > 星)。carrying を落として入札へ。 */
    r->task_casualty_id = casualty;
    r->task_zone[0] = '\0';
    r->task_n = n;
    r->patient_pos = patient;
    r->hospital_pos = HOSPITAL;   /* v1 は病院1個 */
    r->carrying = 0;
    r->target_star = -1;
    r->partner_id = -1;
    present_reset(r);

    r->bid_distance = movement_dist_to(&r->mv, patient.x, patient.y);
    double diag = hypot(r->mv.field_w, r->mv.field_h);
    double frac = diag > 0 ? clampd(r->bid_distance / diag, 0.0, 1.0) : 0.0;
    /* 距離比例 + 小ジッタ(同距離のタイ崩し) */
    r->backoff_ms = clampd(frac * RESCUE_BACKOFF_MAX_MS + rng_unit(&r->rng) * 40.0,
                           0.0, RESCUE_BACKOFF_MAX_MS);
    r->phase_deadline_s = r->sim_now_s + r->backoff_ms / 1000.0;

    movement_clear_target(&r->mv);   /* バックオフ中はその場待機 */
    set_state(r, ST_BIDDING, EV_BID_START);
}

/* 既定 on_content: 傍受 CO(accept / star claim / pose)と自 Interest 応答を処理。 */
static void on_content(ccnx_portal_t *p, const ccnx_name_t *name,
                       const uint8_t *payload, uint16_t payload_len,
                       int is_overheard, void *ctx) {
    (void)p; (void)is_overheard;
    robot_t *r = (robot_t *)ctx;

    char s0[CCNX_MAX_SEG_LEN];
    if (seg_str(name, 0, s0, sizeof s0) < 0) return;

    if (strcmp(s0, "star") == 0) {
        int tid = seg_int(name, 1);
        /* /star/{id}/claim → 二重取り回避のため占有を記録(寿命付き) */
        if (seg_eq(name, 2, "claim")) {
            int by = (int)json_num(payload, payload_len, "id", NULL);
            if (tid >= 0 && tid < ROBOT_MAX_STAR && by >= 0) {
                r->star_claimed_by[tid] = by;
                r->star_claim_ts[tid] = r->sim_now_s;   /* 占有時刻を更新 */
            }
        }
        /* /star/{id}/collected → 回収済を全機へ伝播(地面に残さない) */
        else if (seg_eq(name, 2, "collected")) {
            if (tid >= 0 && tid < ROBOT_MAX_STAR)
                r->star_removed[tid] = 1;
        }
        return;
    }

    if (strcmp(s0, "bin") == 0) {
        int team = seg_int(name, 1);
        /* /bin/{team}/claim → 僚機が箱を運び始めた。自分は引っ越しを控える。
           自分も運搬中なら小さい id が勝つ(負けた側は箱を諦めて巡回へ)。 */
        if (seg_eq(name, 2, "claim")) {
            if (team == team_bin(r)) {
                int fb; int by = (int)json_num(payload, payload_len, "by", &fb);
                r->bin_move_ok_s = r->sim_now_s + BIN_MOVE_COOLDOWN_S;
                if (fb) {
                    r->bin_carried_by = by;            /* 移動中の箱へダンクする目印 */
                    r->bin_claim_s = r->sim_now_s;
                }
                if (fb && r->state == ST_BIN_CARRYING && by != r->id && by < r->id) {
                    new_wander_target(r);
                    set_state(r, ST_PATROL, EV_PATROL);
                }
            }
            return;
        }
        /* /bin/{team}/moved → 自チームの箱のお引っ越しを傍受で同期(投函先を更新)。
           人の介入(UI ドラッグ&ドロップ → controller 発)も同じ CO で届く。 */
        if (seg_eq(name, 2, "moved")) {
            if (team == team_bin(r)) {
                int fx, fy;
                double x = json_num(payload, payload_len, "x", &fx);
                double y = json_num(payload, payload_len, "y", &fy);
                if (fx && fy) {
                    r->bin_pos.x = x; r->bin_pos.y = y;
                    r->bin_move_ok_s = r->sim_now_s + BIN_MOVE_COOLDOWN_S;
                    r->bin_carried_by = -1;             /* 運搬終了(ダンク先を固定位置へ戻す) */
                    if (r->state == ST_STAR_CARRYING)   /* 箱へ向かう途中なら向き直す */
                        movement_set_target(&r->mv, x, y);
                    else if (r->state == ST_BIN_CARRYING)   /* 競合/人の介入: 自分は降りる */
                        { new_wander_target(r); set_state(r, ST_PATROL, EV_PATROL); }
                }
            }
            return;
        }
        return;
    }

    if (strcmp(s0, "robot") == 0) {
        /* /robot/{id}/pose → 全機の位置を記録(同チーム分離用)。相方なら搬送隊形にも使う。 */
        if (seg_eq(name, 2, "pose")) {
            int rid = seg_int(name, 1);
            int fx, fy;
            double x = json_num(payload, payload_len, "x", &fx);
            double y = json_num(payload, payload_len, "y", &fy);
            if (fx && fy) {
                if (rid >= 0 && rid < ROBOT_MAX_PEERS) {
                    r->peer_xy[rid].x = x; r->peer_xy[rid].y = y;
                    r->peer_t[rid] = r->sim_now_s; r->peer_known[rid] = 1;
                }
                if (rid == r->partner_id && r->partner_id >= 0) {
                    r->partner_pos.x = x; r->partner_pos.y = y;
                    r->partner_pos_valid = 1;
                    r->partner_last_pose_s = r->sim_now_s;
                }
            }
            return;
        }
        /* /robot/{id}/repaired → 自分が救助対象なら復活(改修1)。
           carrier が病院で搬送完了時に publish した CO を傍受して受け取る。 */
        if (seg_eq(name, 2, "repaired")) {
            int rid = seg_int(name, 1);
            if (rid == r->id && r->state == ST_NEEDS_RESCUE) {
                /* 病院に置かれた → 5秒(sim15)修理してから復活。 */
                r->mv.pos = HOSPITAL;
                movement_clear_target(&r->mv);
                r->carrying = 0;
                r->partner_id = -1;
                r->target_star = -1;
                r->rescued_locked = 1;
                r->repair_deadline_s = r->sim_now_s + REPAIR_WAIT_S;
                set_state(r, ST_REPAIRING, EV_NONE);
            }
            return;
        }
        return;
    }

    if (strcmp(s0, "task") == 0) {
        int cas = seg_int(name, 3);
        if (cas != r->task_casualty_id && r->state != ST_NEEDS_RESCUE) return;

        int heading = (r->state == ST_BIDDING || r->state == ST_ENROUTE_PICKUP ||
                       r->state == ST_SYNC_WAIT || r->state == ST_COOP_LOADING);

        /* 到着通知: task/rescue/robot/{cas}/carriers/{n}/arrived/{id}
           → present(到着順)に積む。RECRUITING の相方喪失中なら新到着を相方採用。 */
        if (seg_eq(name, 6, "arrived")) {
            int aid = seg_int(name, 7);
            if (aid < 0) return;
            if (r->state == ST_RECRUITING) {
                if (aid != r->id && aid != r->partner_id) {
                    r->partner_id = aid;
                    r->partner_pos_valid = 0;
                    r->partner_last_pose_s = r->sim_now_s;
                    movement_set_target(&r->mv, r->hospital_pos.x, r->hospital_pos.y);
                    set_state(r, ST_COOP_CARRYING, EV_RECRUITED);
                }
                return;
            }
            if (heading) {
                int ft; double ts = json_num(payload, payload_len, "t", &ft);
                present_add(r, aid, ft ? ts : (double)wall_ms());
            }
            return;
        }

        /* ペア確定「君に決めた！」: task/rescue/robot/{cas}/carriers/{n}/pair {a,b}
           b は選ばれた→a と組む。確定2名以外で向かっていた機は「もう来なくていい」で撤退。 */
        if (seg_eq(name, 6, "pair")) {
            int a = (int)json_num(payload, payload_len, "a", NULL);
            int b = (int)json_num(payload, payload_len, "b", NULL);
            /* 自分の救助のペアが決まった → 「助けて！」を止める(casualty)。 */
            if (r->state == ST_NEEDS_RESCUE && cas == r->id) { r->rescued_locked = 1; return; }
            if (r->id == a || r->id == b) {
                r->partner_id = (r->id == a) ? b : a;
                if (r->state == ST_ENROUTE_PICKUP || r->state == ST_BIDDING) {
                    /* まだ現場へ向かう。着いたら合流。 */
                    movement_set_target(&r->mv, r->patient_pos.x, r->patient_pos.y);
                    if (r->state == ST_BIDDING) set_state(r, ST_ENROUTE_PICKUP, EV_PAIR_FORMED);
                }
            } else if (heading) {
                r->partner_id = -1;
                set_state(r, ST_WITHDRAWN, EV_WITHDRAWN);   /* → 星拾いに戻る */
            }
            return;
        }
        /* done CO は系への告知。B層は特段の処理不要(observer が可視化)。 */
        return;
    }
}

/* ===================================================================
 * 初期化 / 登録
 * =================================================================== */
void robot_init(robot_t *r, ccnx_portal_t *portal, int id, const char *zone,
                double x, double y, double field_w, double field_h,
                int n_robots, const uint16_t *peers, int n_peers, uint16_t my_port) {
    memset(r, 0, sizeof *r);
    r->portal = portal;
    r->id = id;
    r->n_robots = n_robots;
    snprintf(r->zone, sizeof r->zone, "%s", zone ? zone : "");
    r->my_port = my_port;
    r->n_peers = 0;
    for (int i = 0; i < n_peers && i < ROBOT_MAX_PEERS; i++)
        r->peers[r->n_peers++] = peers[i];

    movement_init(&r->mv, x, y, field_w, field_h);

    r->state = ST_IDLE;
    r->sim_now_s = 0.0;
    r->state_since_s = 0.0;
    r->phase_deadline_s = 0.0;
    r->pending_event = EV_NONE;
    r->carrying = 0;
    r->partner_id = -1;
    r->target_star = -1;
    r->target_bin = -1;
    for (int i = 0; i < ROBOT_MAX_STAR; i++) {
        r->star_claimed_by[i] = -1;
        r->star_claim_ts[i] = -1e9;
        r->star_removed[i] = 0;
    }
    r->task_zone[0] = '\0';
    r->task_casualty_id = -1;
    r->task_dest[0] = '\0';
    r->task_n = 0;
    r->accepted_count = 0;
    r->partner_pos_valid = 0;
    r->rng = 0x9E3779B9u ^ (unsigned)(id * 2654435761u + 1);
    r->bin_pos = BINS[team_bin(r)];   /* 自チーム箱の初期位置(以後 /bin/{team}/moved で更新) */
    r->bin_carried_by = -1;
}

int robot_register(robot_t *r) {
    char uri[CCNX_URI_MAX];
    int rc;

    /* /area/{zone} 担当登録 */
    if (r->zone[0]) {
        snprintf(uri, sizeof uri, "/area/%s", r->zone);
        rc = ccnx_register_prefix(r->portal, uri, on_area_interest, r);
        if (rc < 0) return rc;
    }
    /* /task/rescue: 全救助/補充 Interest を受ける(担当判定はハンドラ内) */
    rc = ccnx_register_prefix(r->portal, "/command/home", on_home_interest, r);
    if (rc != CCNX_OK) return rc;

    /* /command/focus: エリア集中指令の受け皿 */
    rc = ccnx_register_prefix(r->portal, "/command/focus", on_focus_interest, r);
    if (rc < 0) return rc;
    rc = ccnx_register_prefix(r->portal, "/task/rescue", on_rescue_interest, r);
    if (rc < 0) return rc;

    /* /robot/{id}/pose: pose 要求 Interest への応答経路(現行シナリオでは未使用) */
    snprintf(uri, sizeof uri, "/robot/%d/pose", r->id);
    rc = ccnx_register_prefix(r->portal, uri, on_pose_interest, r);
    if (rc < 0) return rc;

    /* 傍受 CO の既定ハンドラ */
    ccnx_set_default_on_content(r->portal, on_content, r);

    /* FIB: デフォルトルート "/" を自分以外の全 peer へ(ブロードキャスト相当, PROTOCOL §5.2) */
    for (int i = 0; i < r->n_peers; i++) {
        if (r->peers[i] == r->my_port) continue;
        ccnx_fib_add_route(r->portal, "/", r->peers[i]);
    }
    return CCNX_OK;
}

/* ===================================================================
 * 徘徊/移動ヘルパ
 * =================================================================== */
static void new_wander_target(robot_t *r) {
    /* 最寄りの未回収・未占有の星へ向かう(置いた直後もすぐ次の星へ直行できる)。
       占有済みでも寿命切れは対象。星が無ければランダム徘徊。 */
    int best = -1; double bd = 1e18;
    for (int i = 0; i < N_STAR && i < ROBOT_MAX_STAR; i++) {
        if (r->star_removed[i]) continue;
        if (!star_in_focus(r, i)) continue;    /* 集中指令中はエリア外の星を狙わない */
        if (r->star_claimed_by[i] >= 0 &&
            (r->sim_now_s - r->star_claim_ts[i]) < CLAIM_TTL_S) continue;
        double d = vec2_dist(r->mv.pos, STAR[i]);
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) { movement_set_target(&r->mv, STAR[best].x, STAR[best].y); return; }
    if (focus_active(r)) {
        /* エリア内に対象が無ければエリア内を徘徊して待つ */
        double x = r->focus_x0 + rng_unit(&r->rng) * (r->focus_x1 - r->focus_x0);
        double y = r->focus_y0 + rng_unit(&r->rng) * (r->focus_y1 - r->focus_y0);
        movement_set_target(&r->mv, x, y);
        return;
    }
    double x = rng_unit(&r->rng) * r->mv.field_w;
    double y = rng_unit(&r->rng) * r->mv.field_h;
    movement_set_target(&r->mv, x, y);
}

/* ===================================================================
 * 同チーム分離: 同僚と 0.5マス(=1.0m)未満に近づいたら緩く離れる。
 * 目標が近い時(到着直前・星拾い・投函)は働かせない。
 * =================================================================== */
static void teammate_separation(robot_t *r, double dt_s) {
    if (!movement_has_target(&r->mv)) return;
    if (movement_dist_to(&r->mv, r->mv.target.x, r->mv.target.y) < TEAM_SEP_GOAL_GUARD_M)
        return;                                     /* 到着を妨げない */
    int best = -1; double bd = 1e18, bdx = 0, bdy = 0;
    for (int i = 0; i < r->n_robots && i < ROBOT_MAX_PEERS; i++) {
        if (i == r->id || !r->peer_known[i]) continue;
        if (team_of_id(i) != team_of_id(r->id)) continue;  /* 同チームのみ */
        if (r->sim_now_s - r->peer_t[i] > POSE_BEACON_S * 1.5) continue;   /* 古い位置は無視 */
        double dx = r->mv.pos.x - r->peer_xy[i].x;
        double dy = r->mv.pos.y - r->peer_xy[i].y;
        double d = sqrt(dx * dx + dy * dy);
        if (d < TEAM_SEP_M && d < bd) { bd = d; best = i; bdx = dx; bdy = dy; }
    }
    if (best < 0) return;
    double d = (bd > 1e-6) ? bd : 1e-6;
    double w = (TEAM_SEP_M - bd) / TEAM_SEP_M;      /* 近いほど強い 0..1 */
    double push = TEAM_SEP_PUSH_MPS * w * dt_s;
    double x = r->mv.pos.x + (bdx / d) * push;
    double y = r->mv.pos.y + (bdy / d) * push;
    if (x < 0.0) x = 0.0; if (x > r->mv.field_w) x = r->mv.field_w;
    if (y < 0.0) y = 0.0; if (y > r->mv.field_h) y = r->mv.field_h;
    r->mv.pos.x = x; r->mv.pos.y = y;
}

/* 単独移動(星拾い系/帰宅): 前進 + 同チーム分離。 */
static void move_solo(robot_t *r, double dt_s, double speed) {
    movement_step(&r->mv, dt_s, speed);
    teammate_separation(r, dt_s);
}

/* ゴミ箱お引っ越し先の判定: 残存星「群」全体の重心(=群の中央)へ運ぶ。
   3チームの箱が重ならないよう、チームごとに 120° 方向へ 2m オフセット。
   箱がそこから BIN_FAR_M 以上離れている時だけ 1 を返し座標を出す。 */
static int bin_relocation_target(const robot_t *r, double *ox, double *oy) {
    double sx = 0, sy = 0; int k = 0;
    for (int i = 0; i < N_STAR && i < ROBOT_MAX_STAR; i++) {
        if (r->star_removed[i]) continue;
        if (!star_in_focus(r, i)) continue;
        sx += STAR[i].x; sy += STAR[i].y; k++;
    }
    if (k <= 0) return 0;
    sx /= k; sy /= k;
    /* 重なり回避: 群の中央から「自陣(初期の箱のコーナー)方向」へ 4m 寄せる。
       各チームの箱がクラスタ中心の自分側に並び、序盤の重心≒病院(中央)とも重ならない。 */
    {
        double hx = BINS[team_bin(r)].x - sx, hy = BINS[team_bin(r)].y - sy;
        double hl = sqrt(hx * hx + hy * hy);
        if (hl > 1e-6) { sx += hx / hl * 4.0; sy += hy / hl * 4.0; }
    }
    if (sx < 1.0) sx = 1.0; if (sx > r->mv.field_w - 1.0) sx = r->mv.field_w - 1.0;
    if (sy < 1.0) sy = 1.0; if (sy > r->mv.field_h - 1.0) sy = r->mv.field_h - 1.0;
    vec2_t c = { sx, sy };
    if (vec2_dist(r->bin_pos, c) < BIN_FAR_M) return 0;   /* 箱は既に群の中央付近 */
    *ox = sx; *oy = sy;
    return 1;
}

/* PATROL 中の近傍星探索。対象を見つけたら index、無ければ -1。 */
static int find_nearby_star(const robot_t *r) {
    int best = -1; double bd = STAR_RADIUS_M;
    for (int i = 0; i < N_STAR && i < ROBOT_MAX_STAR; i++) {
        if (r->star_removed[i]) continue;
        if (!star_in_focus(r, i)) continue;    /* 集中指令中はエリア外の星を拾わない */
        double d = vec2_dist(r->mv.pos, STAR[i]);
        /* 他機が占有(有効期間内)している星は避ける。ただし目の前(0.5m以内)なら
           占有を無視して拾う(「近くにあるのに拾えない」を防ぐ)。自分の占有は無視。 */
        int claimed_by_other = (r->star_claimed_by[i] >= 0 && r->star_claimed_by[i] != r->id &&
                                (r->sim_now_s - r->star_claim_ts[i]) < CLAIM_TTL_S);
        if (claimed_by_other && d > 0.5) continue;
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}


/* ===================================================================
 * 状態機械 1 反復
 * =================================================================== */
/* 自動故障の対象になる状態(単独稼働中のみ。救助従事中/救助対象/帰宅中は除外)。 */
static int auto_fail_eligible(robot_state_t s) {
    return s == ST_PATROL || s == ST_STAR_CLAIMING || s == ST_STAR_ENROUTE ||
           s == ST_STAR_LOADING || s == ST_STAR_CARRYING || s == ST_STAR_UNLOADING;
}

void robot_update(robot_t *r, double sim_dt_s) {
    if (sim_dt_s < 0) sim_dt_s = 0;

    /* 位置ビーコン: 低頻度(POSE_BEACON_S = 6 sim秒 ≒ 実2秒)で自 pose を撒く(同チーム分離用)。
       搬送中(COOP_CARRYING)は隊形用の高頻度 publish が別にあるため重複させない。 */
    if (r->state != ST_FAILED && r->state != ST_NEEDS_RESCUE && r->state != ST_REPAIRING &&
        r->state != ST_COOP_CARRYING && r->sim_now_s >= r->pose_beacon_s) {
        publish_pose(r);
        r->pose_beacon_s = r->sim_now_s + POSE_BEACON_S;
    }

    /* 自動故障: 全 N 台合計で平均およそ 1台/分(実時間)倒れる。倒れると casualty 化し
       「助けて！」を撒く。sim は 3倍速なので rate は 1/(N×180 sim秒)で実時間 1/分相当。 */
    if (auto_fail_eligible(r->state) && r->n_robots > 0 &&
        r->sim_now_s >= r->no_fail_until_s) {
        double rate = 1.0 / ((double)r->n_robots * 180.0);   /* per robot per sim 秒 */
        if (rng_unit(&r->rng) < rate * sim_dt_s) {
            robot_become_casualty(r);
            return;
        }
    }

    switch (r->state) {

    case ST_IDLE:
        new_wander_target(r);
        set_state(r, ST_PATROL, EV_PATROL);
        break;

    case ST_PATROL: {
        move_solo(r, sim_dt_s, ROBOT_SPEED_NOMINAL);
        if (movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)) new_wander_target(r);
        int t = find_nearby_star(r);
        if (t >= 0) {
            r->target_star = t;
            r->star_claimed_by[t] = r->id;      /* 自占有を記録 */
            r->star_claim_ts[t] = r->sim_now_s; /* 占有時刻(寿命判定用) */
            publish_claim(r, t);                 /* /star/{t}/claim を publish */
            movement_clear_target(&r->mv);
            set_state(r, ST_STAR_CLAIMING, EV_STAR_CLAIM);
        }
        break;
    }

    case ST_STAR_CLAIMING: {
        /* claim 済。星へ向かう。 */
        int t = r->target_star;
        if (t < 0 || t >= N_STAR) { new_wander_target(r); set_state(r, ST_PATROL, EV_PATROL); break; }
        movement_set_target(&r->mv, STAR[t].x, STAR[t].y);
        set_state(r, ST_STAR_ENROUTE, EV_NONE);
        break;
    }

    case ST_STAR_ENROUTE:
        move_solo(r, sim_dt_s, ROBOT_SPEED_NOMINAL);
        if (movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)) {
            r->phase_deadline_s = r->sim_now_s + LOAD_STAR_S;
            set_state(r, ST_STAR_LOADING, EV_NONE);
        }
        break;

    case ST_STAR_LOADING:
        if (r->sim_now_s >= r->phase_deadline_s) {
            r->carrying = 1;
            publish_collected(r, r->target_star);   /* 拾った報告パケット(可視化される) */
            r->target_bin = team_bin(r);
            movement_set_target(&r->mv, r->bin_pos.x, r->bin_pos.y);   /* 箱は可動(現在位置へ) */
            set_state(r, ST_STAR_CARRYING, EV_STAR_PICK);
        }
        break;

    case ST_STAR_CARRYING:
        /* 箱が僚機に運ばれている最中は、その僚機の現在位置(pose 傍受)へ追従して
           「動いている箱」にダンクする。claim が古い/位置が古い場合は固定位置へ。 */
        if (r->bin_carried_by >= 0 && r->bin_carried_by < ROBOT_MAX_PEERS) {
            int c = r->bin_carried_by;
            if (r->sim_now_s - r->bin_claim_s > BIN_MOVE_COOLDOWN_S) {
                r->bin_carried_by = -1;               /* claim が古い → 追跡解除 */
            } else if (r->peer_known[c] &&
                       r->sim_now_s - r->peer_t[c] < POSE_TIMEOUT_S * 2.0) {
                movement_set_target(&r->mv, r->peer_xy[c].x, r->peer_xy[c].y);
            }
        }
        move_solo(r, sim_dt_s, ROBOT_SPEED_NOMINAL);
        if (movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)) {
            r->phase_deadline_s = r->sim_now_s + LOAD_STAR_S;
            set_state(r, ST_STAR_UNLOADING, EV_NONE);
        }
        break;

    case ST_STAR_UNLOADING:
        if (r->sim_now_s >= r->phase_deadline_s) {
            r->carrying = 0;
            if (r->target_star >= 0 && r->target_star < ROBOT_MAX_STAR) {
                r->star_removed[r->target_star] = 1;
                publish_deposited(r, r->target_star);   /* 置いた報告パケット(可視化) */
            }
            r->target_star = -1;
            /* 箱のお引っ越し判定: 残存星群の重心から箱が遠ければ担いで移設。
               着手宣言(claim)を撒き、僚機の二重運搬を防ぐ。 */
            {
                double bx, by;
                if (r->sim_now_s >= r->bin_move_ok_s &&
                    bin_relocation_target(r, &bx, &by)) {
                    publish_bin_claim(r);
                    r->bin_move_ok_s = r->sim_now_s + BIN_MOVE_COOLDOWN_S;
                    movement_set_target(&r->mv, bx, by);
                    set_state(r, ST_BIN_CARRYING, EV_NONE);
                    break;
                }
            }
            new_wander_target(r);
            set_state(r, ST_PATROL, EV_STAR_DROP);
        }
        break;

    case ST_BIN_CARRYING:
        /* 自チームの箱を星の密集地へ運ぶ。着いたら置いて /bin/{team}/moved で告知。
           救助 Interest が来たら on_rescue_interest がこの状態を中断する(箱は元位置のまま)。 */
        move_solo(r, sim_dt_s, ROBOT_SPEED_CARRY);
        /* 運搬中は pose を高頻度化(僚機が「動く箱」へダンクできるように追跡させる) */
        if (r->sim_now_s >= r->pose_tx_deadline_s) {
            publish_pose(r);
            r->pose_tx_deadline_s = r->sim_now_s + POSE_INTERVAL_S;
        }
        if (movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)) {
            r->bin_pos = r->mv.pos;
            r->bin_move_ok_s = r->sim_now_s + BIN_MOVE_COOLDOWN_S;
            publish_bin_moved(r);
            new_wander_target(r);
            set_state(r, ST_PATROL, EV_PATROL);
        }
        break;

    case ST_BIDDING:
        /* バックオフ後、救助対象へ向かう(到着順で決めるので事前選抜しない)。
           向かう前に他所でペア確定(pair)を傍受していれば on_content 側で撤退済み。 */
        if (r->sim_now_s >= r->phase_deadline_s) {
            movement_set_target(&r->mv, r->patient_pos.x, r->patient_pos.y);
            r->partner_id = -1;
            r->partner_pos_valid = 0;
            r->partner_last_pose_s = r->sim_now_s;
            set_state(r, ST_ENROUTE_PICKUP, EV_PAIR_FORMED);
        }
        break;

    /* ST_SELECTED / ST_PAIRING は到着ベースでは使わない(遷移しない)。 */
    case ST_SELECTED:
    case ST_PAIRING:
        set_state(r, ST_WITHDRAWN, EV_WITHDRAWN);
        break;

    case ST_ENROUTE_PICKUP:
        movement_step(&r->mv, sim_dt_s, ROBOT_SPEED_NOMINAL);  /* 救助対象に確実に寄る(回避しない) */
        if (movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)) {
            /* 到着 → 自分を present(到着ts付き)に積み「着いたよ」を撒く → 判定へ。
               直後は present が未確定なので settle 窓を置いてから順位を決める。 */
            present_add(r, r->id, (double)wall_ms());
            publish_arrived(r);
            r->phase_deadline_s = r->sim_now_s + SYNC_SETTLE_S;
            set_state(r, ST_SYNC_WAIT, EV_SYNC_WAIT);
        }
        break;

    case ST_SYNC_WAIT:
        /* settle 後、到着ts順(全機共通)の先頭 task_n が carrier。3番目以降は星拾いへ戻る。
           先着(carriers[0])が「君に決めた！」(pair)を撒いて確定&他候補を打ち切る。 */
        if (r->sim_now_s >= r->phase_deadline_s) {
            int carriers[ROBOT_MAX_ACCEPT];
            int nc = present_topn(r, r->task_n, carriers);
            int mine = 0, first = (nc > 0 ? carriers[0] : -1);
            for (int i = 0; i < nc; i++) if (carriers[i] == r->id) mine = 1;
            if (r->accepted_count >= r->task_n && !mine) {
                set_state(r, ST_WITHDRAWN, EV_WITHDRAWN);   /* 先頭 task_n 圏外 → 星拾いへ */
            } else if (r->accepted_count >= r->task_n && mine) {
                for (int i = 0; i < nc; i++)
                    if (carriers[i] != r->id) { r->partner_id = carriers[i]; break; }
                if (r->id == first)                          /* 先着だけが告知(重複回避) */
                    publish_pair(r, carriers[0], carriers[1]);
                r->phase_deadline_s = r->sim_now_s + LOAD_COOP_S;
                set_state(r, ST_COOP_LOADING, EV_COOP_LOAD);
            }
            /* task_n 未満なら先着として相方到着を待つ(settle を延長) */
            else r->phase_deadline_s = r->sim_now_s + SYNC_SETTLE_S;
        }
        break;

    case ST_COOP_LOADING:
        if (r->sim_now_s >= r->phase_deadline_s) {
            r->carrying = 1;
            movement_set_target(&r->mv, r->hospital_pos.x, r->hospital_pos.y);
            r->pose_tx_deadline_s = r->sim_now_s;
            r->partner_last_pose_s = r->sim_now_s;   /* 搬送開始からタイムアウト計測(誤発火防止) */
            r->partner_pos_valid = 0;
            set_state(r, ST_COOP_CARRYING, EV_PICKUP_ARR);
        }
        break;

    case ST_COOP_CARRYING: {
        /* 担架隊形: 小さい id が「頭側」で病院へ先導、大きい id は「足側」で
           頭側の 1.8m 後方(担架の長さ)に付く。位置は相方が周期 publish する pose CO の傍受で得る。
           casualty は browser が2機の中点(=担架の中央)に描くので、頭-足の直線上に乗る。 */
        int is_head = (r->partner_id < 0) || (r->id < r->partner_id);
        if (is_head) {
            movement_step(&r->mv, sim_dt_s, ROBOT_SPEED_CARRY);   /* 病院へ */
        } else if (r->partner_pos_valid) {
            double ux = r->hospital_pos.x - r->partner_pos.x;
            double uy = r->hospital_pos.y - r->partner_pos.y;
            double ul = sqrt(ux * ux + uy * uy);
            if (ul > 1e-6) { ux /= ul; uy /= ul; } else { ux = 1.0; uy = 0.0; }
            /* pose は最大 POSE_INTERVAL_S 遅れて届く。その間に頭側が進む距離を
               先読みして詰める(実測でほぼ STRETCHER_LEN_M に収束)。 */
            double lag = ROBOT_SPEED_CARRY * POSE_INTERVAL_S;
            double off = STRETCHER_LEN_M - lag;
            if (off < 0.8) off = 0.8;
            movement_set_target(&r->mv,
                                r->partner_pos.x - ux * off,
                                r->partner_pos.y - uy * off);
            /* 追従側は少し速めに詰める(遅れて担架が伸びないように) */
            movement_step(&r->mv, sim_dt_s, ROBOT_SPEED_CARRY * 1.2);
        } else {
            movement_step(&r->mv, sim_dt_s, ROBOT_SPEED_CARRY);   /* pose 未受信の間は自走 */
        }
        /* 隊形維持: 周期ごとに自 pose を publish(相方が傍受)。Interest 要求はフラッディングするため使わない。 */
        if (r->sim_now_s >= r->pose_tx_deadline_s) {
            publish_pose(r);
            r->pose_tx_deadline_s = r->sim_now_s + POSE_INTERVAL_S;
        }
        /* 相方 lost 検知(SIGTERM で pose 途絶)→ 1機分再選抜 */
        if (r->partner_id >= 0 &&
            (r->sim_now_s - r->partner_last_pose_s) > POSE_TIMEOUT_S) {
            movement_clear_target(&r->mv);     /* 単機では担架を運べない */
            r->recruit_retry_s = r->sim_now_s;
            r->partner_id = -1;
            present_reset(r);
            set_state(r, ST_RECRUITING, EV_PARTNER_LOST);
            break;
        }
        /* 到着判定: 頭側は病院ぴったり、足側は担架1本ぶん後ろで完了扱い。 */
        int arrived = is_head
            ? movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)
            : (movement_dist_to(&r->mv, r->hospital_pos.x, r->hospital_pos.y)
               <= STRETCHER_LEN_M + ROBOT_ARRIVE_EPS + 0.3);
        if (arrived) {
            /* 病院到着 → 故障機を置いて担ぎ手は即開放(星拾いへ)。 */
            r->carrying = 0;
            publish_repaired(r);   /* casualty を病院に置き→修理待ちへ(/robot/{id}/repaired) */
            publish_done(r);       /* 系への搬送完了告知(任意) */
            r->partner_id = -1;
            r->task_zone[0] = '\0';
            r->task_casualty_id = -1;
            new_wander_target(r);
            set_state(r, ST_PATROL, EV_COOP_DROP);
        }
        break;
    }

    case ST_COOP_UNLOADING:   /* 現行フローでは未使用(即開放にしたため)。保険で PATROL へ。 */
        r->carrying = 0; r->partner_id = -1; r->task_casualty_id = -1;
        new_wander_target(r);
        set_state(r, ST_PATROL, EV_COOP_DROP);
        break;

    case ST_RECRUITING:
        /* 患者を抱えたまま停止し 1機分を募集。新パートナー確定は on_content が遷移。
           ★自然発生的な故障(自己 FAILED)は撤去(改修1)。相方が来るまで待ち続ける。
             故障は明示操作(fault-inject / 救助対象化)のみとする。 */
        if (r->sim_now_s >= r->recruit_retry_s) {
            express_recruit(r);                /* carriers/1 を再送 */
            /* 初回のみ recruiting イベントを積む(partner_lost の次) */
            r->pending_event = EV_RECRUITING;
            r->recruit_retry_s = r->sim_now_s + RECRUIT_RETRY_S;
        }
        break;

    case ST_WITHDRAWN:
        /* 入札から降りた → 巡回に復帰 */
        r->task_casualty_id = -1;
        new_wander_target(r);
        set_state(r, ST_PATROL, EV_PATROL);
        break;

    case ST_NEEDS_RESCUE:
        /* 救助対象化(停止・生存)。自力では動かない。carrier に病院へ運ばれ
           /robot/{id}/repaired を on_content で受けると PATROL へ復活する(改修1)。
           ペア確定までブロードキャストで「助けて！」を撒く。 */
        movement_clear_target(&r->mv);
        if (!r->rescued_locked && r->sim_now_s >= r->help_next_s) {
            publish_help(r);
            r->help_next_s = r->sim_now_s + 1.0;   /* 1秒毎 */
        }
        break;

    case ST_REPAIRING:
        /* 病院に置き去り。修理完了で復活(復活演出は browser が EV_REPAIRED で発火)。 */
        movement_clear_target(&r->mv);
        if (r->sim_now_s >= r->repair_deadline_s) {
            r->repair_deadline_s = 0;
            r->rescued_locked = 0;
            r->task_casualty_id = -1;
            new_wander_target(r);
            r->no_fail_until_s = r->sim_now_s + FAIL_COOLDOWN_S;   /* 復活直後の再故障を抑止 */
            set_state(r, ST_PATROL, EV_REPAIRED);
        }
        break;

    case ST_GOING_HOME:
        /* ControlCenter(おうち)へ帰還。着いたら星拾いに戻る。 */
        move_solo(r, sim_dt_s, ROBOT_SPEED_NOMINAL);
        if (movement_arrived(&r->mv, ROBOT_ARRIVE_EPS)) {
            new_wander_target(r);
            set_state(r, ST_PATROL, EV_PATROL);
        }
        break;

    case ST_FAILED:
        /* 到達しない(自己 FAILED は撤去済み)。保険として静止。 */
        movement_clear_target(&r->mv);
        break;

    default:
        set_state(r, ST_IDLE, EV_NONE);
        break;
    }
}

/* 救助対象化(改修1): その場で停止し ST_NEEDS_RESCUE へ。プロセスは殺さない。
   robot.c が SIGUSR1 受信フラグを検知して呼ぶ。carrier に運ばれ復活する。 */
void robot_become_casualty(robot_t *r) {
    if (r->state == ST_NEEDS_RESCUE) return;   /* 既に対象なら何もしない */
    if (r->state == ST_BIN_CARRYING) publish_bin_moved(r);   /* 箱は元の場所のまま告知 */
    movement_clear_target(&r->mv);             /* その場で停止 */
    r->carrying = 0;
    r->partner_id = -1;
    r->target_star = -1;
    r->task_casualty_id = -1;                  /* casualty 自身は carrier ではない */
    r->task_n = 2;                             /* 担ぎ手2体を要請 */
    r->rescued_locked = 0;
    r->help_next_s = r->sim_now_s;             /* 即「助けて！」ビーコン */
    present_reset(r);
    set_state(r, ST_NEEDS_RESCUE, EV_NEEDS_RESCUE);
}
