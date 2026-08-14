/*
 * behavior.h - B層: ロボ挙動(状態機械 + CCNx 相互作用)。
 *
 * B-283 CCNx セマンティック群ロボ通信デモ / C B層。
 * A層は ccnx.h の関数だけを経由する(ワイヤバイトに直接触れない, DESIGN §1)。
 * 状態機械の綴りは PROTOCOL §2、event は §2.2、名前空間は §1、payload は §1.1 に厳密準拠。
 */
#ifndef ROBOT_BEHAVIOR_H
#define ROBOT_BEHAVIOR_H

#include "ccnx.h"       /* A層 Portal API(codec.h も取り込む) */
#include "movement.h"

#define ROBOT_MAX_PEERS    32
#define ROBOT_MAX_STAR    256
#define ROBOT_MAX_ACCEPT   16
#define ZONE_STR_MAX       16
#define DEST_STR_MAX       24

/* 状態機械(PROTOCOL §2。列挙順は表記上の並びに一致) */
typedef enum {
    ST_IDLE = 0,
    ST_PATROL,
    ST_STAR_CLAIMING,
    ST_STAR_ENROUTE,
    ST_STAR_LOADING,
    ST_STAR_CARRYING,
    ST_STAR_UNLOADING,
    ST_BIDDING,
    ST_SELECTED,
    ST_PAIRING,
    ST_ENROUTE_PICKUP,
    ST_SYNC_WAIT,
    ST_COOP_LOADING,
    ST_COOP_CARRYING,
    ST_COOP_UNLOADING,
    ST_RECRUITING,
    ST_WITHDRAWN,
    ST_FAILED,
    ST_NEEDS_RESCUE,   /* 救助対象化(停止・生存)。carrier に運ばれ /robot/{id}/repaired で復活 */
    ST_REPAIRING,      /* 病院に置き去り→修理中(5秒)。完了で PATROL(復活演出) */
    ST_GOING_HOME,     /* 「おうちに戻れ」= ControlCenter へ帰還。着いたら星拾いに戻る */
    ST_BIN_CARRYING,   /* 自チームのゴミ箱を星の密集地へお引っ越し中 */
    ST_NSTATES
} robot_state_t;

/* PROTOCOL §2 の綴りをそのまま返す(JSON の state 値) */
const char *robot_state_name(robot_state_t s);

/* stars ファイル("id x y" 行)を読み、星の位置表を満たす。
   戻り値: 読み込んだ星数(path が NULL なら 0、開けなければ -1)。
   supervisor が生成した同一ファイルを全ロボが読むことで位置が一致する。 */
int behavior_load_stars(const char *path);

