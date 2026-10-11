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

/* Yaw and pitch in degrees a second, with the resting offset removed.
 *
 * In player space the yaw component is taken around gravity rather than from
 * one fixed axis, so holding the pad tilted still turns the camera level.
 * Gravity comes from the accelerometer, which is only trustworthy when the
 * pad is not being shaken, hence the magnitude check. */
static INLINE void rates(const ga_args_t *a, const float av[3],
                         const float ac[3], float *out_yaw, float *out_pitch)
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

    *out_yaw = yaw * a->k.rad_to_deg * a->invert_yaw;
    *out_pitch = pitch * a->k.rad_to_deg * a->invert_pitch;
}

/* Tremor handling: pull very slow rates toward zero, and blend slow rates
 * with a short running average so fine aim is steady without blunting a
 * flick. */
static INLINE void steady(ga_args_t *a, float *yaw, float *pitch)
{
    float mag = __builtin_sqrtf(*yaw * *yaw + *pitch * *pitch);

    if (a->tighten > a->k.eps && mag < a->tighten && mag > a->k.zero) {
        float scale = mag / a->tighten;
        *yaw = *yaw * scale;
        *pitch = *pitch * scale;
    }

    uint32_t i = a->ring_i % GA_RING_N;
    a->ring[i][0] = *yaw;
    a->ring[i][1] = *pitch;
    a->ring_i = a->ring_i + 1;

    if (a->smooth_rate <= a->k.eps)
        return;

    float sy = a->k.zero, sp = a->k.zero;
    for (int n = 0; n < GA_RING_N; n++) {
        sy = sy + a->ring[n][0];
        sp = sp + a->ring[n][1];
    }
    sy = sy * a->k.ring_inv;
    sp = sp * a->k.ring_inv;

    /* Fully smoothed at half the threshold, fully direct at the threshold. */
    float half = a->smooth_rate * a->k.half;
    float direct = fclamp(a, (mag - half) / (a->smooth_rate - half),
                          a->k.zero, a->k.one);
    *yaw = sy + (*yaw - sy) * direct;
    *pitch = sp + (*pitch - sp) * direct;
}

/* Camera speed asked of the game, in degrees a second. Acceleration is a ramp
 * between two sensitivities over a band of hand speeds. */
