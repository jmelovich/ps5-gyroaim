/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pad.h"     /* button bits */
#include "stub.h"

#include <stdint.h>

#define GA_CONFIG_PATH "/data/gyroaim/settings.txt"

/* Settings as a person thinks about them.
 *
 * This is deliberately not the same struct the stub reads. Sensitivity here
 * is a magnitude with a separate inversion flag, which is the natural way to
 * present it; the stub gets a single signed number, because negating a float
 * in injected code would pull a constant out of .rodata that the copied block
 * cannot reach.
 */
typedef struct {
    int      enabled;
    int      target;             /* GA_TARGET_* */
    int      activation;         /* GA_ACT_* */
    int      trigger_right;      /* 0 L2, 1 R2 */
    int      trigger_threshold;  /* 0-255 */
    uint32_t activation_mask;    /* button bits, for GA_ACT_BUTTON */
    float    sens_yaw;           /* stick counts per rad/s, magnitude */
    float    sens_pitch;
    int      invert_yaw;
    int      invert_pitch;
    int      axis_yaw;           /* which gyro axis is yaw: 0 x, 1 y, 2 z */
    int      axis_pitch;
    int      stick_deadzone;     /* counts from centre */
} ga_config_t;

void ga_config_defaults(ga_config_t *c);

/* Clamps every field into range. Always call after loading or editing. */
void ga_config_clamp(ga_config_t *c);

int ga_config_load(ga_config_t *c, const char *path);
int ga_config_save(const ga_config_t *c, const char *path);

/* Fills in the settings half of the shared page. */
void ga_config_to_args(const ga_config_t *c, ga_args_t *a);

/* Applies one "key=value" setting. Returns 0 if the key was known. */
int ga_config_set(ga_config_t *c, const char *key, const char *value);

/* Writes the settings as JSON. Returns the length. */
int ga_config_json(const ga_config_t *c, char *out, size_t n);