typedef struct robot {
    ccnx_portal_t *portal;
    int      id;
    int      n_robots;
    char     zone[ZONE_STR_MAX];
    uint16_t my_port;
    uint16_t peers[ROBOT_MAX_PEERS];
    int      n_peers;

    movement_t mv;

    robot_state_t state;
    double   sim_now_s;        /* 累積シミュレーション秒(×倍率後) */
    double   state_since_s;    /* 現状態に入った sim 時刻 */
    double   phase_deadline_s; /* LOADING/UNLOADING/backoff 用汎用タイマ */
    const char *pending_event; /* 遷移が積む event。robot.c が emit 後にクリア */

    int      carrying;         /* 星/患者を運搬中か */
    int      partner_id;       /* 協力搬送の相方。無ければ -1 */

    /* --- 星拾い --- */
    int      target_star;                     /* 対象星 index / -1 */
    int      target_bin;                       /* 目的回収箱 index / -1 */
    int      star_claimed_by[ROBOT_MAX_STAR];/* -1 未claim, else 占有ロボ id */
    double   star_claim_ts[ROBOT_MAX_STAR];  /* 占有した sim 時刻(寿命判定用) */
    int      star_removed[ROBOT_MAX_STAR];   /* 1=回収済 */

    /* --- 救助タスク --- */
    char     task_zone[ZONE_STR_MAX];          /* 旧 zone ベースの残骸(未使用。互換保持) */
    int      task_casualty_id;                 /* 救助対象の故障ロボ id。carrier 以外は -1 */
    int      task_n;                           /* 必要搬送機数 */
    char     task_dest[DEST_STR_MAX];          /* 搬送先 id(hospital-1) */
    vec2_t   patient_pos;                      /* = casualty(故障ロボ)の現在位置 */
    vec2_t   hospital_pos;
    double   bid_distance;                     /* d: 患者までの距離 m */
    double   backoff_ms;                       /* 実測バックオフ ms */
    int      accepted_ids[ROBOT_MAX_ACCEPT];   /* accept したロボ id */
    double   accepted_d[ROBOT_MAX_ACCEPT];     /* 各 accept の casualty までの距離 */
    int      accepted_count;
    double   help_next_s;                      /* casualty: 次に「助けて！」を撒く sim 時刻 */
    int      rescued_locked;                   /* casualty: ペア確定を受けた(help 停止) */

    /* --- 協力搬送(隊形維持 = 周期 pose announce の publish + 傍受) ---
       注: draft-irtf-icnrg-reflexive-forwarding の機構(RNP・PIT 拡張)は本実装に無く別物。
       pose は非請求 CO の announce であり、Interest ⇄ CO の双方向交換ではない。 */
    vec2_t   partner_pos;
    int      partner_pos_valid;
    double   partner_last_pose_s;              /* 直近 partner pose 受信 sim 時刻 */
    double   pose_tx_deadline_s;               /* 次回 pose announce(publish)の sim 時刻 */
    double   recruit_retry_s;                  /* 次回 recruit 再送 sim 時刻 */

    unsigned rng;                              /* 徘徊/ジッタ用 */

    /* --- 同チーム分離(0.5マス=1.0m 以上を保つ)用の位置認識 --- */
    vec2_t   peer_xy[ROBOT_MAX_PEERS];         /* 傍受した /robot/{id}/pose の最新位置 */
    double   peer_t[ROBOT_MAX_PEERS];          /* その受信 sim 時刻 */
    int      peer_known[ROBOT_MAX_PEERS];
    double   pose_beacon_s;                    /* 次に自 pose を撒く sim 時刻(低頻度) */

    /* --- 可動ゴミ箱(自チームの箱の現在位置。/bin/{team}/moved で同期) --- */
    vec2_t   bin_pos;                          /* 自チーム箱の現在位置 */
    double   bin_move_ok_s;                    /* 次に箱を動かしてよい sim 時刻(クールダウン) */
    int      bin_carried_by;                   /* 箱を運搬中の僚機 id(-1=なし)。運搬中はそこへダンク */
    double   bin_claim_s;                      /* claim を聞いた sim 時刻(古い claim は無効化) */

    /* --- エリア集中指令(/command/focus: このエリアの星だけ取る) --- */
    double   focus_until_s;                    /* この sim 時刻まで有効。0=無効 */
    double   focus_x0, focus_y0, focus_x1, focus_y1;   /* エリア矩形(m, Payload 由来) */
    double   time_scale;                       /* 実秒→sim 秒換算(robot.c が設定) */

    /* --- 修理(病院で置き去り→復活) --- */
    double   repair_deadline_s;                /* casualty: この sim 時刻で復活。0=未搬送 */
    double   no_fail_until_s;                  /* この sim 時刻までは自動故障の抽選対象外 */
} robot_t;

/* 初期化。portal は open 済を渡す。zone/座標/peers は supervisor 引数(PROTOCOL §5.3)。 */
void robot_init(robot_t *r, ccnx_portal_t *portal, int id, const char *zone,
                double x, double y, double field_w, double field_h,
                int n_robots, const uint16_t *peers, int n_peers, uint16_t my_port);

/* プレフィックス登録 + FIB 経路 + 既定 on_content を結ぶ。成功 0 / 失敗 負値。 */
int  robot_register(robot_t *r);

/* 1 反復ぶんの挙動更新(状態機械 + 移動)。sim_dt_s は倍率適用後の経過秒。 */
void robot_update(robot_t *r, double sim_dt_s);

/* 救助対象化: 停止させ ST_NEEDS_RESCUE へ(プロセスは生かす)。
 * robot.c が SIGUSR1 受信フラグを検知して 1 反復に 1 回呼ぶ。
 * carrier に病院へ運ばれ /robot/{id}/repaired を on_content で受けると復活する。 */
void robot_become_casualty(robot_t *r);

#endif /* ROBOT_BEHAVIOR_H */
