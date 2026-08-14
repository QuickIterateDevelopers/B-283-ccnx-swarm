/*
 * movement.c - movement.h の実装(純幾何・ドメイン非依存)。
 */
#include "movement.h"
#include <math.h>

double vec2_dist(vec2_t a, vec2_t b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

void movement_init(movement_t *m, double x, double y, double field_w, double field_h) {
    m->pos.x = x;
    m->pos.y = y;
    m->target.x = x;
    m->target.y = y;
    m->has_target = 0;
    m->heading = 0.0;
    m->field_w = field_w > 0 ? field_w : 40.0;
    m->field_h = field_h > 0 ? field_h : 30.0;
}

void movement_set_target(movement_t *m, double tx, double ty) {
    m->target.x = tx;
    m->target.y = ty;
    m->has_target = 1;
}

void movement_clear_target(movement_t *m) {
    m->has_target = 0;
}

int movement_has_target(const movement_t *m) {
    return m->has_target;
}

double movement_dist_to(const movement_t *m, double x, double y) {
    double dx = m->pos.x - x, dy = m->pos.y - y;
    return sqrt(dx * dx + dy * dy);
}

int movement_arrived(const movement_t *m, double eps) {
    if (!m->has_target) return 1;
    return vec2_dist(m->pos, m->target) <= eps;
}

static double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void movement_step(movement_t *m, double dt_s, double speed) {
    if (!m->has_target || dt_s <= 0.0 || speed <= 0.0) return;

    double dx = m->target.x - m->pos.x;
    double dy = m->target.y - m->pos.y;
    double dist = sqrt(dx * dx + dy * dy);
    if (dist <= 1e-9) return;

    m->heading = atan2(dy, dx);

    double step = speed * dt_s;
    if (step >= dist) {
        m->pos = m->target;        /* 到達: スナップ */
    } else {
        m->pos.x += dx / dist * step;
        m->pos.y += dy / dist * step;
    }
    /* フィールド外に出ないようクランプ */
    m->pos.x = clampd(m->pos.x, 0.0, m->field_w);
    m->pos.y = clampd(m->pos.y, 0.0, m->field_h);
}
