/* SPDX-License-Identifier: GPL-3.0-or-later
 * pad.c - reading the DualSense from the service itself.
 *
 * This exists to answer one question before any game memory is touched: does
 * a pad sample actually carry gyro data, in what units, and at which offset.
 * It opens the pad, logs what it sees, and closes it again.
 */
#include "pad.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* libScePad. No public header ships with the SDK, so these are declared here.
 * Keep them in one place; the stub resolves the same names in the game. */
extern int32_t scePadInit(void);
extern int32_t scePadOpen(int32_t user_id, int32_t type, int32_t index,
                          const void *param);
extern int32_t scePadClose(int32_t handle);
extern int32_t scePadReadState(int32_t handle, void *data);
extern int32_t scePadRead(int32_t handle, void *data, int32_t count);
extern int32_t scePadSetMotionSensorState(int32_t handle, int enabled);
extern int32_t scePadSetAngularVelocityDeadbandState(int32_t handle,
                                                     int enabled);
extern int32_t scePadGetHandle(int32_t user_id, int32_t type, int32_t index);

extern int32_t sceUserServiceInitialize(const void *params);
extern int32_t sceUserServiceGetLoginUserIdList(int32_t list[4]);

static float vec_len(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

static void log_raw(const ga_pad_data_t *d)
{
    const uint8_t *b = (const uint8_t *)d;
    char line[3 * 24 + 8];

    /* The motion block only: 0x0c to 0x33. */
    for (unsigned row = 0x0c; row < 0x34; row += 16) {
        unsigned n = 0;
        for (unsigned i = row; i < row + 16 && i < 0x34; i++)
            n += (unsigned)snprintf(line + n, sizeof(line) - n, "%02x ", b[i]);
        ga_log("  raw %02x: %s", row, line);
    }
}

int ga_pad_probe(unsigned samples)
{
    int32_t users[4] = { -1, -1, -1, -1 };
    int32_t handle = -1;
    int rc;

    if ((rc = sceUserServiceInitialize(NULL)) != 0)
        ga_log("pad probe: sceUserServiceInitialize returned 0x%x "
               "(continuing, it may already be up)", rc);

    if ((rc = scePadInit()) != 0)
        ga_log("pad probe: scePadInit returned 0x%x (continuing)", rc);

    if ((rc = sceUserServiceGetLoginUserIdList(users)) != 0) {
        ga_log("pad probe: no logged-in user list (0x%x); cannot open a pad",
               rc);
        return -1;
    }
    ga_log("pad probe: logged-in users %d %d %d %d",
           users[0], users[1], users[2], users[3]);

    for (int i = 0; i < 4 && handle < 0; i++) {
        if (users[i] < 0)
            continue;
        handle = scePadOpen(users[i], 0, 0, NULL);
        if (handle < 0) {
            ga_log("pad probe: scePadOpen(user %d) returned 0x%x",
                   users[i], handle);
            /* A pad a game already owns may only be reachable by handle. */
            int32_t h = scePadGetHandle(users[i], 0, 0);
            ga_log("pad probe: scePadGetHandle(user %d) -> 0x%x", users[i], h);
            if (h > 0)
                handle = h;
        } else {
            ga_log("pad probe: opened user %d as handle %d", users[i], handle);
        }
    }

    if (handle < 0) {
        ga_log("pad probe: could not get a pad handle");
        return -1;
    }

    rc = scePadSetMotionSensorState(handle, 1);
    ga_log("pad probe: scePadSetMotionSensorState(on) -> 0x%x", rc);
    rc = scePadSetAngularVelocityDeadbandState(handle, 0);
    ga_log("pad probe: scePadSetAngularVelocityDeadbandState(off) -> 0x%x", rc);

    /* Let the motion sensor spin up before judging whether it reports. */
    usleep(200000);

    unsigned nonzero_motion = 0, reads_ok = 0;
    uint64_t first_ts = 0, last_ts = 0;

    for (unsigned i = 0; i < samples; i++) {
        ga_pad_data_t d;
        memset(&d, 0, sizeof(d));

        rc = scePadReadState(handle, &d);
        if (rc != 0) {
            ga_log("pad probe: scePadReadState -> 0x%x", rc);
            usleep(16000);
            continue;
        }
        reads_ok++;
        if (!first_ts)
            first_ts = d.timestamp_us;
        last_ts = d.timestamp_us;

        float a = vec_len(d.acceleration.x, d.acceleration.y,
                          d.acceleration.z);
        float w = vec_len(d.angular_velocity.x, d.angular_velocity.y,
                          d.angular_velocity.z);
        if (a > 0.0f || w > 0.0f)
            nonzero_motion++;

        /* Log a handful, spread out, rather than every sample. */
        if (i < 3 || i % 30 == 0) {
            ga_log("pad probe: conn=%d btn=%08x L=%3u,%3u R=%3u,%3u "
                   "L2=%3u R2=%3u fingers=%u",
                   d.connected, d.buttons,
                   d.left_stick.x, d.left_stick.y,
                   d.right_stick.x, d.right_stick.y,
                   d.triggers.l2, d.triggers.r2, d.finger_count);
            ga_log("  at 0x1c: % .4f % .4f % .4f  |v|=%.4f",
                   (double)d.acceleration.x, (double)d.acceleration.y,
                   (double)d.acceleration.z, (double)a);
            ga_log("  at 0x28: % .4f % .4f % .4f  |v|=%.4f  (%.2f deg/s)",
                   (double)d.angular_velocity.x, (double)d.angular_velocity.y,
                   (double)d.angular_velocity.z, (double)w,
                   (double)(w * 57.29578f));
            ga_log("  quat   : % .4f % .4f % .4f % .4f",
                   (double)d.orientation.x, (double)d.orientation.y,
                   (double)d.orientation.z, (double)d.orientation.w);
            if (i == 0)
                log_raw(&d);
        }
        usleep(8000);
    }

    ga_log("pad probe: %u/%u reads ok, %u carried motion",
           reads_ok, samples, nonzero_motion);
    if (reads_ok >= 2 && last_ts > first_ts)
        ga_log("pad probe: timestamps spanned %llu us over %u reads",
               (unsigned long long)(last_ts - first_ts), reads_ok);
    if (!nonzero_motion)
        ga_log("pad probe: motion fields were all zero - the sensor is off, "
               "or it lives at a different offset");
    else
        ga_log("pad probe: whichever of 0x1c / 0x28 reads about 1.0 at rest "
               "is acceleration in g; the other is angular velocity");

    scePadClose(handle);
    return reads_ok ? 0 : -1;
}
