/* SPDX-License-Identifier: GPL-3.0-or-later
 * config.c - the settings, the curve tables, and drift calibration.
 */
#include "config.h"
#include "log.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void ga_config_defaults(ga_config_t *c)
{
    memset(c, 0, sizeof(*c));

    c->enabled = 1;
    c->space = GA_SPACE_PLAYER;

    c->activation = GA_ACT_TRIGGER;
    c->trigger_right = 0;
    c->trigger_threshold = 64;
    c->activation_mask = GA_BTN_L2;

    /* Turning drives the right stick sideways and tilting drives it
     * vertically, which is ordinary gyro aiming. Positive yaw is turning
     * left while aiming left is a smaller stick value, hence the sign. */
    c->route[GA_SRC_YAW][GA_OUT_RIGHT_X] = -1;
    c->route[GA_SRC_PITCH][GA_OUT_RIGHT_Y] = 1;

    /* A touch over one-to-one is a good starting point for aiming: the view
     * turns slightly further than the hands do, which keeps wrist travel
     * reasonable without losing the sense of pointing. */
    c->strength[GA_SRC_YAW] = 1.2f;
    c->strength[GA_SRC_PITCH] = 1.2f;
    c->strength[GA_SRC_ROLL] = 1.0f;

    /* Read as a speed by default, which is what aiming wants. Thirty-five
     * degrees of tilt for full deflection is a comfortable reach when a
     * rotation is switched to being held instead. */
    for (int s = 0; s < GA_SRC_COUNT; s++) {
        c->read_mode[s] = GA_READ_FLOW;
        c->full_angle[s] = 35.0f;
    }
    c->recentre = 0.0f;
    c->hold_band = 0.04f;
    c->hold_correct = 4.0f;
    c->rest_on_activate = 1;

    c->accel_mul = 1.0f;             /* no acceleration by default */
    c->sens_lo_rate = 0.0f;
    c->sens_hi_rate = 75.0f;

    /* PlayStation's frame is right-handed with Y up, so Y is yaw, X is pitch
     * and Z is roll. */
    c->axis_yaw = 1;
    c->axis_pitch = 0;
    c->axis_roll = 2;

    /* Guesses until measured per game. 300 deg/s is a typical full-stick turn
     * rate, and most games apply a mild curve with a small deadzone. */
    c->max_turn = 300.0f;
    c->game_deadzone = 0.15f;
    c->game_outer = 0.02f;
    c->game_power = 1.6f;

    c->tighten = 4.0f;
    c->smooth_rate = 8.0f;
    c->carry_cap = 0.25f;

    c->merge_stick = 1;
    c->stick_deadzone = 20;

    c->calibrate = 1;

    c->rumble = 0;
    c->rumble_scale = 0.6f;
    c->rumble_period_ms = 60;
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
    c->space = clampi(c->space, 0, GA_SPACE_PLAYER);

    c->activation = clampi(c->activation, 0, GA_ACT_NEVER);
    c->trigger_right = c->trigger_right ? 1 : 0;
    c->trigger_threshold = clampi(c->trigger_threshold, 1, 255);
    if (!c->activation_mask)
        c->activation_mask = GA_BTN_L2;

    for (int s = 0; s < GA_SRC_COUNT; s++) {
        c->strength[s] = clampf(c->strength[s], 0.0f, 20.0f);
        for (int d = 0; d < GA_OUT_COUNT; d++)
            c->route[s][d] = clampi(c->route[s][d], -1, 1);
        c->read_mode[s] = clampi(c->read_mode[s], 0, GA_READ_HOLD);
        c->full_angle[s] = clampf(c->full_angle[s], 3.0f, 180.0f);
    }
    c->recentre = clampf(c->recentre, 0.0f, 5.0f);
    c->hold_band = clampf(c->hold_band, 0.0f, 0.3f);
    c->hold_correct = clampf(c->hold_correct, 0.0f, 50.0f);
    c->rest_on_activate = c->rest_on_activate ? 1 : 0;
    c->accel_mul = clampf(c->accel_mul, 0.1f, 8.0f);
    c->sens_lo_rate = clampf(c->sens_lo_rate, 0.0f, 500.0f);
    c->sens_hi_rate = clampf(c->sens_hi_rate, 0.0f, 1000.0f);
    c->axis_yaw = clampi(c->axis_yaw, 0, 2);
    c->axis_pitch = clampi(c->axis_pitch, 0, 2);
    c->axis_roll = clampi(c->axis_roll, 0, 2);

    c->max_turn = clampf(c->max_turn, 20.0f, 2000.0f);
    c->game_deadzone = clampf(c->game_deadzone, 0.0f, 0.6f);
    c->game_outer = clampf(c->game_outer, 0.0f, 0.3f);
    /* Keep some travel between the two deadzones. */
    if (c->game_deadzone + c->game_outer > 0.9f)
        c->game_outer = 0.9f - c->game_deadzone;
    c->game_power = clampf(c->game_power, 0.5f, 4.0f);

    c->tighten = clampf(c->tighten, 0.0f, 60.0f);
    c->smooth_rate = clampf(c->smooth_rate, 0.0f, 120.0f);
    /* Smoothing blends between half the threshold and the threshold, so a
     * threshold at or below zero must mean off. */
    c->carry_cap = clampf(c->carry_cap, 0.0f, 2.0f);

    c->merge_stick = c->merge_stick ? 1 : 0;
    c->stick_deadzone = clampi(c->stick_deadzone, 0, 120);

    c->calibrate = c->calibrate ? 1 : 0;
    for (int i = 0; i < 3; i++)
        c->bias[i] = clampf(c->bias[i], -1.0f, 1.0f);

    c->rumble = c->rumble ? 1 : 0;
    c->rumble_scale = clampf(c->rumble_scale, 0.0f, 2.0f);
    c->rumble_period_ms = clampi(c->rumble_period_ms, 16, 500);
}

