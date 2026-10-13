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

/* What the gyro can drive. Two sticks, two axes each, addressed as a flat
 * list so the routing table below is a plain matrix. */
enum {
    GA_OUT_RIGHT_X = 0,
    GA_OUT_RIGHT_Y,
    GA_OUT_LEFT_X,
    GA_OUT_LEFT_Y,
    GA_OUT_COUNT
};

#define GA_STICK_COUNT 2
#define GA_OUT_OF(stick, axis) ((stick) * 2 + (axis))

/* How a rotation is read.
 *
 * A gyroscope measures how fast it is turning, not how far it has turned, so
 * on its own it can only say "moving left now". Held still at an angle it
 * reads zero and the control returns to centre, and turning back to rest
 * reads the opposite way. That is right for aiming, where the hands ratchet
 * and the view should stay put between movements, and wrong for steering,
 * where a held tilt should mean a held input.
 *
 * So each rotation is read one of two ways. FLOW is the rate itself. HOLD
 * integrates it into an angle away from a remembered resting position, and
 * the control stays pushed for as long as the pad is held there.
 */
enum {
    GA_READ_FLOW = 0,   /* speed of turning, for aiming */
    GA_READ_HOLD,       /* angle away from rest, for steering */
};

/* What the gyro offers. Rotations rather than raw sensor axes, because a
 * person thinks in turning, tilting and leaning, and because the gravity-
 * referenced spaces are defined in those terms. */
enum {
    GA_SRC_YAW = 0,     /* turning left and right */
    GA_SRC_PITCH,       /* tilting up and down */
    GA_SRC_ROLL,        /* leaning side to side */
    GA_SRC_COUNT
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
    float half_turn;      /* 180, for the far side of an arcsine */
    float right_angle;    /* 90 */
    float quarter;        /* 0.25, the least gravity an axis needs to see */
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
    volatile uint32_t activation;        /* GA_ACT_* */
    volatile uint32_t activation_mask;
    volatile uint32_t trigger_right;
    volatile uint32_t trigger_threshold;
    volatile uint32_t space;             /* GA_SPACE_* */
    volatile uint32_t axis_yaw;          /* index into angularVelocity */
    volatile uint32_t axis_pitch;
    volatile uint32_t axis_roll;

    /* Where each rotation goes, and how hard.
     *
     * route[source][destination] is +1, -1 or 0: which control a rotation
     * drives and which way round. A row may feed several destinations, so one
     * rotation can move both sticks, and a column may be fed by several
     * sources. Held as floats because the stub multiplies by them, and it
     * cannot negate a float itself.
     *
     * strength is per rotation, in camera degrees per degree of hand
     * movement, so 1.0 turns the view as far as the hands move. */
    volatile float route[GA_SRC_COUNT][GA_OUT_COUNT];
    volatile float strength[GA_SRC_COUNT];

    /* Per rotation: how it is read, and for a held angle, how far from rest
     * counts as pushing the control all the way. */
    volatile uint32_t read_mode[GA_SRC_COUNT];   /* GA_READ_* */
    volatile float full_angle[GA_SRC_COUNT];     /* degrees */

    /* How far the remembered angle may run before it stops counting. Well
     * past full_angle on purpose: clamping the stored angle at the full
     * deflection throws away any rotation beyond it, so turning further than
     * full and coming back lands nowhere near rest. Only the output is
     * clamped; the angle itself tracks where the pad actually is. */
    volatile float angle_cap[GA_SRC_COUNT];      /* degrees */

    /* Make wherever the pad is the rest position each time the condition
     * engages. Pressing the trigger then means "straight ahead is here", which
     * is both what steering wants and what keeps a slow drift from ever
     * building up. */
    volatile uint32_t rest_on_activate;

    /* How quickly a held angle creeps back to rest on its own, as a fraction
     * shed per second. Zero holds for as long as the pad is held, which is
     * what steering wants; a little lets integration drift forgive itself. */
    volatile float recentre;

    /* How close to rest counts as being at rest, as a fraction of the full
     * angle. Without it the control never quite lets go: a tenth of a degree
     * left over still asks for something, and the game's deadzone is then
     * undone on its behalf, so a fraction of a degree becomes a sixth of the
     * stick and the game keeps drifting. */
    volatile float hold_band;

    /* How quickly a remembered angle is pulled onto the answer gravity gives,
     * per second. Four settles in about a quarter of a second. Zero turns the
     * correction off and leaves the angle adding up, which drifts. */
    volatile float hold_correct;

    /* Bumped to make wherever the pad is now the new resting position. */
    volatile uint32_t rest_epoch;

    /* Acceleration: everything is multiplied by 1 below lo_rate, rising to
     * accel_mul at hi_rate. A multiplier of 1 means none. */
    volatile float accel_mul;
    volatile float sens_lo_rate;         /* deg/s where the ramp starts */
    volatile float sens_hi_rate;         /* deg/s where it is fully applied */

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

    /* Arcsine, in degrees, sampled over a sine of 0 to 1. Hold mode measures
     * an angle from where gravity points, and that arrives as a sine and a
     * cosine. There is no arcsine the injected block can call, so the service
     * samples one here, the same way it samples the curves. */
    volatile float lut_asin[GA_LUT_N];

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
    volatile float    ring[GA_RING_N][GA_SRC_COUNT];
    volatile uint32_t ring_i;
    volatile float    carry[GA_STICK_COUNT][2];
    volatile float    angle[GA_SRC_COUNT];   /* degrees away from rest */
    volatile uint32_t rest_seen;             /* the epoch these angles start from */
    volatile uint32_t was_active;            /* to catch the moment it engages */

