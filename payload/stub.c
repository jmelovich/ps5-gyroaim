/* SPDX-License-Identifier: GPL-3.0-or-later
 * stub.c - the transform, as it runs inside the game.
 *
 * This code is compiled here but executed in another process: the service
 * copies the whole block below into a mapping inside the game and points one
 * of the game's import slots at it. Three rules follow, and breaking any of
 * them crashes the game rather than failing politely:
 *
 *   1. No calls out. Sony's functions are reached only through a->orig[].
 *      No libc, no libm, no helper in another file.
 *   2. No references to anything outside the block. In particular no float or
 *      double literals, no string literals, and no negating a float, because
 *      each of those puts a constant in .rodata that the compiler reaches
 *      with a RIP-relative load, pointing into open space once copied. Every
 *      constant lives in a->k, and signs are carried in the settings.
 *   3. Everything stays in section .text.gastub, ahead of ga_stub_end, so the
 *      service can find the block's bounds and copy it whole.
 *
 * `make check-stub` fails the build if any of this leaks.
 *
 * What the transform does, and why it is shaped this way: a stick is not a
 * direction, it is a request for a camera speed, and the game puts whatever
 * it receives through its own deadzone and response curve. So the gyro's
 * rate is converted into the camera speed it should produce, combined with
 * whatever the physical stick is already asking for, and only then turned
 * back into the deflection that makes the game produce that speed. Working
 * in speed rather than deflection is what stops the game's curve being
 * applied twice.
 */
#include "stub.h"

#define STUB __attribute__((noinline, used, section(".text.gastub")))
#define INLINE __attribute__((always_inline)) inline

