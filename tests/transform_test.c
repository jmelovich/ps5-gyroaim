/* SPDX-License-Identifier: GPL-3.0-or-later
 * transform_test.c - runs the injected transform on the host.
 *
 * The stub is deliberately self-contained and reaches Sony's functions only
 * through pointers on the shared page, which means it can be exercised here
 * with a stand-in that fills a pad sample. No console needed.
 *
 * It exists because of a bug it would have caught. A stick's vertical axis
 * grows downward while camera pitch grows upward, and adding one into the
 * other inverted the right stick's up and down, but only while the gyro was
 * engaged, so nothing else showed it.
 */
#include "../payload/config.h"
#include "../payload/stub.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static uint8_t g_sample[GA_PAD_SIZE];

/* Stands in for scePadReadState: hands back whatever the test set up. */
static int32_t fake_read_state(int32_t handle, uint8_t *data)
{
    (void)handle;
    memcpy(data, g_sample, GA_PAD_SIZE);
    return 0;
}

static void set_float(unsigned off, float v)
{
    memcpy(g_sample + off, &v, sizeof(v));
}

static void sample_reset(void)
{
    memset(g_sample, 0, sizeof(g_sample));
    g_sample[GA_OFF_LX] = 128;
    g_sample[GA_OFF_LY] = 128;
    g_sample[GA_OFF_RX] = 128;
    g_sample[GA_OFF_RY] = 128;
    g_sample[GA_OFF_CONNECTED] = 1;
    /* Gravity, so player space trusts the accelerometer. */
    set_float(GA_OFF_ACCEL + 4, 1.0f);
}

static void sample_timestamp(uint64_t us)
{
    memcpy(g_sample + GA_OFF_TIMESTAMP, &us, sizeof(us));
}

static void check(const char *what, int ok, double got, double want)
{
    if (ok) {
        printf("  ok   %-44s %.1f\n", what, got);
        return;
    }
    printf("  FAIL %-44s got %.1f, wanted %.1f\n", what, got, want);
    failures++;
}

/* A clock shared by the stepped sequences below, so one run carries on from
 * where the last stopped rather than jumping back. */
static uint64_t g_when = 1000000;

/* Feeds the stub a steady rotation for a while and returns the last output.
 * Rate is in degrees a second about the yaw axis. */
static void spin(ga_args_t *a, float deg_per_s, int ms, uint8_t *out)
{
    const int step = 8;                 /* a real pad's interval */
    uint8_t buf[GA_PAD_SIZE];

    set_float(GA_OFF_ANGVEL + 4, deg_per_s / 57.29578f);
    for (int elapsed = 0; elapsed < ms; elapsed += step) {
        sample_timestamp(g_when);
        g_when += (uint64_t)step * 1000;
        memset(buf, 0, sizeof(buf));
        ga_stub_read_state(1, buf, a);
    }
    memcpy(out, buf, GA_PAD_SIZE);
}

/* Runs the stub over a few samples so its smoothing settles, and returns the
 * last output. */
static void run(ga_args_t *a, uint8_t *out_x, uint8_t *out_y)
{
    uint8_t buf[GA_PAD_SIZE];
    uint64_t t = 1000000;

    for (int i = 0; i < 24; i++) {
        sample_timestamp(t);
        t += 8000;                  /* 125 Hz, like a real pad */
        memset(buf, 0, sizeof(buf));
        ga_stub_read_state(1, buf, a);
    }
    *out_x = buf[GA_OFF_RX];
    *out_y = buf[GA_OFF_RY];
}

static void prepare(ga_args_t *a, ga_config_t *c)
{
    memset(a, 0, sizeof(*a));
    ga_args_init_consts(a);
    ga_config_to_args(c, a);
    a->orig[GA_FN_READ_STATE] = (uint64_t)(uintptr_t)fake_read_state;
    a->age = 0;
    a->cal_epoch = 1;
}