    /* Where gravity pointed when rest was taken, as a unit vector in the
     * pad's own frame, and whether it was worth recording. Tilting and
     * leaning are measured against this rather than added up over time, which
     * is the whole reason hold mode can be trusted for more than a few
     * seconds. */
    volatile float    rest_g[3];
    volatile uint32_t rest_g_ok;

    /* Per rotation: 1 when gravity could measure it this sample, 0 when the
     * reading had to be added up instead. Turning a pad that is lying flat is
     * the case gravity cannot see, because the pad turns about gravity
     * itself. Reported so the page can say so rather than leave it a
     * mystery. */
    volatile uint32_t hold_abs[GA_SRC_COUNT];
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
    volatile float    rate[GA_SRC_COUNT];   /* deg/s, after bias and space */
    volatile float    held[GA_SRC_COUNT];   /* degrees away from rest */
    volatile float    want[GA_STICK_COUNT][2];  /* deg/s asked of each stick */
    volatile float    out_frac;              /* the busier stick, 0..1 */
    volatile uint8_t  in_rx, in_ry, out_rx, out_ry;    /* right stick */
    volatile uint8_t  in_lx, in_ly, out_lx, out_ly;    /* left stick */
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

/* Who may write what, enforced at build time.
 *
 * The service republishes every settings field in one go, from `enabled`
 * through `settings_seq`, roughly ten times a second, straight over whatever
 * the page holds. So any field the stub writes must sit after that range. A
 * stub-written field inside it is not a race that shows up now and then: the
 * service simply wins, about ten times a second, and the stub's write is
 * never seen.
 *
 * That exact mistake shipped once. The stub asked for a fresh rest position
 * by bumping `rest_epoch`, which lives among the settings, and the service
 * put its own value back before the next frame. Hold mode reset at random and
 * the cause was invisible from either side alone.
 *
 * Every field named below is written by stub.c. Keep the list in step with it:
 * moving a field up among the settings, or teaching the stub to write a new
 * settings field, now fails the build here rather than on the console.
 *
 * `age` is the one shared field and is deliberately absent. The stub counts
 * reads up, the service zeroes it as a separate write after the settings, and
 * it is only ever a staleness watchdog, so neither side needs the other's
 * value.
 */
#define GA_PUBLISHED_END \
    (__builtin_offsetof(ga_args_t, settings_seq) + sizeof(uint32_t))

#define GA_STUB_OWNED(field)                                              \
    _Static_assert(__builtin_offsetof(ga_args_t, field) >=                \
                   GA_PUBLISHED_END,                                      \
                   "the stub writes " #field ", so it must sit after "    \
                   "settings_seq; the service overwrites everything "     \
                   "before that")

GA_STUB_OWNED(active);
GA_STUB_OWNED(angle);
GA_STUB_OWNED(cal_amax);
GA_STUB_OWNED(cal_amin);
GA_STUB_OWNED(cal_count);
GA_STUB_OWNED(cal_max);
GA_STUB_OWNED(cal_min);
GA_STUB_OWNED(cal_seen);
GA_STUB_OWNED(cal_sum);
GA_STUB_OWNED(cal_was_active);
GA_STUB_OWNED(carry);
GA_STUB_OWNED(connected);
GA_STUB_OWNED(held);
GA_STUB_OWNED(in_lx);
GA_STUB_OWNED(in_ly);
GA_STUB_OWNED(in_rx);
GA_STUB_OWNED(in_ry);
GA_STUB_OWNED(last_accel);
GA_STUB_OWNED(last_angvel);
GA_STUB_OWNED(last_buttons);
GA_STUB_OWNED(last_handle);
GA_STUB_OWNED(last_result);
GA_STUB_OWNED(last_timestamp);
GA_STUB_OWNED(last_ts);
GA_STUB_OWNED(out_frac);
GA_STUB_OWNED(out_lx);
GA_STUB_OWNED(out_ly);
GA_STUB_OWNED(out_rx);
GA_STUB_OWNED(out_ry);
GA_STUB_OWNED(rate);
GA_STUB_OWNED(rest_seen);
GA_STUB_OWNED(rest_g);
GA_STUB_OWNED(rest_g_ok);
GA_STUB_OWNED(hold_abs);
GA_STUB_OWNED(ring);
GA_STUB_OWNED(ring_i);
GA_STUB_OWNED(rumble_last_level);
GA_STUB_OWNED(rumble_last_ts);
GA_STUB_OWNED(rumble_mode_ts);
GA_STUB_OWNED(saturated);
GA_STUB_OWNED(want);
GA_STUB_OWNED(was_active);
GA_STUB_OWNED(applied);
GA_STUB_OWNED(calls);
GA_STUB_OWNED(sample_seq);

#undef GA_STUB_OWNED

int32_t ga_stub_read_state(int32_t handle, uint8_t *data, ga_args_t *a);
int32_t ga_stub_read_state_ext(int32_t handle, uint8_t *data, ga_args_t *a);
int32_t ga_stub_read(int32_t handle, uint8_t *data, int32_t count,
                     ga_args_t *a);
void    ga_stub_end(void);
