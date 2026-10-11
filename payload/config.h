/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pad.h"     /* button bits */
#include "stub.h"

#include <stdint.h>

#define GA_DIR              "/data/gyroaim"
#define GA_CONFIG_PATH      GA_DIR "/settings.txt"
#define GA_PATH_MAX         96

/* Settings as a person thinks about them.
 *
 * Deliberately not the struct the stub reads. Here a sensitivity is a
 * magnitude with a separate inversion flag and a curve is an exponent; the
 * stub gets signed numbers and sampled tables, because it can neither negate
 * a float nor call powf.
 */
typedef struct {
    /* what it does */
    int   enabled;
    int   target;              /* GA_TARGET_* */
    int   space;               /* GA_SPACE_* */

    /* when it applies */
    int   activation;          /* GA_ACT_* */
    int   trigger_right;       /* 0 L2, 1 R2 */
    int   trigger_threshold;   /* 1-255 */
    uint32_t activation_mask;  /* button bits for GA_ACT_BUTTON */

    /* how much. Sensitivity is camera degrees per degree of hand movement,
     * so 1.0 is a one-to-one turn. Two values with a speed band between them
     * give acceleration; equal values give none. */
    float sens_min;
    float sens_max;
    float sens_lo_rate;        /* deg/s at which sens_min applies */
    float sens_hi_rate;        /* deg/s at which sens_max applies */
    float vert_scale;          /* pitch relative to yaw */
    int   invert_yaw;
    int   invert_pitch;
    int   axis_yaw;            /* 0 x, 1 y, 2 z */
    int   axis_pitch;
    int   axis_roll;

    /* the game's own camera, which everything else is measured against.
     * max_turn is how fast it spins at full stick; the three curve numbers
     * describe what it does to a stick value before using it. */
    float max_turn;            /* deg/s */
    float game_deadzone;       /* fraction of travel ignored at the centre */
    float game_outer;          /* fraction ignored at the edge */
    float game_power;          /* response curve exponent */

    /* steadiness */
    float tighten;             /* deg/s pulled toward zero */
    float smooth_rate;         /* deg/s below which motion is averaged */
    float carry_cap;           /* stick-seconds of held-back flick */

    /* the physical stick */
    int   merge_stick;
    int   stick_deadzone;      /* counts from centre before it counts as used */

    /* drift */
    int   calibrate;           /* learn the resting offset automatically */
    float bias[3];             /* rad/s, learned or set by hand */

    /* feel */
    int   rumble;
    float rumble_scale;
    int   rumble_period_ms;
} ga_config_t;

void ga_config_defaults(ga_config_t *c);
void ga_config_clamp(ga_config_t *c);

int ga_config_load(ga_config_t *c, const char *path);
int ga_config_save(const ga_config_t *c, const char *path);

/* GA_DIR/<title>.txt, so each game can keep its own numbers. */
void ga_config_title_path(const char *title, char *out, size_t n);

/* Fills the settings half of the shared page, including the sampled curves. */
void ga_config_to_args(const ga_config_t *c, ga_args_t *a);

/* Fills the float constants the stub needs. Called once before injecting. */
void ga_args_init_consts(ga_args_t *a);

/* Applies one "key=value" setting. Returns 0 if the key was known. */
int ga_config_set(ga_config_t *c, const char *key, const char *value);

int ga_config_json(const ga_config_t *c, char *out, size_t n);

/* Looks at the stillness window the stub filled in and, if the pad really was
 * still, eases the resting offset toward what it measured. Returns 1 if the
 * bias changed. Always arranges for a fresh window. */
int ga_config_calibrate(ga_config_t *c, const ga_args_t *live,
                        uint32_t *epoch);