static INLINE float load_float(const uint8_t *p)
{
    float v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static INLINE uint32_t load_u32(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static INLINE uint64_t load_u64(const uint8_t *p)
{
    uint64_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static INLINE float fclamp(const ga_args_t *a, float v, float lo, float hi)
{
    (void)a;
    return v < lo ? lo : v > hi ? hi : v;
}

static INLINE float fabs_(const ga_args_t *a, float v)
{
    return v < a->k.zero ? a->k.zero - v : v;
}

/* 1..255 with 128 as centre. Zero is avoided because some games read it as
 * full deflection the other way. */
static INLINE uint8_t to_axis(int v)
{
    if (v < 1)
        return 1;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/* Linear interpolation into one of the sampled curves. */
static INLINE float lut(const ga_args_t *a, const volatile float *table,
                        float x)
{
    float scaled;
    int i;

    x = fclamp(a, x, a->k.zero, a->k.one);
    scaled = x * a->k.lut_scale;
    i = (int)scaled;
    if (i >= GA_LUT_LAST)
        return table[GA_LUT_LAST];
    return table[i] + (table[i + 1] - table[i]) * (scaled - (float)i);
}

static INLINE int is_active(const ga_args_t *a, const uint8_t *data)
{
    uint32_t buttons = load_u32(data + GA_OFF_BUTTONS);

    switch (a->activation) {
    case GA_ACT_ALWAYS:
        return 1;
    case GA_ACT_BUTTON:
        return (buttons & a->activation_mask) != 0;
    case GA_ACT_TRIGGER: {
        uint8_t t = a->trigger_right ? data[GA_OFF_R2] : data[GA_OFF_L2];
        return (uint32_t)t >= a->trigger_threshold;
    }
    default:
        return 0;
    }
}

/* Accumulates the stillness window the service uses to learn the gyro's
 * resting offset. */
static INLINE void feed_calibration(ga_args_t *a, const float av[3],
                                    const float ac[3], int active)
{
    if (a->cal_seen != a->cal_epoch) {
        a->cal_seen = a->cal_epoch;
        a->cal_count = 0;
        a->cal_was_active = 0;
        for (int i = 0; i < 3; i++) {
            a->cal_sum[i] = a->k.zero;
            a->cal_min[i] = av[i];
            a->cal_max[i] = av[i];
            a->cal_amin[i] = ac[i];
            a->cal_amax[i] = ac[i];
        }
    }

    for (int i = 0; i < 3; i++) {
        a->cal_sum[i] = a->cal_sum[i] + av[i];
        if (av[i] < a->cal_min[i])
            a->cal_min[i] = av[i];
        if (av[i] > a->cal_max[i])
            a->cal_max[i] = av[i];
        if (ac[i] < a->cal_amin[i])
            a->cal_amin[i] = ac[i];
        if (ac[i] > a->cal_amax[i])
            a->cal_amax[i] = ac[i];
    }
    if (active)
        a->cal_was_active = 1;
    a->cal_count = a->cal_count + 1;
}

/* The three rotations in degrees a second, with the resting offset removed.
 *
 * In player space the yaw component is taken around gravity rather than from
 * one fixed axis, so holding the pad tilted still turns the camera level.
 * Gravity comes from the accelerometer, which is only trustworthy when the
 * pad is not being shaken, hence the magnitude check. */
static INLINE void rates(const ga_args_t *a, const float av[3],
                         const float ac[3], float out[GA_SRC_COUNT],
                         float local[GA_SRC_COUNT])
{
    uint32_t iy = a->axis_yaw > 2 ? 1 : a->axis_yaw;
    uint32_t ip = a->axis_pitch > 2 ? 0 : a->axis_pitch;
    uint32_t ir = a->axis_roll > 2 ? 2 : a->axis_roll;

    float w[3];
    for (int i = 0; i < 3; i++)
        w[i] = av[i] - a->bias[i];

    float yaw = w[iy];
    float pitch = w[ip];

    if (a->space == GA_SPACE_PLAYER) {
        float g = __builtin_sqrtf(ac[0] * ac[0] + ac[1] * ac[1] +
                                  ac[2] * ac[2]);
        float lo = a->k.half + a->k.half * a->k.half;   /* 0.75 */
        float hi = a->k.one + a->k.half * a->k.half;    /* 1.25 */
        if (g > lo && g < hi) {
            /* Gravity, normalised. The yaw axis is whatever points along it,
             * so project the rate onto it and keep the sign convention of the
             * configured yaw axis. */
            float gy = ac[iy] / g;
            float gr = ac[ir] / g;
            float along = w[iy] * gy + w[ir] * gr;
            float len = __builtin_sqrtf(w[iy] * w[iy] + w[ir] * w[ir]);
            /* Relaxing the projection keeps full yaw for a pad held within
             * about 45 degrees of flat or upright. */
            float relaxed = fabs_(a, along) * (a->k.one + a->k.half - a->k.eps);
            if (relaxed > len)
                relaxed = len;
            yaw = along < a->k.zero ? a->k.zero - relaxed : relaxed;
        }
    }

    out[GA_SRC_YAW] = yaw * a->k.rad_to_deg;
    out[GA_SRC_PITCH] = pitch * a->k.rad_to_deg;
    out[GA_SRC_ROLL] = w[ir] * a->k.rad_to_deg;

    /* The plain rotation about each axis, with none of the gravity-referenced
     * reshaping above. A remembered angle is built from these: the reshaping
     * is a feel adjustment for aiming and is not something whose integral
     * means anything, so integrating it made turning out and back fail to
     * cancel. */
    local[GA_SRC_YAW] = w[iy] * a->k.rad_to_deg;
    local[GA_SRC_PITCH] = w[ip] * a->k.rad_to_deg;
    local[GA_SRC_ROLL] = w[ir] * a->k.rad_to_deg;
}

/* Tremor handling: pull very slow rotation toward zero, and blend slow
 * rotation with a short running average so fine aim is steady without
 * blunting a flick. Applied to the rotations rather than to any one output,
 * so it behaves the same however they are routed. */
static INLINE void steady(ga_args_t *a, float rate[GA_SRC_COUNT])
{
    float mag = a->k.zero;

    for (int i = 0; i < GA_SRC_COUNT; i++)
        mag = mag + rate[i] * rate[i];
    mag = __builtin_sqrtf(mag);

    if (a->tighten > a->k.eps && mag < a->tighten && mag > a->k.zero) {
        float scale = mag / a->tighten;
        for (int i = 0; i < GA_SRC_COUNT; i++)
            rate[i] = rate[i] * scale;
    }

    uint32_t slot = a->ring_i % GA_RING_N;
    for (int i = 0; i < GA_SRC_COUNT; i++)
        a->ring[slot][i] = rate[i];
    a->ring_i = a->ring_i + 1;

    if (a->smooth_rate <= a->k.eps)
        return;

    /* Fully smoothed at half the threshold, fully direct at the threshold. */
    float half = a->smooth_rate * a->k.half;
    float direct = fclamp(a, (mag - half) / (a->smooth_rate - half),
                          a->k.zero, a->k.one);

    for (int i = 0; i < GA_SRC_COUNT; i++) {
        float mean = a->k.zero;
        for (int n = 0; n < GA_RING_N; n++)
            mean = mean + a->ring[n][i];
        mean = mean * a->k.ring_inv;
        rate[i] = mean + (rate[i] - mean) * direct;
    }
}

/* How much everything is multiplied by, given how fast the hands are moving.
 * Only the rotations that are actually routed count toward the speed, so an
 * unused axis cannot trip the acceleration. */
static INLINE float accel_factor(const ga_args_t *a,
                                 const float rate[GA_SRC_COUNT])
{
    float mag = a->k.zero;

    if (a->sens_hi_rate <= a->sens_lo_rate)
        return a->k.one;

    for (int i = 0; i < GA_SRC_COUNT; i++) {
        int used = 0;
        for (int d = 0; d < GA_OUT_COUNT; d++)
            if (a->route[i][d] != a->k.zero)
                used = 1;
        if (used)
            mag = mag + rate[i] * rate[i];
    }
    mag = __builtin_sqrtf(mag);

    float t = fclamp(a, (mag - a->sens_lo_rate) /
                         (a->sens_hi_rate - a->sens_lo_rate),
                     a->k.zero, a->k.one);
    return a->k.one + (a->accel_mul - a->k.one) * t;
}

/* Rumble.
 *
 * The DualSense has no rotary motors, only voice coils, so scePadSetVibration
 * does nothing at all until the pad is put into legacy motor emulation with
 * scePadSetVibrationMode(handle, 2). That is why an earlier version produced
 * no rumble whatsoever however the settings were changed.
 *
 * The mode is re-applied every couple of seconds while rumble is wanted,
 * because the game sets the mode too and whichever call came last wins.
 */
static INLINE void set_rumble(ga_args_t *a, int32_t handle, uint32_t level,
                              uint64_t ts)
{
    typedef int32_t (*vib_t)(int32_t, const uint8_t *);
    typedef int32_t (*mode_t)(int32_t, int32_t);

    if (!a->orig[GA_FN_SET_VIBRATION])
        return;

    if (a->orig[GA_FN_SET_VIBRATION_MODE] &&
        (a->rumble_mode_ts == 0 || ts - a->rumble_mode_ts > 2000000u)) {
        a->rumble_mode_ts = ts;
        ((mode_t)(uintptr_t)a->orig[GA_FN_SET_VIBRATION_MODE])(handle, 2);
    }

    uint8_t motors[2];
    motors[0] = (uint8_t)level;
    motors[1] = (uint8_t)level;
    a->rumble_last_level = level;
    a->rumble_last_ts = ts;
    ((vib_t)(uintptr_t)a->orig[GA_FN_SET_VIBRATION])(handle, motors);
}

/* Turns one stick's wanted camera speed into a deflection and writes it.
 *
 * Each stick is handled as a pair, because the game's deadzone and response
 * curve act on how far the stick is pushed, not on one axis at a time. A stick
 * with nothing routed to it is left exactly as the game delivered it.
 *
 * Returns how hard this stick ended up being pushed, 0 to 1.
 */
static INLINE float drive_stick(ga_args_t *a, uint8_t *data, unsigned stick,
                                float vx, float vy, float dt)
{
    int xoff = stick ? GA_OFF_LX : GA_OFF_RX;
    int yoff = stick ? GA_OFF_LY : GA_OFF_RY;

    /* What the hands are already asking for through the stick itself. Running
     * it through the game's own curve first is what lets the two be added
     * without the curve landing on the gyro's contribution twice.
     *
     * Mind the two frames: a stick's vertical axis grows downward, while the
     * camera's grows upward, so the stick's vertical part is subtracted. */
    if (a->merge_stick) {
        float sx = (float)((int)data[xoff] - 128) / a->k.counts;
        float sy = (float)((int)data[yoff] - 128) / a->k.counts;
        float sm = __builtin_sqrtf(sx * sx + sy * sy);
        float dz = (float)a->stick_deadzone / a->k.counts;
        if (sm > dz && sm > a->k.eps) {
            float v = lut(a, a->lut_to_vel, sm) * a->max_turn;
            vx = vx + sx / sm * v;
            vy = vy - sy / sm * v;
        }
    }

    /* A flick can ask for more than the game can turn. Hold the excess back in
     * stick-seconds and pay it out once the demand drops, so the movement
     * still arrives; going back the other way cancels it. */
    float want = __builtin_sqrtf(vx * vx + vy * vy);
    float frac = a->max_turn > a->k.eps ? want / a->max_turn : a->k.zero;

    if (dt > a->k.zero && a->carry_cap > a->k.zero) {
        float dirx = want > a->k.eps ? vx / want : a->k.zero;
        float diry = want > a->k.eps ? vy / want : a->k.zero;

        if (frac > a->k.one) {
            a->carry[stick][0] = a->carry[stick][0] +
                                 dirx * (frac - a->k.one) * dt;
            a->carry[stick][1] = a->carry[stick][1] +
                                 diry * (frac - a->k.one) * dt;
            frac = a->k.one;
        } else {
            float cm = __builtin_sqrtf(a->carry[stick][0] * a->carry[stick][0] +
                                       a->carry[stick][1] * a->carry[stick][1]);
            if (cm > a->k.eps) {
                float room = a->k.one - frac;
                float pay = cm / dt;
                if (pay > room)
                    pay = room;
                vx = vx + a->carry[stick][0] / cm * pay * a->max_turn;
                vy = vy + a->carry[stick][1] / cm * pay * a->max_turn;
                float scale = (cm - pay * dt) / cm;
                if (scale < a->k.zero)
                    scale = a->k.zero;
                a->carry[stick][0] = a->carry[stick][0] * scale;
                a->carry[stick][1] = a->carry[stick][1] * scale;
                want = __builtin_sqrtf(vx * vx + vy * vy);
                frac = want / a->max_turn;
            }
        }

        float cm = __builtin_sqrtf(a->carry[stick][0] * a->carry[stick][0] +
                                   a->carry[stick][1] * a->carry[stick][1]);
        if (cm > a->carry_cap && cm > a->k.eps) {
            float s = a->carry_cap / cm;
            a->carry[stick][0] = a->carry[stick][0] * s;
            a->carry[stick][1] = a->carry[stick][1] * s;
        }
    }

    float clamped = fclamp(a, frac, a->k.zero, a->k.one);

    /* Back to a deflection, through the inverse of the game's own curve, so
     * the view actually moves at the speed asked for. */
    float out = clamped > a->k.eps ? lut(a, a->lut_to_stick, clamped)
                                   : a->k.zero;
    int nx = 128, ny = 128;

    if (out > a->k.eps && want > a->k.eps) {
        float px = fclamp(a, vx / want * out * a->k.counts,
                          a->k.neg_counts, a->k.counts);
        float py = fclamp(a, vy / want * out * a->k.counts,
                          a->k.neg_counts, a->k.counts);
        nx = 128 + (int)px;
        /* Screen y grows downward, so looking up is the smaller value. */
        ny = 128 - (int)py;
    }

    uint8_t bx = to_axis(nx);
    uint8_t by = to_axis(ny);
    data[xoff] = bx;
    data[yoff] = by;

    if (stick) {
        a->out_lx = bx;
        a->out_ly = by;
    } else {
        a->out_rx = bx;
        a->out_ry = by;
    }
    return frac;
}

static INLINE void transform(ga_args_t *a, uint8_t *data, int32_t handle)
{
    uint32_t age = __atomic_add_fetch(&a->age, 1u, __ATOMIC_RELAXED);
    float av[3], ac[3];
    float rate[GA_SRC_COUNT];
    float local[GA_SRC_COUNT];

    for (int i = 0; i < 3; i++) {
        av[i] = load_float(data + GA_OFF_ANGVEL + 4 * i);
        ac[i] = load_float(data + GA_OFF_ACCEL + 4 * i);
        a->last_angvel[i] = av[i];
        a->last_accel[i] = ac[i];
    }

    a->last_handle = handle;
    a->last_buttons = load_u32(data + GA_OFF_BUTTONS);
    a->last_l2 = data[GA_OFF_L2];
    a->last_r2 = data[GA_OFF_R2];
    a->in_rx = data[GA_OFF_RX];
    a->in_ry = data[GA_OFF_RY];
    a->in_lx = data[GA_OFF_LX];
    a->in_ly = data[GA_OFF_LY];
    a->connected = data[GA_OFF_CONNECTED];

    uint64_t ts = load_u64(data + GA_OFF_TIMESTAMP);
    a->last_timestamp = ts;

    int active = a->enabled && age <= GA_STALE_READS && is_active(a, data);

    /* Taking rest from the moment the condition engages means pressing the
     * trigger says "straight ahead is here", and nothing has time to drift. */
    if (active && !a->was_active && a->rest_on_activate)
        a->rest_epoch = a->rest_epoch + 1;
    a->was_active = active ? 1u : 0u;

    feed_calibration(a, av, ac, active);

    /* Time since the previous distinct sample. A game may read the pad more
     * than once for the same sample, and carried movement must not be counted
     * twice, so anything without a new timestamp advances no state. */
    int fresh = ts != a->last_ts;
    float dt = a->k.zero;
    if (fresh && a->last_ts && ts > a->last_ts) {
        uint64_t us = ts - a->last_ts;
        if (us > 100000u)
            us = 100000u;               /* a gap, not one long frame */
        dt = (float)(uint32_t)us * a->k.us_to_s;
    }
    if (fresh)
        a->last_ts = ts;

    rates(a, av, ac, rate, local);
    for (int i = 0; i < GA_SRC_COUNT; i++)
        a->rate[i] = rate[i];

    if (!active) {
        a->active = 0;
        a->saturated = 0;
        a->out_frac = a->k.zero;
        a->out_rx = data[GA_OFF_RX];
        a->out_ry = data[GA_OFF_RY];
        a->out_lx = data[GA_OFF_LX];
        a->out_ly = data[GA_OFF_LY];
        for (unsigned s = 0; s < GA_STICK_COUNT; s++) {
            a->want[s][0] = a->k.zero;
            a->want[s][1] = a->k.zero;
            /* Leaving the carry behind would deliver a stale flick the next
             * time aiming starts. */
            a->carry[s][0] = a->k.zero;
            a->carry[s][1] = a->k.zero;
        }
        for (int n = 0; n < GA_RING_N; n++)
            for (int i = 0; i < GA_SRC_COUNT; i++)
                a->ring[n][i] = a->k.zero;
        if (a->rumble_last_level && fresh)
            set_rumble(a, handle, 0, ts);
        __atomic_add_fetch(&a->sample_seq, 1u, __ATOMIC_RELAXED);
        return;
    }

    /* The remembered angle is built from the plain rates, before any
     * tightening or smoothing. Those shrink slow movement, which is fine for
     * feel but ruinous for an integral: turning out quickly and back slowly
     * would not cancel. */
    if (a->rest_seen != a->rest_epoch) {
        a->rest_seen = a->rest_epoch;
        for (int i = 0; i < GA_SRC_COUNT; i++)
            a->angle[i] = a->k.zero;
    }
    if (fresh && dt > a->k.zero) {
        for (int i = 0; i < GA_SRC_COUNT; i++) {
            float next = a->angle[i] + local[i] * dt;
            if (a->recentre > a->k.zero) {
                float shed = a->recentre * dt;
                if (shed > a->k.one)
                    shed = a->k.one;
                next = next - next * shed;
            }
            float cap = a->angle_cap[i] > a->k.eps ? a->angle_cap[i]
                                                   : a->k.one;
            a->angle[i] = fclamp(a, next, a->k.zero - cap, cap);
        }
    }
    for (int i = 0; i < GA_SRC_COUNT; i++)
        a->held[i] = a->angle[i];

    if (fresh)
        steady(a, rate);

    float accel = accel_factor(a, rate);
    float busiest = a->k.zero;

    for (unsigned s = 0; s < GA_STICK_COUNT; s++) {
        float vx = a->k.zero, vy = a->k.zero;
        int routed = 0;

        for (int i = 0; i < GA_SRC_COUNT; i++) {
            float gx = a->route[i][GA_OUT_OF(s, 0)];
            float gy = a->route[i][GA_OUT_OF(s, 1)];

            if (gx != a->k.zero || gy != a->k.zero)
                routed = 1;

            /* Both modes end up as a speed, so everything downstream, the
             * merge with the stick and the game's own curve, is shared. In
             * HOLD the speed is whatever asks for the deflection the angle
             * calls for; acceleration has no meaning there and is left out. */
            float scaled;
            if (a->read_mode[i] == GA_READ_HOLD) {
                float limit = a->full_angle[i] > a->k.eps ? a->full_angle[i]
                                                          : a->k.one;
                float f = fclamp(a, a->angle[i] / limit,
                                 a->k.zero - a->k.one, a->k.one);
                float size = f < a->k.zero ? a->k.zero - f : f;

                /* Near rest, nothing at all. Past the band the remainder is
                 * stretched back out, so letting go is smooth rather than a
                 * step. */
                if (size <= a->hold_band) {
                    f = a->k.zero;
                } else if (a->hold_band > a->k.zero) {
                    float span = a->k.one - a->hold_band;
                    float over = (size - a->hold_band) / span;
                    f = f < a->k.zero ? a->k.zero - over : over;
                }
                scaled = f * a->max_turn;
            } else {
                scaled = rate[i] * a->strength[i] * accel;
            }
            vx = vx + gx * scaled;
            vy = vy + gy * scaled;
        }

        a->want[s][0] = vx;
        a->want[s][1] = vy;

        if (!routed) {
            /* Nothing points here, so the game keeps what it delivered. */
            if (s) {
                a->out_lx = data[GA_OFF_LX];
                a->out_ly = data[GA_OFF_LY];
            } else {
                a->out_rx = data[GA_OFF_RX];
                a->out_ry = data[GA_OFF_RY];
            }
            continue;
        }

        float frac = drive_stick(a, data, s, vx, vy, dt);
        if (frac > busiest)
            busiest = frac;
    }

    a->saturated = busiest >= a->k.one ? 1 : 0;
    a->out_frac = fclamp(a, busiest, a->k.zero, a->k.one);
    a->active = 1;

    /* Rumble that rises with how hard the controls are being pushed. Rate
     * limited, and only sent when the level changes, because this calls back
     * into Sony's library from the game's own thread. */
    if (a->rumble && fresh) {
        uint32_t level = (uint32_t)(a->out_frac * a->rumble_scale * a->k.full);
        if (level > 255u)
            level = 255u;
        uint32_t gap = a->rumble_period_ms * 1000u;
        if (level != a->rumble_last_level &&
            (a->rumble_last_ts == 0 || ts - a->rumble_last_ts >= gap))
            set_rumble(a, handle, level, ts);
    } else if (!a->rumble && a->rumble_last_level && fresh) {
        set_rumble(a, handle, 0, ts);      /* switched off mid-turn */
    }

    __atomic_add_fetch(&a->applied, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&a->sample_seq, 1u, __ATOMIC_RELAXED);
}

static INLINE int usable(const ga_args_t *a)
{
    return a && a->magic == GA_MAGIC && a->version == GA_LAYOUT_VERSION;
}

/* scePadReadState(handle, data): the args pointer arrives in rdx. */
STUB int32_t ga_stub_read_state(int32_t handle, uint8_t *data, ga_args_t *a)
{
    typedef int32_t (*fn_t)(int32_t, uint8_t *);
    int32_t r = ((fn_t)(uintptr_t)a->orig[GA_FN_READ_STATE])(handle, data);

    __atomic_add_fetch(&a->calls, 1u, __ATOMIC_RELAXED);
    if (usable(a)) {
        a->last_result = r;
        a->last_handle = handle;
    }
    if (r == 0 && data && usable(a))
        transform(a, data, handle);
    return r;
}

/* scePadReadStateExt(handle, data): args in rdx. */
STUB int32_t ga_stub_read_state_ext(int32_t handle, uint8_t *data,
                                    ga_args_t *a)
{
    typedef int32_t (*fn_t)(int32_t, uint8_t *);
    int32_t r = ((fn_t)(uintptr_t)a->orig[GA_FN_READ_STATE_EXT])(handle, data);

    __atomic_add_fetch(&a->calls, 1u, __ATOMIC_RELAXED);
    if (r == 0 && data && usable(a))
        transform(a, data, handle);
    return r;
}

/* scePadRead(handle, data, count): args in rcx. Returns how many samples it
 * wrote, oldest first, so each gets the same treatment in order. */
STUB int32_t ga_stub_read(int32_t handle, uint8_t *data, int32_t count,
                          ga_args_t *a)
{
    typedef int32_t (*fn_t)(int32_t, uint8_t *, int32_t);
    int32_t r = ((fn_t)(uintptr_t)a->orig[GA_FN_READ])(handle, data, count);

    __atomic_add_fetch(&a->calls, 1u, __ATOMIC_RELAXED);
    if (r > 0 && data && usable(a))
        for (int32_t i = 0; i < r && i < 64; i++)
            transform(a, data + (uint32_t)i * GA_PAD_SIZE, handle);
    return r;
}

/* Marks the end of the block. Must be last in the section. */
STUB void ga_stub_end(void)
{
}
