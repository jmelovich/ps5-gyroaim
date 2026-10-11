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

/* Where the gyro goes. */
enum {
    GA_TARGET_RIGHT_STICK = 0,
    GA_TARGET_LEFT_STICK,
    GA_TARGET_TOUCHPAD,
};

/* What has to be true for the gyro to apply. */
enum {
    GA_ACT_ALWAYS = 0,
    GA_ACT_BUTTON,        /* any bit of activation_mask held */
    GA_ACT_TRIGGER,       /* l2/r2 at or past the threshold */
    GA_ACT_NEVER,         /* parked: the transform runs but changes nothing */
};

#define GA_MAGIC          0x4f524947u   /* "GYRO" */
#define GA_LAYOUT_VERSION 1u

/* How many reads without a settings refresh before the stub stops touching
 * the data. If the service dies, the game goes back to normal input rather
 * than freezing mid-turn. */
#define GA_STALE_READS    600u

/* The page the service and the stub share.
 *
 * Everything the stub needs is in here: it makes no reference to anything
 * outside this struct and its own code, which is what lets the code be copied
 * into another process and still work.
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t orig[GA_FN_COUNT];      /* Sony's real entry points */

    /* ---- settings: service writes, stub reads ---- */
    volatile uint32_t enabled;
    volatile uint32_t target;            /* GA_TARGET_* */
    volatile uint32_t activation;        /* GA_ACT_* */
    volatile uint32_t activation_mask;   /* button bits for GA_ACT_BUTTON */
    volatile uint32_t trigger_right;     /* 0 uses L2, 1 uses R2 */
    volatile uint32_t trigger_threshold; /* 0-255 */
    volatile uint32_t axis_yaw;          /* index into angularVelocity */
    volatile uint32_t axis_pitch;
    /* Sensitivity carries the axis direction: the service negates it rather
     * than the stub negating a float, which would need a constant from
     * .rodata that the copied block cannot reach. */
    volatile float    sens_yaw;          /* stick units per rad/s, signed */
    volatile float    sens_pitch;
    volatile float    bias[3];           /* rad/s, removed before scaling */
    volatile uint32_t stick_deadzone;    /* physical stick counts from centre */
    volatile uint32_t settings_seq;      /* bumped on every refresh */

    /* ---- telemetry: stub writes, service reads ---- */
    volatile uint64_t calls;
    volatile uint64_t applied;
    volatile uint32_t sample_seq;
    volatile uint32_t age;               /* reads since the last refresh */
    volatile int32_t  last_handle;
    volatile uint32_t last_buttons;
    volatile float    last_angvel[3];
    volatile float    last_accel[3];
    volatile float    used_yaw;          /* after bias and inversion */
    volatile float    used_pitch;
    volatile uint8_t  in_rx, in_ry, out_rx, out_ry;
    volatile uint8_t  last_l2, last_r2;
    volatile uint8_t  active;
    volatile uint8_t  connected;
    volatile uint64_t last_timestamp;

    /* ---- how to put the game back ---- */
    uint32_t hook_count;
    uint64_t hook_slot[GA_MAX_SLOTS];
    uint64_t hook_orig[GA_MAX_SLOTS];
    uint32_t hook_prot[GA_MAX_SLOTS];
} ga_args_t;

/* The injected functions. They are never called in this process; their
 * addresses are used to find and copy the code. */
int32_t ga_stub_read_state(int32_t handle, uint8_t *data, ga_args_t *a);
int32_t ga_stub_read_state_ext(int32_t handle, uint8_t *data, ga_args_t *a);
int32_t ga_stub_read(int32_t handle, uint8_t *data, int32_t count,
                     ga_args_t *a);
void    ga_stub_end(void);
