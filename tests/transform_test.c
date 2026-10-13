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
#include <stddef.h>
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

/* Feeds the stub a steady rotation about one gyro axis and returns the last
 * output. Rate is in degrees a second. */
static void spin_axis(ga_args_t *a, int axis, float deg_per_s, int ms,
                      uint8_t *out)
{
    const int step = 8;                 /* a real pad's interval */
    uint8_t buf[GA_PAD_SIZE];

    for (int i = 0; i < 3; i++)
        set_float(GA_OFF_ANGVEL + 4 * i, 0.0f);
    set_float(GA_OFF_ANGVEL + 4 * axis, deg_per_s / 57.29578f);
    for (int elapsed = 0; elapsed < ms; elapsed += step) {
        /* The service clears the staleness counter on every publish, about
         * ten times a second. Without this a long run quietly crosses the
         * watchdog and the transform parks itself, which looks exactly like
         * a bug in the maths. */
        a->age = 0;
        sample_timestamp(g_when);
        g_when += (uint64_t)step * 1000;
        memset(buf, 0, sizeof(buf));
        ga_stub_read_state(1, buf, a);
    }
    memcpy(out, buf, GA_PAD_SIZE);
}

/* Where gravity points, in the pad's own frame, for a pad rotated so many
 * degrees about one of its axes from flat. The pad turning one way moves
 * gravity the other way in its own frame, which is what the sensor reports.
 *
 * Driving this alongside the rotation rate is what makes a test honest: a
 * rate with gravity held still describes a pad that is spinning and
 * stationary at once, and the transform is entitled to disbelieve it. */
static void set_pose(int axis, float degrees)
{
    float flat[3] = { 0.0f, 1.0f, 0.0f };   /* lying flat, gravity up its y */
    unsigned u = axis == 2 ? 0u : (unsigned)axis + 1u;
    unsigned v = u == 2u ? 0u : u + 1u;
    float r = degrees / 57.29578f;
    float g[3] = { flat[0], flat[1], flat[2] };

    g[u] = flat[u] * cosf(r) + flat[v] * sinf(r);
    g[v] = flat[v] * cosf(r) - flat[u] * sinf(r);
    for (int i = 0; i < 3; i++)
        set_float(GA_OFF_ACCEL + 4 * i, g[i]);
}

/* Rotates the pad from one angle to another over a stretch of time, moving
 * the rate and the pose together so the two agree. */
static void sweep(ga_args_t *a, int axis, float from, float to, int ms,
                  uint8_t *out)
{
    const int step = 8;
    uint8_t buf[GA_PAD_SIZE];
    int ticks = ms / step;

    if (ticks < 1)
        ticks = 1;
    float rate = ms > 0 ? (to - from) / ((float)ms / 1000.0f) : 0.0f;

    memset(buf, 0, sizeof(buf));
    for (int n = 1; n <= ticks; n++) {
        for (int i = 0; i < 3; i++)
            set_float(GA_OFF_ANGVEL + 4 * i, 0.0f);
        set_float(GA_OFF_ANGVEL + 4 * axis, rate / 57.29578f);
        set_pose(axis, from + (to - from) * (float)n / (float)ticks);
        a->age = 0;
        sample_timestamp(g_when);
        g_when += (uint64_t)step * 1000;
        memset(buf, 0, sizeof(buf));
        ga_stub_read_state(1, buf, a);
    }
    memcpy(out, buf, GA_PAD_SIZE);
}

/* Holds the pad still at an angle. */
static void rest_at(ga_args_t *a, int axis, float degrees, int ms,
                    uint8_t *out)
{
    sweep(a, axis, degrees, degrees, ms, out);
}