void ga_args_init_consts(ga_args_t *a)
{
    a->k.zero = 0.0f;
    a->k.one = 1.0f;
    a->k.half = 0.5f;
    a->k.counts = 127.0f;
    a->k.centre = 128.0f;
    a->k.lut_scale = (float)GA_LUT_LAST;
    a->k.eps = 1e-5f;
    a->k.rad_to_deg = 57.29578f;
    a->k.us_to_s = 1e-6f;
    a->k.ring_inv = 1.0f / (float)GA_RING_N;
    a->k.neg_counts = -127.0f;
    a->k.full = 255.0f;
    a->k.half_turn = 180.0f;
    a->k.right_angle = 90.0f;
    a->k.quarter = 0.25f;
}

/* ---- the game's response curve ---------------------------------------- *
 *
 * A game takes a stick deflection, ignores a deadzone at each end, and bends
 * what is left by some exponent to get a camera speed. Both directions of
 * that mapping are sampled into tables so the stub can look them up: one to
 * learn what the physical stick is already asking for, and its inverse to
 * turn a wanted speed back into the deflection that produces it.
 */
static float deflection_to_speed(float d, const ga_config_t *c)
{
    float live = 1.0f - c->game_deadzone - c->game_outer;

    if (live < 0.01f)
        live = 0.01f;
    float t = (d - c->game_deadzone) / live;
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return powf(t, c->game_power);
}

static float speed_to_deflection(float v, const ga_config_t *c)
{
    float live = 1.0f - c->game_deadzone - c->game_outer;

    if (live < 0.01f)
        live = 0.01f;
    if (v <= 0.0f)
        return 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    return c->game_deadzone + powf(v, 1.0f / c->game_power) * live;
}

