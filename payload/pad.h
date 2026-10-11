/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

/* The 120-byte sample libScePad fills in.
 *
 * Offsets are asserted below. `connected` landing at 0x4c is the anchor that
 * pins the motion and touch block above it. The order of acceleration and
 * angularVelocity inside that block is the one thing the public sources
 * disagreed on. It was settled on hardware: at rest, 0x1c reads about 1 g and
 * 0x28 reads about zero. See docs/CONSOLE_NOTES.md.
 */
typedef struct {
    uint16_t x, y;
    uint8_t  finger;
    uint8_t  reserved[3];
} ga_pad_touch_t;

typedef struct {
    uint32_t buttons;                       /* 0x00 */
    struct { uint8_t x, y; } left_stick;    /* 0x04 */
    struct { uint8_t x, y; } right_stick;   /* 0x06 */
    struct { uint8_t l2, r2; } triggers;    /* 0x08 */
    uint16_t pad0;                          /* 0x0a */
    struct { float x, y, z, w; } orientation;      /* 0x0c */
    struct { float x, y, z; } acceleration;        /* 0x1c, g */
    struct { float x, y, z; } angular_velocity;    /* 0x28, rad/s */
    uint8_t  finger_count;                  /* 0x34 */
    uint8_t  pad1[3];
    uint32_t pad2;
    ga_pad_touch_t touch[2];                /* 0x3c, 0x44 */
    int32_t  connected;                     /* 0x4c */
    uint64_t timestamp_us;                  /* 0x50 */
    uint8_t  extension[16];                 /* 0x58 */
    uint8_t  connected_count;               /* 0x68 */
    uint8_t  pad3[15];
} ga_pad_data_t;

_Static_assert(sizeof(ga_pad_data_t) == 120, "ScePadData must be 120 bytes");
_Static_assert(__builtin_offsetof(ga_pad_data_t, orientation) == 0x0c, "");
_Static_assert(__builtin_offsetof(ga_pad_data_t, acceleration) == 0x1c, "");
_Static_assert(__builtin_offsetof(ga_pad_data_t, angular_velocity) == 0x28, "");
_Static_assert(__builtin_offsetof(ga_pad_data_t, finger_count) == 0x34, "");
_Static_assert(__builtin_offsetof(ga_pad_data_t, touch) == 0x3c, "");
_Static_assert(__builtin_offsetof(ga_pad_data_t, connected) == 0x4c, "");
_Static_assert(__builtin_offsetof(ga_pad_data_t, timestamp_us) == 0x50, "");

/* Button bits. */
#define GA_BTN_L3        0x00000002u
#define GA_BTN_R3        0x00000004u
#define GA_BTN_OPTIONS   0x00000008u
#define GA_BTN_UP        0x00000010u
#define GA_BTN_RIGHT     0x00000020u
#define GA_BTN_DOWN      0x00000040u
#define GA_BTN_LEFT      0x00000080u
#define GA_BTN_L2        0x00000100u
#define GA_BTN_R2        0x00000200u
#define GA_BTN_L1        0x00000400u
#define GA_BTN_R1        0x00000800u
#define GA_BTN_TRIANGLE  0x00001000u
#define GA_BTN_CIRCLE    0x00002000u
#define GA_BTN_CROSS     0x00004000u
#define GA_BTN_SQUARE    0x00008000u
#define GA_BTN_TOUCH     0x00100000u
#define GA_BTN_INTERCEPT 0x80000000u

