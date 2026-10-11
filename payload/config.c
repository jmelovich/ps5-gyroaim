/* SPDX-License-Identifier: GPL-3.0-or-later
 * config.c - the settings, and turning them into what the stub reads.
 */
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ga_config_defaults(ga_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = 1;
    c->target = GA_TARGET_RIGHT_STICK;
    c->activation = GA_ACT_TRIGGER;
    c->trigger_right = 0;
    c->trigger_threshold = 64;
    c->activation_mask = GA_BTN_L2;
    /* 30 counts per rad/s puts full deflection at about 4 rad/s, or 240
     * degrees a second of hand movement. Measured hand motion while aiming
     * runs to 3-5 rad/s, so this is a sane place to start tuning from. */
    c->sens_yaw = 30.0f;
    c->sens_pitch = 30.0f;
    /* PlayStation's frame is right-handed with Y up, so Y is the yaw axis and
     * X the pitch axis. Positive yaw is turning left and aiming left is a
     * smaller stick value, hence the default inversion on yaw only. */
    c->axis_yaw = 1;
    c->axis_pitch = 0;
    c->invert_yaw = 1;
    c->invert_pitch = 0;
    c->stick_deadzone = 20;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void ga_config_clamp(ga_config_t *c)
{
    c->enabled = c->enabled ? 1 : 0;
    c->target = clampi(c->target, 0, GA_TARGET_TOUCHPAD);
    c->activation = clampi(c->activation, 0, GA_ACT_NEVER);
    c->trigger_right = c->trigger_right ? 1 : 0;
    c->trigger_threshold = clampi(c->trigger_threshold, 1, 255);
    c->sens_yaw = clampf(c->sens_yaw, 0.0f, 400.0f);
    c->sens_pitch = clampf(c->sens_pitch, 0.0f, 400.0f);
    c->invert_yaw = c->invert_yaw ? 1 : 0;
    c->invert_pitch = c->invert_pitch ? 1 : 0;
    c->axis_yaw = clampi(c->axis_yaw, 0, 2);
    c->axis_pitch = clampi(c->axis_pitch, 0, 2);
    c->stick_deadzone = clampi(c->stick_deadzone, 0, 120);
    if (!c->activation_mask)
        c->activation_mask = GA_BTN_L2;
}

void ga_config_to_args(const ga_config_t *c, ga_args_t *a)
{
    a->magic = GA_MAGIC;
    a->version = GA_LAYOUT_VERSION;
    a->enabled = (uint32_t)c->enabled;
    a->target = (uint32_t)c->target;
    a->activation = (uint32_t)c->activation;
    a->activation_mask = c->activation_mask;
    a->trigger_right = (uint32_t)c->trigger_right;
    a->trigger_threshold = (uint32_t)c->trigger_threshold;
    a->axis_yaw = (uint32_t)c->axis_yaw;
    a->axis_pitch = (uint32_t)c->axis_pitch;
    a->stick_deadzone = (uint32_t)c->stick_deadzone;

    /* The sign lives in the sensitivity. See the note in config.h. */
    a->sens_yaw = c->invert_yaw ? -c->sens_yaw : c->sens_yaw;
    a->sens_pitch = c->invert_pitch ? -c->sens_pitch : c->sens_pitch;
    a->settings_seq++;
}

int ga_config_set(ga_config_t *c, const char *key, const char *value)
{
    if (!c || !key || !value)
        return -1;

#define INT_FIELD(name, member)                      \
    if (!strcmp(key, name)) {                        \
        c->member = (int)strtol(value, NULL, 0);     \
        return 0;                                    \
    }
#define FLOAT_FIELD(name, member)                    \
    if (!strcmp(key, name)) {                        \
        c->member = strtof(value, NULL);             \
        return 0;                                    \
    }

    INT_FIELD("enabled", enabled)
    INT_FIELD("target", target)
    INT_FIELD("activation", activation)
    INT_FIELD("trigger_right", trigger_right)
    INT_FIELD("trigger_threshold", trigger_threshold)
    INT_FIELD("invert_yaw", invert_yaw)
    INT_FIELD("invert_pitch", invert_pitch)
    INT_FIELD("axis_yaw", axis_yaw)
    INT_FIELD("axis_pitch", axis_pitch)
    INT_FIELD("stick_deadzone", stick_deadzone)
    FLOAT_FIELD("sens_yaw", sens_yaw)
    FLOAT_FIELD("sens_pitch", sens_pitch)

    if (!strcmp(key, "activation_mask")) {
        c->activation_mask = (uint32_t)strtoul(value, NULL, 0);
        return 0;
    }

#undef INT_FIELD
#undef FLOAT_FIELD
    return -1;
}

int ga_config_json(const ga_config_t *c, char *out, size_t n)
{
    return snprintf(out, n,
        "{\"enabled\":%d,\"target\":%d,\"activation\":%d,"
        "\"trigger_right\":%d,\"trigger_threshold\":%d,"
        "\"activation_mask\":%u,\"sens_yaw\":%.2f,\"sens_pitch\":%.2f,"
        "\"invert_yaw\":%d,\"invert_pitch\":%d,\"axis_yaw\":%d,"
        "\"axis_pitch\":%d,\"stick_deadzone\":%d}",
        c->enabled, c->target, c->activation, c->trigger_right,
        c->trigger_threshold, c->activation_mask, (double)c->sens_yaw,
        (double)c->sens_pitch, c->invert_yaw, c->invert_pitch,
        c->axis_yaw, c->axis_pitch, c->stick_deadzone);
}

int ga_config_load(ga_config_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[128];

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        char *v = eq + 1;
        size_t l = strlen(v);
        while (l && (v[l - 1] == '\n' || v[l - 1] == '\r' || v[l - 1] == ' '))
            v[--l] = '\0';
        ga_config_set(c, line, v);
    }
    fclose(f);
    ga_config_clamp(c);
    return 0;
}

int ga_config_save(const ga_config_t *c, const char *path)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    fprintf(f,
            "# GyroAim settings. Edit here or from the portal; the service\n"
            "# rereads this file only at startup.\n"
            "enabled=%d\n"
            "target=%d\n"
            "activation=%d\n"
            "trigger_right=%d\n"
            "trigger_threshold=%d\n"
            "activation_mask=0x%x\n"
            "sens_yaw=%.2f\n"
            "sens_pitch=%.2f\n"
            "invert_yaw=%d\n"
            "invert_pitch=%d\n"
            "axis_yaw=%d\n"
            "axis_pitch=%d\n"
            "stick_deadzone=%d\n",
            c->enabled, c->target, c->activation, c->trigger_right,
            c->trigger_threshold, c->activation_mask, (double)c->sens_yaw,
            (double)c->sens_pitch, c->invert_yaw, c->invert_pitch,
            c->axis_yaw, c->axis_pitch, c->stick_deadzone);
    fclose(f);
    return 0;
}