int main(void)
{
    ga_config_t c;
    ga_args_t a;
    uint8_t x, y;

    ga_config_defaults(&c);
    /* Always on, no smoothing or tightening, so each case is deterministic. */
    c.activation = GA_ACT_ALWAYS;
    c.tighten = 0.0f;
    c.smooth_rate = 0.0f;
    c.carry_cap = 0.0f;
    c.space = GA_SPACE_LOCAL;
    c.calibrate = 0;
    ga_config_clamp(&c);

    printf("stick passes through unchanged when the gyro is still\n");
    sample_reset();
    g_sample[GA_OFF_RY] = 200;            /* pushed down */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("pushed down stays down", y > 170 && y < 230, y, 200);
    check("horizontal stays centred", x > 120 && x < 136, x, 128);

    sample_reset();
    g_sample[GA_OFF_RX] = 200;            /* pushed right */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("pushed right stays right", x > 170 && x < 230, x, 200);

    sample_reset();
    g_sample[GA_OFF_RY] = 56;             /* pushed up */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("pushed up stays up", y > 26 && y < 86, y, 56);

    printf("the gyro moves the stick the right way\n");
    sample_reset();
    set_float(GA_OFF_ANGVEL + 4, 1.0f);   /* yaw: turning left */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("turning left aims left", x < 120, x, 100);

    sample_reset();
    set_float(GA_OFF_ANGVEL, 1.0f);       /* pitch: nose up */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("tilting up aims up", y < 120, y, 100);

    printf("the gyro and the stick add rather than fight\n");
    sample_reset();
    g_sample[GA_OFF_RY] = 180;            /* already looking down */
    set_float(GA_OFF_ANGVEL, -1.0f);      /* and tilting down too */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("both downward goes further down", y > 180, y, 220);

    sample_reset();
    g_sample[GA_OFF_RX] = 180;
    set_float(GA_OFF_ANGVEL + 4, -1.0f);  /* turning right as well */
    prepare(&a, &c);
    run(&a, &x, &y);
    check("both rightward goes further right", x > 180, x, 220);

    printf("a rotation can be routed anywhere, and to more than one place\n");

    /* Turning drives the LEFT stick sideways instead of the right one. */
    ga_config_defaults(&c);
    c.activation = GA_ACT_ALWAYS;
    c.tighten = 0.0f;
    c.smooth_rate = 0.0f;
    c.carry_cap = 0.0f;
    c.space = GA_SPACE_LOCAL;
    c.calibrate = 0;
    memset(c.route, 0, sizeof(c.route));
    c.route[GA_SRC_YAW][GA_OUT_LEFT_X] = -1;
    ga_config_clamp(&c);
    sample_reset();
    set_float(GA_OFF_ANGVEL + 4, 1.0f);          /* turning left */
    prepare(&a, &c);
    {
        uint8_t buf[GA_PAD_SIZE];
        uint64_t when = 1000000;
        for (int i = 0; i < 24; i++) {
            sample_timestamp(when);
            when += 8000;
            memset(buf, 0, sizeof(buf));
            ga_stub_read_state(1, buf, &a);
        }
        check("turning moves the left stick left", buf[GA_OFF_LX] < 120,
              buf[GA_OFF_LX], 100);
        check("and leaves the right stick alone", buf[GA_OFF_RX] == 128,
              buf[GA_OFF_RX], 128);
    }

    /* One rotation feeding both sticks at once. */
    memset(c.route, 0, sizeof(c.route));
    c.route[GA_SRC_YAW][GA_OUT_RIGHT_X] = 1;
    c.route[GA_SRC_YAW][GA_OUT_LEFT_X] = -1;
    ga_config_clamp(&c);
    sample_reset();
    set_float(GA_OFF_ANGVEL + 4, 1.0f);
    prepare(&a, &c);
    {
        uint8_t buf[GA_PAD_SIZE];
        uint64_t when = 1000000;
        for (int i = 0; i < 24; i++) {
            sample_timestamp(when);
            when += 8000;
            memset(buf, 0, sizeof(buf));
            ga_stub_read_state(1, buf, &a);
        }
        check("one rotation drives both sticks", buf[GA_OFF_RX] > 136 &&
                                                 buf[GA_OFF_LX] < 120,
              buf[GA_OFF_RX], 160);
        check("and in opposite directions as asked",
              buf[GA_OFF_RX] > 128 && buf[GA_OFF_LX] < 128, buf[GA_OFF_LX], 100);
    }

    /* Split: turning on one stick, tilting on the other. */
    memset(c.route, 0, sizeof(c.route));
    c.route[GA_SRC_YAW][GA_OUT_LEFT_X] = 1;
    c.route[GA_SRC_PITCH][GA_OUT_RIGHT_Y] = 1;
    ga_config_clamp(&c);
    sample_reset();
    set_float(GA_OFF_ANGVEL + 4, 1.0f);          /* turn */
    set_float(GA_OFF_ANGVEL, 1.0f);              /* and tilt up */
    prepare(&a, &c);
    {
        uint8_t buf[GA_PAD_SIZE];
        uint64_t when = 1000000;
        for (int i = 0; i < 24; i++) {
            sample_timestamp(when);
            when += 8000;
            memset(buf, 0, sizeof(buf));
            ga_stub_read_state(1, buf, &a);
        }
        check("split: turning went to the left stick", buf[GA_OFF_LX] > 136,
              buf[GA_OFF_LX], 160);
        check("split: tilting went to the right stick", buf[GA_OFF_RY] < 120,
              buf[GA_OFF_RY], 100);
    }

    printf("a rotation can be held instead of flowing\n");

    /* The complaint this answers: a gyro reports how fast it is turning, so a
     * held tilt reads as nothing and the control springs back, then reads the
     * other way on the way home. Reading the rotation as an angle instead
     * keeps the control pushed for as long as the pad is held over. */
    ga_config_defaults(&c);
    c.activation = GA_ACT_ALWAYS;
    c.tighten = 0.0f;
    c.smooth_rate = 0.0f;
    c.carry_cap = 0.0f;
    c.space = GA_SPACE_LOCAL;
    c.calibrate = 0;
    c.merge_stick = 0;
    memset(c.route, 0, sizeof(c.route));
    c.route[GA_SRC_YAW][GA_OUT_RIGHT_X] = 1;
    c.read_mode[GA_SRC_YAW] = GA_READ_HOLD;
    c.full_angle[GA_SRC_YAW] = 45.0f;
    c.recentre = 0.0f;
    ga_config_clamp(&c);

    {
        uint8_t out[GA_PAD_SIZE];
        sample_reset();
        prepare(&a, &c);
        g_when = 1000000;

        /* Tilt over at 45 deg/s for half a second: about 22 degrees, half of
         * the 45 that asks for everything. */
        spin(&a, 45.0f, 500, out);
        int tilted = out[GA_OFF_RX];
        check("tilting over pushes the stick", tilted > 150, tilted, 190);

        /* Now stop moving and hold there. This is where the old behaviour
         * sprang back to centre. */
        spin(&a, 0.0f, 1000, out);
        int held = out[GA_OFF_RX];
        check("holding it there keeps the stick pushed", held > 150, held, 190);
        check("and at the same amount", abs(held - tilted) < 6, held, tilted);

        /* Return to where it started: the stick comes back to centre. */
        spin(&a, -45.0f, 500, out);
        check("coming back to rest centres the stick completely",
              out[GA_OFF_RX] == 128, out[GA_OFF_RX], 128);

        /* Past the full angle it stops asking for more, so coming back does
         * not have to unwind an overshoot first. */
        spin(&a, 90.0f, 1500, out);
        int pinned = out[GA_OFF_RX];
        spin(&a, 0.0f, 200, out);
        check("beyond the full angle it just stays at full",
              out[GA_OFF_RX] == pinned && pinned > 230, out[GA_OFF_RX], 255);
        spin(&a, -90.0f, 600, out);
        check("and lets go as soon as it comes back",
              out[GA_OFF_RX] < 200, out[GA_OFF_RX], 150);
    }

    /* The contrast: read as a speed, the same hold springs back. */
    c.read_mode[GA_SRC_YAW] = GA_READ_FLOW;
    ga_config_clamp(&c);
    {
        uint8_t out[GA_PAD_SIZE];
        sample_reset();
        prepare(&a, &c);
        g_when = 1000000;

        spin(&a, 45.0f, 500, out);
        check("read as a speed, turning pushes the stick",
              out[GA_OFF_RX] > 140, out[GA_OFF_RX], 170);
        spin(&a, 0.0f, 500, out);
        check("and stopping lets it go, as aiming wants",
              abs((int)out[GA_OFF_RX] - 128) < 8, out[GA_OFF_RX], 128);
    }

    printf("parking it leaves the sample alone\n");
    sample_reset();
    g_sample[GA_OFF_RY] = 200;
    set_float(GA_OFF_ANGVEL + 4, 2.0f);
    c.activation = GA_ACT_NEVER;
    ga_config_clamp(&c);
    prepare(&a, &c);
    run(&a, &x, &y);
    check("untouched while parked", y == 200 && x == 128, y, 200);

    printf(failures ? "\n%d failure(s)\n" : "\nall transform checks passed\n",
           failures);
    return failures ? 1 : 0;
}