void ga_config_to_args(const ga_config_t *c, ga_args_t *a)
{
    a->magic = GA_MAGIC;
    a->version = GA_LAYOUT_VERSION;

    a->enabled = (uint32_t)c->enabled;
    a->space = (uint32_t)c->space;

    a->activation = (uint32_t)c->activation;
    a->activation_mask = c->activation_mask;
    a->trigger_right = (uint32_t)c->trigger_right;
    a->trigger_threshold = (uint32_t)c->trigger_threshold;

    a->axis_yaw = (uint32_t)c->axis_yaw;
    a->axis_pitch = (uint32_t)c->axis_pitch;
    a->axis_roll = (uint32_t)c->axis_roll;

    /* The signs live here as numbers: the stub multiplies by them, since it
     * cannot negate a float itself. */
    for (int s = 0; s < GA_SRC_COUNT; s++) {
        a->strength[s] = c->strength[s];
        a->read_mode[s] = (uint32_t)c->read_mode[s];
        a->full_angle[s] = c->full_angle[s];
        /* Generous on purpose: the stored angle has to follow the pad well
         * past full deflection, or turning further than full and coming back
         * does not cancel. */
        a->angle_cap[s] = c->full_angle[s] * 6.0f;
        if (a->angle_cap[s] < 180.0f)
            a->angle_cap[s] = 180.0f;
        for (int d = 0; d < GA_OUT_COUNT; d++)
            a->route[s][d] = (float)c->route[s][d];
    }
    a->recentre = c->recentre;
    a->hold_band = c->hold_band;
    a->hold_correct = c->hold_correct;
    a->rest_on_activate = (uint32_t)c->rest_on_activate;
    a->accel_mul = c->accel_mul;
    a->sens_lo_rate = c->sens_lo_rate;
    a->sens_hi_rate = c->sens_hi_rate;

    a->max_turn = c->max_turn;
    a->tighten = c->tighten;
    a->smooth_rate = c->smooth_rate;
    a->carry_cap = c->carry_cap;

    a->merge_stick = (uint32_t)c->merge_stick;
    a->stick_deadzone = (uint32_t)c->stick_deadzone;

    for (int i = 0; i < 3; i++)
        a->bias[i] = c->bias[i];

    a->rumble = (uint32_t)c->rumble;
    a->rumble_scale = c->rumble_scale;
    a->rumble_period_ms = (uint32_t)c->rumble_period_ms;

    for (int i = 0; i < GA_LUT_N; i++) {
        float x = (float)i / (float)GA_LUT_LAST;
        a->lut_to_vel[i] = deflection_to_speed(x, c);
        a->lut_to_stick[i] = speed_to_deflection(x, c);
        /* Degrees, for a sine of x. The last entry is a right angle, where
         * the slope is vertical and interpolation would undershoot, so it is
         * written exactly. */
        a->lut_asin[i] = (float)(asin(x > 1.0 ? 1.0 : x) * 180.0 / M_PI);
    }

    a->settings_seq++;
}

/* ---- drift ------------------------------------------------------------- *
 *
 * The gyro's resting reading is never exactly zero, and left alone that shows
 * up as a camera that creeps. The fix is to watch the pad while it is still
 * and subtract what it reads then.
 *
 * Knowing when it is still is the hard part. Turning on the spot at a steady
 * rate looks identical to stillness as far as the accelerometer is concerned,
 * so a window in which aiming was active is never trusted, and a measured
 * offset is only eased in rather than taken whole.
 */
#define CAL_MIN_SAMPLES   30
#define CAL_GYRO_SPREAD   0.030f     /* rad/s, about 1.7 deg/s */
#define CAL_ACCEL_SPREAD  0.025f     /* g */
#define CAL_FIRST_MAX     0.150f     /* a larger first offset is real motion */
#define CAL_STEP_MAX      0.040f     /* later, drift only creeps */
#define CAL_EASE          0.5f

int ga_config_calibrate(ga_config_t *c, const ga_args_t *live, uint32_t *epoch)
{
    static int settled;

    if (!c->calibrate)
        return 0;

    /* Nothing usable yet; leave the window running. */
    if (live->cal_seen != *epoch || live->cal_count < CAL_MIN_SAMPLES)
        return 0;

    /* Aiming happened during this window, so it proves nothing. */
    if (live->cal_was_active) {
        (*epoch)++;
        return 0;
    }

    for (int i = 0; i < 3; i++) {
        if (live->cal_max[i] - live->cal_min[i] > CAL_GYRO_SPREAD ||
            live->cal_amax[i] - live->cal_amin[i] > CAL_ACCEL_SPREAD) {
            (*epoch)++;        /* moving: start again */
            return 0;
        }
    }

    float mean[3];
    float limit = settled ? CAL_STEP_MAX : CAL_FIRST_MAX;
    for (int i = 0; i < 3; i++) {
        mean[i] = live->cal_sum[i] / (float)live->cal_count;
        if (fabsf(mean[i] - c->bias[i]) > limit) {
            (*epoch)++;
            return 0;
        }
    }

    int changed = 0;
    for (int i = 0; i < 3; i++) {
        float next = c->bias[i] + (mean[i] - c->bias[i]) * CAL_EASE;
        if (fabsf(next - c->bias[i]) > 1e-6f)
            changed = 1;
        c->bias[i] = next;
    }

    if (changed && !settled)
        ga_log("drift: resting offset learned, % .4f % .4f % .4f rad/s",
               (double)c->bias[0], (double)c->bias[1], (double)c->bias[2]);
    settled = 1;
    (*epoch)++;
    return changed;
}

/* ---- taking it off the console ------------------------------------------ */

#define PLDMGR_PAYLOAD_DIR  "/data/pldmgr/payloads/gyroaim"
#define PLDMGR_PAYLOAD_ELF  PLDMGR_PAYLOAD_DIR "/gyroaim.elf"
#define PLDMGR_AUTOLOAD     "/data/pldmgr/autoload.txt"