static INLINE void to_camera(const ga_args_t *a, float yaw, float pitch,
                            float *cy, float *cp)
{
    float mag = __builtin_sqrtf(yaw * yaw + pitch * pitch);
    float sens = a->sens_min;

    if (a->sens_hi_rate > a->sens_lo_rate) {
        float t = fclamp(a, (mag - a->sens_lo_rate) /
                             (a->sens_hi_rate - a->sens_lo_rate),
                         a->k.zero, a->k.one);
        sens = a->sens_min + (a->sens_max - a->sens_min) * t;
    }

    *cy = yaw * sens;
    *cp = pitch * sens * a->vert_scale;
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

static INLINE void transform(ga_args_t *a, uint8_t *data, int32_t handle)
{
    uint32_t age = __atomic_add_fetch(&a->age, 1u, __ATOMIC_RELAXED);
    float av[3], ac[3];

    for (int i = 0; i < 3; i++) {
        av[i] = load_float(data + GA_OFF_ANGVEL + 4 * i);
        ac[i] = load_float(data + GA_OFF_ACCEL + 4 * i);
        a->last_angvel[i] = av[i];
        a->last_accel[i] = ac[i];
    }

    int xoff = GA_OFF_RX, yoff = GA_OFF_RY;
    if (a->target == GA_TARGET_LEFT_STICK) {
        xoff = GA_OFF_LX;
        yoff = GA_OFF_LY;
    }

    a->last_handle = handle;
    a->last_buttons = load_u32(data + GA_OFF_BUTTONS);
    a->last_l2 = data[GA_OFF_L2];
    a->last_r2 = data[GA_OFF_R2];
    a->in_rx = data[xoff];
    a->in_ry = data[yoff];
    a->connected = data[GA_OFF_CONNECTED];

    uint64_t ts = load_u64(data + GA_OFF_TIMESTAMP);
    a->last_timestamp = ts;

    int active = a->enabled && age <= GA_STALE_READS && is_active(a, data);

    feed_calibration(a, av, ac, active);

    /* Time since the previous distinct sample. A game may read the pad more
     * than once for the same sample, and carried motion must not be counted
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

    float yaw, pitch;
    rates(a, av, ac, &yaw, &pitch);
    a->rate_yaw = yaw;
    a->rate_pitch = pitch;

    if (!active) {
        a->active = 0;
        a->saturated = 0;
        a->cam_yaw = a->k.zero;
        a->cam_pitch = a->k.zero;
        a->out_frac = a->k.zero;
        a->out_rx = data[xoff];
        a->out_ry = data[yoff];
        /* Leaving the carry and the ring behind would deliver a stale flick
         * the next time aiming starts. */
        a->carry[0] = a->k.zero;
        a->carry[1] = a->k.zero;
        for (int n = 0; n < GA_RING_N; n++) {
            a->ring[n][0] = a->k.zero;
            a->ring[n][1] = a->k.zero;
        }
        /* Letting go must stop the motors, or they stay on at whatever level
         * the last aiming frame asked for. */
        if (a->rumble_last_level && fresh)
            set_rumble(a, handle, 0, ts);
        __atomic_add_fetch(&a->sample_seq, 1u, __ATOMIC_RELAXED);
        return;
    }

    if (fresh)
        steady(a, &yaw, &pitch);

    float cam_yaw, cam_pitch;
    to_camera(a, yaw, pitch, &cam_yaw, &cam_pitch);
    a->cam_yaw = cam_yaw;
    a->cam_pitch = cam_pitch;

    /* What the physical stick is already asking the camera to do. Running it
     * through the game's own curve first is what lets the two be added
     * without the curve landing on the gyro's contribution twice.
     *
     * Mind the two coordinate systems. A stick's vertical axis grows
     * downward, so a value above centre means pushed down; camera pitch grows
     * upward, so positive means looking up. The stick's vertical contribution
     * is therefore subtracted, not added. Getting this wrong inverts the
     * right stick's up and down, but only while the gyro is engaged, since
     * the sample passes through untouched otherwise. */
    float vx = cam_yaw, vy = cam_pitch;
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

    /* A flick can ask for more than the camera can do. Hold the excess back
     * in stick-seconds and pay it out once the demand drops, so the turn
     * still arrives; moving back the other way cancels it. */
    float want = __builtin_sqrtf(vx * vx + vy * vy);
    float frac = a->max_turn > a->k.eps ? want / a->max_turn : a->k.zero;

    if (dt > a->k.zero && a->carry_cap > a->k.zero) {
        float dirx = want > a->k.eps ? vx / want : a->k.zero;
        float diry = want > a->k.eps ? vy / want : a->k.zero;

        if (frac > a->k.one) {
            a->carry[0] = a->carry[0] + dirx * (frac - a->k.one) * dt;
            a->carry[1] = a->carry[1] + diry * (frac - a->k.one) * dt;
            frac = a->k.one;
        } else {
            float cm = __builtin_sqrtf(a->carry[0] * a->carry[0] +
                                       a->carry[1] * a->carry[1]);
            if (cm > a->k.eps) {
                float room = a->k.one - frac;
                float pay = cm / dt;
                if (pay > room)
                    pay = room;
                vx = vx + a->carry[0] / cm * pay * a->max_turn;
                vy = vy + a->carry[1] / cm * pay * a->max_turn;
                float scale = (cm - pay * dt) / cm;
                if (scale < a->k.zero)
                    scale = a->k.zero;
                a->carry[0] = a->carry[0] * scale;
                a->carry[1] = a->carry[1] * scale;
                want = __builtin_sqrtf(vx * vx + vy * vy);
                frac = want / a->max_turn;
            }
        }

        float cm = __builtin_sqrtf(a->carry[0] * a->carry[0] +
                                   a->carry[1] * a->carry[1]);
        if (cm > a->carry_cap && cm > a->k.eps) {
            float s = a->carry_cap / cm;
            a->carry[0] = a->carry[0] * s;
            a->carry[1] = a->carry[1] * s;
        }
    }

    a->saturated = frac >= a->k.one ? 1 : 0;
    frac = fclamp(a, frac, a->k.zero, a->k.one);

    /* Back to a deflection, through the inverse of the game's own curve so
     * the camera actually moves at the speed asked for. */
    float out = frac > a->k.eps ? lut(a, a->lut_to_stick, frac) : a->k.zero;
    a->out_frac = out;

    int nx = 128, ny = 128;
    if (out > a->k.eps && want > a->k.eps) {
        /* Clamped here, in float, rather than left to the integer clamp
         * below: letting the compiler fold the two gave it an excuse to park
         * the limit in a constant pool. */
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
    a->out_rx = bx;
    a->out_ry = by;
    a->active = 1;

    /* Rumble that rises with how hard the camera is being turned. Rate
     * limited, and only sent when the level actually changes, because this
     * calls back into Sony's library from the game's own thread. */
    if (a->rumble && fresh) {
        uint32_t level = (uint32_t)(frac * a->rumble_scale * a->k.full);
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
