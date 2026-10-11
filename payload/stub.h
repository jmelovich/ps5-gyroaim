/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "hook.h"

#include <stdint.h>

/* Offsets into the 120-byte pad sample. Spelled out rather than taken from a
 * struct because the stub works on a raw pointer handed to it by the game. */
#define GA_OFF_BUTTONS    0x00
#define GA_OFF_LX         0x04
#define GA_OFF_LY         0x05
#define GA_OFF_RX         0x06
#define GA_OFF_RY         0x07
#define GA_OFF_L2         0x08
#define GA_OFF_R2         0x09
#define GA_OFF_ACCEL      0x1c
#define GA_OFF_ANGVEL     0x28
#define GA_OFF_FINGERS    0x34
#define GA_OFF_TOUCH0     0x3c
#define GA_OFF_CONNECTED  0x4c
#define GA_OFF_TIMESTAMP  0x50
#define GA_PAD_SIZE       120

enum {
    GA_TARGET_RIGHT_STICK = 0,
    GA_TARGET_LEFT_STICK,
    GA_TARGET_TOUCHPAD,
};

enum {
    GA_ACT_ALWAYS = 0,
    GA_ACT_BUTTON,        /* any bit of activation_mask held */
    GA_ACT_TRIGGER,       /* l2/r2 at or past the threshold */
    GA_ACT_NEVER,         /* parked: the transform runs but changes nothing */
};

/* How the gyro's three axes become yaw and pitch. */
enum {
    GA_SPACE_LOCAL = 0,   /* the chosen axes, as they come */
    GA_SPACE_PLAYER,      /* yaw from the component around gravity */
};

#define GA_MAGIC          0x4f524947u   /* "GYRO" */
#define GA_LAYOUT_VERSION 4u

/* Reads without a settings refresh before the stub stops touching the data.
 * If the service dies the game goes back to normal input rather than freezing
 * mid-turn. */
#define GA_STALE_READS    600u

/* Response curves are sampled rather than evaluated, because the stub cannot
 * call powf and cannot reach a constant in .rodata. The service recomputes
 * both tables whenever the curve settings change. */
#define GA_LUT_N          129
#define GA_LUT_LAST       (GA_LUT_N - 1)

/* Smoothing window, in samples. At 60 to 250 reads a second this covers
 * roughly 30 to 130 ms. */
#define GA_RING_N         8

/* Float constants the stub needs. Writing 1.0f in injected code makes the
 * compiler load it from .rodata, which the copied block cannot reach, so the
 * service fills these in instead. */
typedef struct {
    float zero;
    float one;
    float half;
    float counts;         /* 127, full stick travel from centre */
    float centre;         /* 128 */
    float lut_scale;      /* GA_LUT_LAST, for indexing a table from 0..1 */
    float eps;            /* below this a magnitude counts as nothing */
    float rad_to_deg;     /* 57.29578 */
    float us_to_s;        /* 1e-6, so microseconds need no division */
    float ring_inv;       /* 1 / GA_RING_N */
    float neg_counts;     /* -127, so the stub never negates a float */
    float full;           /* 255, full motor strength */
} ga_consts_t;