/* Rewrites pldmgr's list without our line, leaving every other payload
 * exactly as it was. */
static int drop_autoload_line(void)
{
    char list[8192];
    FILE *f = fopen(PLDMGR_AUTOLOAD, "r");
    size_t n;

    if (!f)
        return 0;
    n = fread(list, 1, sizeof(list) - 1, f);
    fclose(f);
    list[n] = '\0';

    if (!strstr(list, "gyroaim.elf"))
        return 0;

    if (!(f = fopen(PLDMGR_AUTOLOAD, "w"))) {
        ga_log("uninstall: cannot rewrite %s", PLDMGR_AUTOLOAD);
        return 0;
    }
    char *line = list;
    while (line && *line) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && strncmp(line, "gyroaim.elf", 11) != 0) {
            fwrite(line, 1, len, f);
            fputc('\n', f);
        }
        line = end ? end + 1 : NULL;
    }
    fclose(f);
    ga_log("uninstall: removed our line from %s", PLDMGR_AUTOLOAD);
    return 1;
}

int ga_uninstall(int remove_settings)
{
    int removed = 0;

    if (unlink(PLDMGR_PAYLOAD_ELF) == 0) {
        ga_log("uninstall: removed %s", PLDMGR_PAYLOAD_ELF);
        removed++;
    }
    rmdir(PLDMGR_PAYLOAD_DIR);
    removed += drop_autoload_line();

    if (!remove_settings) {
        /* The pidfile would otherwise name a process that no longer exists. */
        if (unlink(GA_DIR "/pid") == 0)
            removed++;
        ga_log("uninstall: settings under %s kept", GA_DIR);
        return removed;
    }

    /* Everything, which means reading the directory: profiles are named after
     * whichever games were played and the mapping notes after process ids, so
     * they cannot be listed ahead of time. */
    DIR *dir = opendir(GA_DIR);
    if (!dir) {
        ga_log("uninstall: cannot read %s", GA_DIR);
        return removed;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char path[GA_PATH_MAX + 64];

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", GA_DIR, entry->d_name);
        if (unlink(path) == 0) {
            ga_log("uninstall: removed %s", path);
            removed++;
        } else {
            ga_log("uninstall: could not remove %s", path);
        }
    }
    closedir(dir);

    if (rmdir(GA_DIR) == 0)
        ga_log("uninstall: removed %s", GA_DIR);

    return removed;
}

/* ---- reading and writing settings -------------------------------------- */

void ga_config_title_path(const char *title, char *out, size_t n)
{
    if (title && title[0])
        snprintf(out, n, "%s/%s.txt", GA_DIR, title);
    else
        snprintf(out, n, "%s", GA_CONFIG_PATH);
}

#define FIELDS(I, F, U)                                 \
    I("enabled", enabled)                               \
    I("space", space)                                    \
    I("activation", activation)                          \
    I("trigger_right", trigger_right)                    \
    I("trigger_threshold", trigger_threshold)            \
    I("axis_yaw", axis_yaw)                              \
    I("axis_pitch", axis_pitch)                          \
    I("axis_roll", axis_roll)                            \
    I("merge_stick", merge_stick)                        \
    I("stick_deadzone", stick_deadzone)                  \
    I("calibrate", calibrate)                            \
    I("rumble", rumble)                                  \
    I("rumble_period_ms", rumble_period_ms)              \
    F("accel_mul", accel_mul)                            \
    F("sens_lo_rate", sens_lo_rate)                      \
    F("sens_hi_rate", sens_hi_rate)                      \
    I("read_yaw", read_mode[GA_SRC_YAW])                 \
    I("read_pitch", read_mode[GA_SRC_PITCH])             \
    I("read_roll", read_mode[GA_SRC_ROLL])               \
    F("full_angle_yaw", full_angle[GA_SRC_YAW])          \
    F("full_angle_pitch", full_angle[GA_SRC_PITCH])      \
    F("full_angle_roll", full_angle[GA_SRC_ROLL])        \
    F("recentre", recentre)                              \
    F("hold_band", hold_band)                              \
    F("hold_correct", hold_correct)                            \
    I("rest_on_activate", rest_on_activate)              \
    F("strength_yaw", strength[GA_SRC_YAW])              \
    F("strength_pitch", strength[GA_SRC_PITCH])          \
    F("strength_roll", strength[GA_SRC_ROLL])            \
    I("route_yaw_rx", route[GA_SRC_YAW][GA_OUT_RIGHT_X]) \
    I("route_yaw_ry", route[GA_SRC_YAW][GA_OUT_RIGHT_Y]) \
    I("route_yaw_lx", route[GA_SRC_YAW][GA_OUT_LEFT_X])  \
    I("route_yaw_ly", route[GA_SRC_YAW][GA_OUT_LEFT_Y])  \
    I("route_pitch_rx", route[GA_SRC_PITCH][GA_OUT_RIGHT_X]) \
    I("route_pitch_ry", route[GA_SRC_PITCH][GA_OUT_RIGHT_Y]) \
    I("route_pitch_lx", route[GA_SRC_PITCH][GA_OUT_LEFT_X])  \
    I("route_pitch_ly", route[GA_SRC_PITCH][GA_OUT_LEFT_Y])  \
    I("route_roll_rx", route[GA_SRC_ROLL][GA_OUT_RIGHT_X])   \
    I("route_roll_ry", route[GA_SRC_ROLL][GA_OUT_RIGHT_Y])   \
    I("route_roll_lx", route[GA_SRC_ROLL][GA_OUT_LEFT_X])    \
    I("route_roll_ly", route[GA_SRC_ROLL][GA_OUT_LEFT_Y])    \
    F("max_turn", max_turn)                              \
    F("game_deadzone", game_deadzone)                    \
    F("game_outer", game_outer)                          \
    F("game_power", game_power)                          \
    F("tighten", tighten)                                \
    F("smooth_rate", smooth_rate)                        \
    F("carry_cap", carry_cap)                            \
    F("rumble_scale", rumble_scale)                      \
    U("activation_mask", activation_mask)

