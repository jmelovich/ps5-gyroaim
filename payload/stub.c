/* SPDX-License-Identifier: GPL-3.0-or-later
 * stub.c - the transform, as it runs inside the game.
 *
 * This code is compiled here but executed in another process: the service
 * copies the whole block below into a mapping inside the game and points one
 * of the game's import slots at it. Three rules follow from that, and
 * breaking any of them crashes the game rather than failing politely:
 *
 *   1. No calls out. Sony's functions are reached only through a->orig[].
 *      No libc, no libm, no helper in another file.
 *   2. No references to anything outside the block. In particular no float or
 *      double literals and no string literals, because the compiler puts
 *      those in .rodata and reaches them with a RIP-relative load that would
 *      point into open space once the code is copied. Every constant that is
 *      not an integer immediate lives in the shared page.
 *   3. Everything stays in section .text.gastub, ahead of ga_stub_end, so the
 *      service can find the block's bounds and copy it whole.
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

/* 1..255 with 128 as centre. Zero is avoided because some games read 0 as a
 * full deflection in the negative direction. */
static INLINE uint8_t to_axis(int v)
{
    if (v < 1)
        return 1;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static INLINE int axis_deflected(const uint8_t *data, int off, uint32_t dz)
{
    int v = (int)data[off] - 128;
    if (v < 0)
        v = -v;
    return (uint32_t)v > dz;
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

/* The whole transform. Phase 2 shape: bias, scale, add to whatever the stick
 * is already doing, clamp. The response curve, smoothing and deadzone
 * compensation come later and will slot in here. */
static INLINE void transform(ga_args_t *a, uint8_t *data, int32_t handle)
{
    uint32_t age = __atomic_add_fetch(&a->age, 1u, __ATOMIC_RELAXED);

    /* Always record what arrived, even when parked, so the service can show
     * live values while the user is still setting things up. */
    float av[3], ac[3];
    for (int i = 0; i < 3; i++) {
        av[i] = load_float(data + GA_OFF_ANGVEL + 4 * i);
        ac[i] = load_float(data + GA_OFF_ACCEL + 4 * i);
        a->last_angvel[i] = av[i];
        a->last_accel[i] = ac[i];
    }
    a->last_handle = handle;
    a->last_buttons = load_u32(data + GA_OFF_BUTTONS);
    a->last_l2 = data[GA_OFF_L2];
    a->last_r2 = data[GA_OFF_R2];
    a->in_rx = data[GA_OFF_RX];
    a->in_ry = data[GA_OFF_RY];
    a->connected = data[GA_OFF_CONNECTED];
    a->last_timestamp = load_u64(data + GA_OFF_TIMESTAMP);

    int active = a->enabled && age <= GA_STALE_READS && is_active(a, data);

    if (!active) {
        a->active = 0;
        a->out_rx = data[GA_OFF_RX];
        a->out_ry = data[GA_OFF_RY];
        a->used_yaw = 0;
        a->used_pitch = 0;
        __atomic_add_fetch(&a->sample_seq, 1u, __ATOMIC_RELAXED);
        return;
    }

    uint32_t iy = a->axis_yaw > 2 ? 1 : a->axis_yaw;
    uint32_t ip = a->axis_pitch > 2 ? 0 : a->axis_pitch;

    /* No negation here on purpose. Writing -x makes the compiler load a sign
     * mask from a constant pool in .rodata, which does not come along when
     * the block is copied. Axis inversion is folded into the sign of the
     * sensitivity by the service instead. */
    float rate_yaw = av[iy] - a->bias[iy];
    float rate_pitch = av[ip] - a->bias[ip];

    a->used_yaw = rate_yaw;
    a->used_pitch = rate_pitch;

    int dx = (int)(rate_yaw * a->sens_yaw);
    int dy = (int)(rate_pitch * a->sens_pitch);

    int xoff = GA_OFF_RX, yoff = GA_OFF_RY;
    if (a->target == GA_TARGET_LEFT_STICK) {
        xoff = GA_OFF_LX;
        yoff = GA_OFF_LY;
    }

    /* Add to the physical stick when it is being used, so holding the stick
     * and turning the pad both steer. Outside the deadzone the stick's own
     * value is the base; inside it, centre is. */
    int basex = axis_deflected(data, xoff, a->stick_deadzone)
                    ? (int)data[xoff] : 128;
    int basey = axis_deflected(data, yoff, a->stick_deadzone)
                    ? (int)data[yoff] : 128;

    /* Screen y grows downward, so looking up is a smaller value. */
    uint8_t nx = to_axis(basex + dx);
    uint8_t ny = to_axis(basey - dy);

    data[xoff] = nx;
    data[yoff] = ny;

    a->out_rx = nx;
    a->out_ry = ny;
    a->active = 1;
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
 * wrote, oldest first, so every one gets the same treatment. */
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

/* Marks the end of the block. Must be the last thing in the section. */
STUB void ga_stub_end(void)
{
}