/* The page the service and the stub share. The stub references nothing
 * outside this struct and its own code. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t orig[GA_FN_COUNT];      /* Sony's real entry points */
    ga_consts_t k;

    /* ---- settings: service writes, stub reads ---- */
    volatile uint32_t enabled;
    volatile uint32_t target;            /* GA_TARGET_* */
    volatile uint32_t activation;        /* GA_ACT_* */
    volatile uint32_t activation_mask;
    volatile uint32_t trigger_right;
    volatile uint32_t trigger_threshold;
    volatile uint32_t space;             /* GA_SPACE_* */
    volatile uint32_t axis_yaw;          /* index into angularVelocity */
    volatile uint32_t axis_pitch;
    volatile uint32_t axis_roll;

    /* Sensitivity, in camera degrees per degree of hand movement. Two values
     * with a speed band between them give the usual acceleration: slow
     * movement gets min_sens for fine aim, fast movement gets max_sens.
     * Equal values mean no acceleration. The sign carries axis inversion,
     * because the stub must not negate a float. */
    volatile float sens_min;
    volatile float sens_max;
    volatile float sens_lo_rate;         /* deg/s where min applies */
    volatile float sens_hi_rate;         /* deg/s where max applies */
    volatile float vert_scale;           /* pitch relative to yaw */
    volatile float invert_yaw;           /* +1 or -1 */
    volatile float invert_pitch;

    /* The game's own camera. max_turn is how fast it spins at full stick, in
     * degrees a second; it is what makes sensitivity mean anything. */
    volatile float max_turn;

    /* Tremor handling. Rates under tighten are pulled toward zero rather than
     * cut off, and rates under smooth_rate are blended with a short average,
     * so slow movement is steady while flicks stay instant. */
    volatile float tighten;
    volatile float smooth_rate;

    /* A flick asking for more than full deflection has the excess held back
     * and delivered just after, so the turn still completes. In stick-seconds.
     */
    volatile float carry_cap;

    volatile float bias[3];              /* rad/s, removed before scaling */
    volatile uint32_t stick_deadzone;    /* physical stick counts from centre */
    volatile uint32_t merge_stick;       /* combine with the physical stick */

    /* Rumble that rises with turn rate, so the gyro can be felt working. */
    volatile uint32_t rumble;
    volatile float    rumble_scale;
    volatile uint32_t rumble_period_ms;

    /* Sampled curves. to_vel turns a stick deflection into the camera speed
     * the game will produce; to_stick is its inverse. Both are fractions of
     * full travel and full speed. */
    volatile float lut_to_vel[GA_LUT_N];
    volatile float lut_to_stick[GA_LUT_N];

    volatile uint32_t cal_epoch;         /* bumped to restart the window */
    volatile uint32_t settings_seq;

    /* ---- stillness window: stub fills, service judges ---- *
     *
     * The service decides when the pad is still, because that needs history
     * and a clock. The stub only accumulates. A slow steady turn around
     * gravity looks exactly like stillness to the accelerometer, so the
     * service also refuses to trust a window in which aiming was active.
     */
    volatile uint32_t cal_seen;          /* the epoch this window started on */
    volatile uint32_t cal_count;
    volatile uint32_t cal_was_active;
    volatile float    cal_sum[3];
    volatile float    cal_min[3];
    volatile float    cal_max[3];
    volatile float    cal_amin[3];
    volatile float    cal_amax[3];

    /* ---- per-pad running state, owned by the stub ---- */
    volatile float    ring[GA_RING_N][2];
    volatile uint32_t ring_i;
    volatile float    carry[2];
    volatile uint64_t last_ts;
    volatile uint32_t rumble_last_level;
    volatile uint64_t rumble_last_ts;
    volatile uint64_t rumble_mode_ts;

    /* ---- telemetry: stub writes, service reads ---- */
    volatile uint64_t calls;
    volatile uint64_t applied;
    volatile uint32_t sample_seq;
    volatile uint32_t age;
    volatile int32_t  last_handle;
    volatile uint32_t last_buttons;
    volatile float    last_angvel[3];
    volatile float    last_accel[3];
    volatile float    rate_yaw;          /* deg/s, after bias and space */
    volatile float    rate_pitch;
    volatile float    cam_yaw;           /* deg/s asked of the camera */
    volatile float    cam_pitch;
    volatile float    out_frac;          /* final deflection, 0..1 */
    volatile uint8_t  in_rx, in_ry, out_rx, out_ry;
    volatile uint8_t  last_l2, last_r2;
    volatile uint8_t  active;
    volatile uint8_t  connected;
    volatile uint8_t  saturated;
    volatile int32_t  last_result;       /* what Sony's read returned */
    volatile uint64_t last_timestamp;

    /* ---- how to put the game back ---- */
    uint32_t hook_count;
    uint64_t hook_slot[GA_MAX_SLOTS];
    uint64_t hook_orig[GA_MAX_SLOTS];
    uint32_t hook_prot[GA_MAX_SLOTS];
} ga_args_t;

int32_t ga_stub_read_state(int32_t handle, uint8_t *data, ga_args_t *a);
int32_t ga_stub_read_state_ext(int32_t handle, uint8_t *data, ga_args_t *a);
int32_t ga_stub_read(int32_t handle, uint8_t *data, int32_t count,
                     ga_args_t *a);
void    ga_stub_end(void);