int ga_config_set(ga_config_t *c, const char *key, const char *value)
{
    if (!c || !key || !value)
        return -1;

#define AS_INT(name, member)                         \
    if (!strcmp(key, name)) {                        \
        c->member = (int)strtol(value, NULL, 0);     \
        return 0;                                    \
    }
#define AS_FLOAT(name, member)                       \
    if (!strcmp(key, name)) {                        \
        c->member = strtof(value, NULL);             \
        return 0;                                    \
    }
#define AS_U32(name, member)                         \
    if (!strcmp(key, name)) {                        \
        c->member = (uint32_t)strtoul(value, NULL, 0); \
        return 0;                                    \
    }

    FIELDS(AS_INT, AS_FLOAT, AS_U32)

    if (!strcmp(key, "bias_x")) { c->bias[0] = strtof(value, NULL); return 0; }
    if (!strcmp(key, "bias_y")) { c->bias[1] = strtof(value, NULL); return 0; }
    if (!strcmp(key, "bias_z")) { c->bias[2] = strtof(value, NULL); return 0; }

#undef AS_INT
#undef AS_FLOAT
#undef AS_U32
    return -1;
}

int ga_config_json(const ga_config_t *c, char *out, size_t n)
{
    int w = snprintf(out, n, "{");

#define J_INT(name, member) \
    w += snprintf(out + w, n - (size_t)w, "\"" name "\":%d,", c->member);
#define J_FLOAT(name, member) \
    w += snprintf(out + w, n - (size_t)w, "\"" name "\":%.3f,", (double)c->member);
#define J_U32(name, member) \
    w += snprintf(out + w, n - (size_t)w, "\"" name "\":%u,", c->member);

    FIELDS(J_INT, J_FLOAT, J_U32)

#undef J_INT
#undef J_FLOAT
#undef J_U32

    w += snprintf(out + w, n - (size_t)w,
                  "\"bias_x\":%.5f,\"bias_y\":%.5f,\"bias_z\":%.5f}",
                  (double)c->bias[0], (double)c->bias[1], (double)c->bias[2]);
    return w;
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

    fprintf(f, "# GyroAim settings. The service reads this at startup and\n"
               "# writes it back whenever something changes in the portal.\n");

#define W_INT(name, member)   fprintf(f, name "=%d\n", c->member);
#define W_FLOAT(name, member) fprintf(f, name "=%.4f\n", (double)c->member);
#define W_U32(name, member)   fprintf(f, name "=0x%x\n", c->member);

    FIELDS(W_INT, W_FLOAT, W_U32)

#undef W_INT
#undef W_FLOAT
#undef W_U32

    fprintf(f, "bias_x=%.6f\nbias_y=%.6f\nbias_z=%.6f\n",
            (double)c->bias[0], (double)c->bias[1], (double)c->bias[2]);
    fclose(f);
    return 0;
}
