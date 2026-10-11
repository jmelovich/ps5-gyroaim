/* SPDX-License-Identifier: GPL-3.0-or-later
 * curve_test.c - checks the sampled response curves on the host.
 *
 * The stub cannot evaluate a power curve, so the service samples the game's
 * response and its inverse into two tables. If those two are not actually
 * inverses, aiming is subtly wrong in a way that is very hard to feel out, so
 * it is worth testing away from the console.
 */
#include "../payload/config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *what, double a, double b)
{
    printf("  FAIL %s: %.5f vs %.5f\n", what, a, b);
    failures++;
}

/* Same interpolation the stub does. */
static float lut(const volatile float *t, float x)
{
    if (x < 0.0f)
        x = 0.0f;
    if (x > 1.0f)
        x = 1.0f;
    float scaled = x * (float)GA_LUT_LAST;
    int i = (int)scaled;
    if (i >= GA_LUT_LAST)
        return t[GA_LUT_LAST];
    return t[i] + (t[i + 1] - t[i]) * (scaled - (float)i);
}

static void check(float deadzone, float outer, float power)
{
    ga_config_t c;
    ga_args_t a;

    ga_config_defaults(&c);
    c.game_deadzone = deadzone;
    c.game_outer = outer;
    c.game_power = power;
    ga_config_clamp(&c);
    memset(&a, 0, sizeof(a));
    ga_config_to_args(&c, &a);

    printf("deadzone %.2f outer %.2f power %.2f\n", (double)c.game_deadzone,
           (double)c.game_outer, (double)c.game_power);

    /* Both tables must rise without stepping backwards, or the stub's
     * interpolation would make the camera jitter. */
    for (int i = 1; i < GA_LUT_N; i++) {
        if (a.lut_to_vel[i] < a.lut_to_vel[i - 1])
            fail("to_vel not rising", a.lut_to_vel[i - 1], a.lut_to_vel[i]);
        if (a.lut_to_stick[i] < a.lut_to_stick[i - 1])
            fail("to_stick not rising", a.lut_to_stick[i - 1],
                 a.lut_to_stick[i]);
    }

    /* Asking for no camera movement must mean no deflection, and asking for
     * everything must mean full deflection minus the outer deadzone. */
    if (a.lut_to_stick[0] != 0.0f)
        fail("to_stick(0) is not zero", a.lut_to_stick[0], 0.0);
    float full = a.lut_to_stick[GA_LUT_LAST];
    if (fabsf(full - (1.0f - c.game_outer)) > 0.01f)
        fail("to_stick(1) is not full travel", full, 1.0f - c.game_outer);

    /* Inside the deadzone the game does nothing, so the forward table must
     * agree. */
    if (a.lut_to_vel[0] != 0.0f)
        fail("to_vel(0) is not zero", a.lut_to_vel[0], 0.0);

    /* The round trip is the point: a wanted speed turned into a deflection
     * and back must come out as the same speed. */
    double worst = 0;
    for (int i = 1; i <= 40; i++) {
        float v = (float)i / 40.0f;
        float d = lut(a.lut_to_stick, v);
        float back = lut(a.lut_to_vel, d);
        double err = fabs((double)back - (double)v);
        if (err > worst)
            worst = err;
        if (err > 0.02)
            fail("round trip", v, back);
    }
    printf("  worst round-trip error %.4f\n", worst);
}

int main(void)
{
    check(0.0f, 0.0f, 1.0f);      /* a game with no deadzone and no curve */
    check(0.15f, 0.02f, 1.6f);    /* the defaults */
    check(0.30f, 0.10f, 2.5f);    /* a heavy deadzone and a strong curve */
    check(0.05f, 0.0f, 0.7f);     /* a curve that bends the other way */

    printf(failures ? "\n%d failure(s)\n" : "\nall curve checks passed\n",
           failures);
    return failures ? 1 : 0;
}