/* The yaw axis, which most of the cases below use. */
static void spin(ga_args_t *a, float deg_per_s, int ms, uint8_t *out)
{
    const int step = 8;
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

        /* Past the full angle the control stays at full, but the angle keeps
         * following the pad, so turning further out and all the way back
         * lands at rest rather than pushed the other way. This is what being
         * 50 percent out after a 90 degree turn looked like. */
        spin(&a, 90.0f, 1000, out);             /* 90 degrees out, full at 45 */
        int pinned = out[GA_OFF_RX];
        check("well past full, the control is just at full", pinned > 240,
              pinned, 255);
        spin(&a, -90.0f, 1000, out);            /* and all the way back */
        check("turning out past full and back lands at rest",
              out[GA_OFF_RX] == 128, out[GA_OFF_RX], 128);
    }

    /* Engaging takes rest afresh, so the pad's angle when the trigger goes
     * down is straight ahead however it was held before. */
    c.activation = GA_ACT_TRIGGER;
    c.trigger_threshold = 64;
    c.rest_on_activate = 1;
    ga_config_clamp(&c);
    {
        uint8_t out[GA_PAD_SIZE];
        sample_reset();
        prepare(&a, &c);
        g_when = 1000000;

        /* Tilt a long way with the trigger up: nothing should be held. */
        spin(&a, 60.0f, 1000, out);
        check("not engaged, the stick is untouched", out[GA_OFF_RX] == 128,
              out[GA_OFF_RX], 128);

        /* Now press the trigger while still tilted over. */
        g_sample[GA_OFF_L2] = 200;
        spin(&a, 0.0f, 200, out);
        check("engaging where you are is treated as rest",
              out[GA_OFF_RX] == 128, out[GA_OFF_RX], 128);

        /* From there, tilting further does push. */
        spin(&a, 45.0f, 500, out);
        check("and moving on from there pushes", out[GA_OFF_RX] > 150,
              out[GA_OFF_RX], 190);
        g_sample[GA_OFF_L2] = 0;
    }

    /* The contrast: read as a speed, the same hold springs back. */
    c.activation = GA_ACT_ALWAYS;
    ga_config_clamp(&c);
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

    printf("every rotation holds the same way in both directions\n");

    /* The complaint that prompted all of this: one way round saturated and
     * held, the other way did almost nothing. Each rotation is driven out and
     * back, each way round, and the two must mirror each other. */
    {
        static const struct { int src; int axis; const char *name; } cases[] = {
            { GA_SRC_YAW, 1, "turning" },
            { GA_SRC_PITCH, 0, "tilting" },
            { GA_SRC_ROLL, 2, "leaning" },
        };

        for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
            uint8_t out[GA_PAD_SIZE];
            int pushed[2], returned[2];

            for (int dir = 0; dir < 2; dir++) {
                float sign = dir ? -1.0f : 1.0f;

                ga_config_defaults(&c);
                c.activation = GA_ACT_ALWAYS;
                c.calibrate = 0;
                memset(c.route, 0, sizeof(c.route));
                c.route[cases[k].src][GA_OUT_RIGHT_X] = 1;
                c.read_mode[cases[k].src] = GA_READ_HOLD;
                c.full_angle[cases[k].src] = 30.0f;
                ga_config_clamp(&c);

                sample_reset();
                prepare(&a, &c);
                g_when = 1000000;
                rest_at(&a, cases[k].axis, 0.0f, 200, out);

                sweep(&a, cases[k].axis, 0.0f, sign * 60.0f, 1000, out);
                rest_at(&a, cases[k].axis, sign * 60.0f, 600, out);
                pushed[dir] = (int)out[GA_OFF_RX] - 128;

                sweep(&a, cases[k].axis, sign * 60.0f, 0.0f, 1000, out);
                rest_at(&a, cases[k].axis, 0.0f, 600, out);
                returned[dir] = (int)out[GA_OFF_RX] - 128;
            }

            char what[80];
            snprintf(what, sizeof(what), "%s pushes both ways", cases[k].name);
            check(what, pushed[0] > 100 && pushed[1] < -100, pushed[0],
                  -pushed[1]);
            snprintf(what, sizeof(what), "%s by the same amount each way",
                     cases[k].name);
            check(what, abs(pushed[0] + pushed[1]) <= 2, pushed[0],
                  -pushed[1]);
            snprintf(what, sizeof(what), "%s returns to rest both ways",
                     cases[k].name);
            check(what, abs(returned[0]) <= 2 && abs(returned[1]) <= 2,
                  returned[0], returned[1]);
        }
    }

    printf("one rotation after another holds just the same\n");

    /* The two directions on ONE state, in sequence, as a pair of hands does
     * it, because anything left over from the first is what breaks the
     * second. The routing is the reporter's own: two held rotations stacked
     * on one control, which together ask for more than it can give. */
    {
        uint8_t out[GA_PAD_SIZE];
        int held[2], rested[2];

        ga_config_defaults(&c);
        c.activation = GA_ACT_ALWAYS;
        c.calibrate = 0;
        memset(c.route, 0, sizeof(c.route));
        c.route[GA_SRC_YAW][GA_OUT_LEFT_X] = -1;
        c.route[GA_SRC_ROLL][GA_OUT_LEFT_X] = -1;
        c.read_mode[GA_SRC_YAW] = GA_READ_HOLD;
        c.read_mode[GA_SRC_ROLL] = GA_READ_HOLD;
        c.full_angle[GA_SRC_YAW] = 30.0f;
        c.full_angle[GA_SRC_ROLL] = 35.0f;
        c.strength[GA_SRC_YAW] = 1.55f;
        c.strength[GA_SRC_ROLL] = 1.70f;
        ga_config_clamp(&c);

        sample_reset();
        prepare(&a, &c);
        g_when = 1000000;
        rest_at(&a, 2, 0.0f, 300, out);

        for (int dir = 0; dir < 2; dir++) {
            float sign = dir ? -1.0f : 1.0f;
            sweep(&a, 2, 0.0f, sign * 60.0f, 1000, out);
            rest_at(&a, 2, sign * 60.0f, 600, out);
            held[dir] = (int)out[GA_OFF_LX] - 128;
            sweep(&a, 2, sign * 60.0f, 0.0f, 1000, out);
            rest_at(&a, 2, 0.0f, 600, out);
            rested[dir] = (int)out[GA_OFF_LX] - 128;
        }

        check("the first way out holds the control over", held[0] < -100,
              held[0], -120);
        check("the second way out holds it over just as far", held[1] > 100,
              held[1], 120);
        check("it settles at rest after the first", abs(rested[0]) <= 2,
              rested[0], 0);
        check("it settles at rest after the second", abs(rested[1]) <= 2,
              rested[1], 0);
    }

    printf("a remembered angle does not creep\n");

    /* Measured on a console after one session: turning sat 147 degrees from a
     * full scale of 30, leaning 51 the other way, with the pad lying still on
     * a table. Two held rotations on one control, pinned in opposite
     * directions, cancelling. The control was dead one way and welded over
     * the other, and asking for rest fixed it until it crept back.
     *
     * The cause was adding rates up with nothing to check them against. This
     * drives a pad that is not moving but whose gyro insists it is, which is
     * what a real one does, and the angle has to stay put. Tilting and
     * leaning can be checked against gravity and must hold near zero.
     * Turning a flat pad cannot: it turns about gravity itself, so there is
     * nothing to check it against and it is expected to creep. */
    {
        uint8_t out[GA_PAD_SIZE];
        const float creep = 20.0f;          /* degrees a second of untruth */

        ga_config_defaults(&c);
        c.activation = GA_ACT_ALWAYS;
        c.calibrate = 0;                    /* no bias learning to hide it */
        memset(c.route, 0, sizeof(c.route));
        c.read_mode[GA_SRC_YAW] = GA_READ_HOLD;
        c.read_mode[GA_SRC_PITCH] = GA_READ_HOLD;
        c.read_mode[GA_SRC_ROLL] = GA_READ_HOLD;
        ga_config_clamp(&c);

        sample_reset();
        prepare(&a, &c);
        g_when = 1000000;
        set_pose(2, 0.0f);                  /* flat, and it stays flat */

        /* Half a minute of a gyro reporting rotation that is not happening. */
        for (int tick = 0; tick < 3750; tick++) {
            for (int i = 0; i < 3; i++)
                set_float(GA_OFF_ANGVEL + 4 * i, creep / 57.29578f);
            a.age = 0;
            sample_timestamp(g_when);
            g_when += 8000;
            memset(out, 0, sizeof(out));
            ga_stub_read_state(1, out, &a);
        }

        check("tilting stays put", fabsf(a.held[GA_SRC_PITCH]) < 6.0f,
              a.held[GA_SRC_PITCH], 0.0);
        check("leaning stays put", fabsf(a.held[GA_SRC_ROLL]) < 6.0f,
              a.held[GA_SRC_ROLL], 0.0);
        check("and gravity is reported as the reference for both",
              a.hold_abs[GA_SRC_PITCH] && a.hold_abs[GA_SRC_ROLL], 1.0, 1.0);
        check("turning a flat pad says it has no reference",
              a.hold_abs[GA_SRC_YAW] == 0, 0.0, 0.0);
    }

    printf("the service republishing does not disturb the remembered angle\n");

    /* The service rewrites every settings field in one go from its own copy,
     * about ten times a second. This stands in for that.
     *
     * Rest must be taken once, when the condition engages, and never again
     * until it is asked for. Asking for it through a settings field meant the
     * service put its own value back a moment later and the angle was thrown
     * away a second time, part way into the rotation. So this counts how
     * often the remembered angle falls while the rotation is still going one
     * way, which can only happen when rest is retaken. Once is right, at the
     * engage itself. Twice is the fault.
     */
    {
        uint8_t buf[GA_PAD_SIZE];
        ga_args_t published;

        ga_config_defaults(&c);
        c.activation = GA_ACT_TRIGGER;
        c.trigger_right = 1;
        c.rest_on_activate = 1;
        c.calibrate = 0;
        memset(c.route, 0, sizeof(c.route));
        c.route[GA_SRC_YAW][GA_OUT_LEFT_X] = 1;
        c.read_mode[GA_SRC_YAW] = GA_READ_HOLD;
        c.full_angle[GA_SRC_YAW] = 30.0f;
        ga_config_clamp(&c);

        sample_reset();
        prepare(&a, &c);
        published = a;                  /* what the service holds and resends */
        /* As if rest had been asked for from the page a few times already,
         * which is what put the two counters out of step. */
        published.rest_epoch = 5;
        g_when = 1000000;

        g_sample[GA_OFF_R2] = 255;      /* the condition engages: rest is here */
        set_float(GA_OFF_ANGVEL + 4, 60.0f / 57.29578f);

        int retaken = 0, lowest = 127;
        float previous = 0.0f;
        for (int tick = 0; tick < 200; tick++) {
            if (tick % 10 == 0) {       /* the service publishes */
                size_t off = offsetof(ga_args_t, enabled);
                size_t len = offsetof(ga_args_t, settings_seq) +
                             sizeof(uint32_t) - off;
                memcpy((char *)&a + off, (const char *)&published + off, len);
            }
            a.age = 0;                  /* cleared on every publish */
            sample_timestamp(g_when);
            g_when += 8000;
            memset(buf, 0, sizeof(buf));
            ga_stub_read_state(1, buf, &a);

            if (a.angle[GA_SRC_YAW] < previous - 0.01f)
                retaken++;
            previous = a.angle[GA_SRC_YAW];

            if (tick > 100) {           /* well past full by now */
                int moved = (int)buf[GA_OFF_LX] - 128;
                if (moved < lowest)
                    lowest = moved;
            }
        }

        check("rest is taken once and not again", retaken == 0, retaken, 0);
        check("the control stays over through every republish", lowest > 100,
              lowest, 120);
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
