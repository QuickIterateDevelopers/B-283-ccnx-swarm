/*
 * movement.h - B層: ロボの運動学(位置・目標・移動)のみ。
 *
 * B-283 CCNx セマンティック群ロボ通信デモ / C B層。
 * ドメイン非依存の純幾何。CCNx(ccnx.h)も名前も知らない。behavior.c から呼ばれる。
 * 座標系は PROTOCOL.md §0: 40m(x) × 30m(y)、原点(0,0)=左上、x 右・y 下が正、単位 m。
 */
#ifndef ROBOT_MOVEMENT_H
#define ROBOT_MOVEMENT_H

typedef struct { double x, y; } vec2_t;

typedef struct {
    vec2_t pos;
    vec2_t target;
    int    has_target;
    double heading;          /* rad, 直近の進行方位(RF pose の hd) */
    double field_w, field_h; /* フィールド寸法(はみ出しクランプ用) */
} movement_t;

/* 速度・許容誤差(PROTOCOL §2: 公称1.5 / 搬送0.8 m/s) */
#define ROBOT_SPEED_NOMINAL  1.5   /* m/s */
#define ROBOT_SPEED_CARRY    0.8   /* m/s 協力搬送 */
#define ROBOT_ARRIVE_EPS     0.25  /* m 到達判定 */

void   movement_init(movement_t *m, double x, double y, double field_w, double field_h);
void   movement_set_target(movement_t *m, double tx, double ty);
void   movement_clear_target(movement_t *m);
int    movement_has_target(const movement_t *m);
int    movement_arrived(const movement_t *m, double eps);

/* 目標へ speed*dt_s だけ前進。フィールド内にクランプ。heading を更新。
 * 目標未設定なら何もしない(その場停止)。 */
void   movement_step(movement_t *m, double dt_s, double speed);

double movement_dist_to(const movement_t *m, double x, double y);
double vec2_dist(vec2_t a, vec2_t b);

#endif /* ROBOT_MOVEMENT_H */
