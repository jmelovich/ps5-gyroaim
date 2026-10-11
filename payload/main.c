/* SPDX-License-Identifier: GPL-3.0-or-later
 * GyroAim - gyro aiming for any PS5 game.
 *
 * The resident service. It runs as an ordinary root process outside any title
 * sandbox, loaded by the console's ELF loader, and stays alive across game
 * launches. When a game starts it resolves the game's pad entry points, finds
 * the import slots that reach them, and installs the transform.
 */
#include "hook.h"
#include "inject.h"
#include "kstuff.h"
#include "log.h"
#include "pad.h"
#include "proc.h"
#include "stub.h"

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

#define GAME_PROC   "eboot.bin"
#define PIDFILE     "/data/gyroaim/pid"
#define TICK_US     100000
#define REPORT_MS   2000

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    g_quit = sig;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/* ---- single instance -------------------------------------------------- *
 *
 * The ELF loader names every payload "payload.elf", so matching running
 * processes by name would sweep up etaHEN and anything else the owner has
 * loaded. An early version of this file did that and killed two unrelated
 * payloads. The listening socket is the lock instead: only our own process
 * can hold it. The pidfile just says who to ask to leave, and we only ever
 * signal a pid we wrote there ourselves.
 */
static int bind_portal(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    /* Deliberately no SO_REUSEADDR: the bind failing is the signal. */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(GYROAIM_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static pid_t read_pidfile(void)
{
    FILE *f = fopen(PIDFILE, "r");
    long v = 0;

    if (!f)
        return -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = 0;
    fclose(f);
    return v > 0 ? (pid_t)v : -1;
}

static void write_pidfile(void)
{
    FILE *f = fopen(PIDFILE, "w");

    if (!f) {
        ga_log("cannot write %s; a later instance will not be able to "
               "replace this one cleanly", PIDFILE);
        return;
    }
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
}

static int claim_single_instance(void)
{
    int fd = bind_portal();

    if (fd >= 0)
        return fd;

    pid_t older = read_pidfile();

    if (older <= 0 || older == getpid()) {
        ga_log("port %d is already in use and %s names no owner", GYROAIM_PORT,
               PIDFILE);
        return -1;
    }
    if (!ga_proc_alive(older)) {
        ga_log("port %d is in use but the recorded owner (pid %d) is gone; "
               "not guessing who holds it", GYROAIM_PORT, (int)older);
        return -1;
    }

    ga_log("an older instance (pid %d) holds port %d; asking it to stop",
           (int)older, GYROAIM_PORT);
    if (kill(older, SIGTERM) != 0) {
        ga_log("could not signal pid %d: %s", (int)older, strerror(errno));
        return -1;
    }

    for (int waited = 0; waited < 5000; waited += 100) {
        usleep(100000);
        fd = bind_portal();
        if (fd >= 0) {
            ga_log("older instance stood down after %d ms", waited + 100);
            return fd;
        }
    }

    ga_log("older instance (pid %d) still holds port %d after 5 s",
           (int)older, GYROAIM_PORT);
    return -1;
}

/* ---- settings --------------------------------------------------------- */

/* Phase 2 defaults: gyro to the right stick while the left trigger is held.
 *
 * Sensitivity is in stick counts per rad/s and carries the axis direction,
 * because the stub must not negate a float. With 127 counts of travel, 40
 * means full deflection at about 3 rad/s, or 180 degrees a second of hand
 * movement.
 *
 * The signs follow the PlayStation convention of a right-handed frame with Y
 * up: positive Y is turning left, positive X is pitching up. Aiming left is a
 * smaller stick X, and the stub adds its offset, so yaw needs a negative
 * sensitivity. Aiming up is a smaller stick Y and the stub subtracts, so
 * pitch stays positive. Both are easy to flip once a human has felt it.
 */
static void default_settings(ga_args_t *s)
{
    memset(s, 0, sizeof(*s));
    s->magic = GA_MAGIC;
    s->version = GA_LAYOUT_VERSION;
    s->enabled = 1;
    s->target = GA_TARGET_RIGHT_STICK;
    s->activation = GA_ACT_TRIGGER;
    s->trigger_right = 0;              /* L2 */
    s->trigger_threshold = 64;         /* about a quarter press */
    s->activation_mask = GA_BTN_L2;
    s->axis_yaw = 1;
    s->axis_pitch = 0;
    s->sens_yaw = -40.0f;
    s->sens_pitch = 40.0f;
    s->stick_deadzone = 20;
    s->settings_seq = 1;
}

/* ---- self test -------------------------------------------------------- *
 *
 * Whether the stub writes the stick cannot be judged from a controller lying
 * still, because the gyro reads nearly zero and the output should then be
 * unchanged. Feeding in an artificial bias makes the expected output exact:
 * with a bias of -1 rad/s on the yaw axis the transform sees a steady 1 rad/s
 * turn and must deflect the stick by the sensitivity, in counts.
 *
 * This proves the whole path end to end without anyone touching the pad.
 */
static void selftest(const ga_inject_t *inj, const ga_args_t *base)
{
    ga_args_t probe = *base;
    ga_args_t got;

    probe.activation = GA_ACT_ALWAYS;
    probe.bias[1] = -1.0f;          /* axis_yaw is 1 */
    probe.settings_seq = base->settings_seq + 1;

    if (ga_inject_publish(inj, &probe) != 0) {
        ga_log("self test: could not publish");
        return;
    }
    usleep(500000);

    if (ga_inject_read(inj, &got) != 0) {
        ga_log("self test: could not read back");
        return;
    }

    /* The stub centres on 128 unless the physical stick is outside its
     * deadzone, and adds the offset for yaw. */
    int base_x = 128;
    if (abs((int)got.in_rx - 128) > (int)probe.stick_deadzone)
        base_x = got.in_rx;
    int want = base_x + (int)(got.used_yaw * probe.sens_yaw);
    if (want < 1)
        want = 1;
    if (want > 255)
        want = 255;

    ga_log("self test: yaw rate % .3f rad/s, expected stick x %d, got %d%s",
           (double)got.used_yaw, want, got.out_rx,
           got.out_rx == want ? "  PASS" : "  MISMATCH");
    ga_log("self test: applied %llu of %llu reads, active=%u",
           (unsigned long long)got.applied, (unsigned long long)got.calls,
           got.active);

    /* Put the real settings back. */
    ga_inject_publish(inj, base);
}

/* ---- the service ------------------------------------------------------ */

static void report(const ga_args_t *t)
{
    ga_log("pad: handle %d conn %u btn %08x L2 %3u  in R %3u,%3u -> out %3u,%3u"
           "  %s",
           t->last_handle, t->connected, t->last_buttons, t->last_l2,
           t->in_rx, t->in_ry, t->out_rx, t->out_ry,
           t->active ? "ACTIVE" : "idle");
    ga_log("  gyro rad/s % .4f % .4f % .4f   accel % .3f % .3f % .3f",
           (double)t->last_angvel[0], (double)t->last_angvel[1],
           (double)t->last_angvel[2], (double)t->last_accel[0],
           (double)t->last_accel[1], (double)t->last_accel[2]);
    ga_log("  used yaw % .4f pitch % .4f   reads %llu, applied %llu",
           (double)t->used_yaw, (double)t->used_pitch,
           (unsigned long long)t->calls, (unsigned long long)t->applied);
}

static int attach(pid_t game, const char *title, ga_inject_t *inj)
{
    ga_target_t target;
    ga_slot_t slots[GA_MAX_SLOTS];
    int n;

    for (int tries = 0; tries < 60 && !g_quit; tries++) {
        if (ga_hook_inspect(game, &target) == 0)
            break;
        if (tries == 59) {
            ga_log("%s: libScePad never appeared", title);
            return -1;
        }
        usleep(250000);
    }
    if (g_quit)
        return -1;

    n = ga_hook_find_slots(&target, slots, GA_MAX_SLOTS);
    if (n <= 0) {
        ga_log("%s: no slots in the relocation table; scanning by value",
               title);
        n = ga_hook_scan_slots(&target, slots, GA_MAX_SLOTS);
    }
    if (n <= 0) {
        ga_log("%s: found no pad import slots; leaving it alone", title);
        return -1;
    }

    if (ga_inject_install(&target, slots, (unsigned)n, inj) != 0) {
        ga_log("%s: install failed; the game is untouched", title);
        return -1;
    }
    return 0;
}

int main(void)
{
    pid_t game = -1;
    char title[GA_TITLE_ID_MAX] = "";
    ga_inject_t inj;
    ga_args_t settings;
    ga_args_t telemetry;
    int attached = 0;
    int64_t next_report = 0;
    int64_t ms = 0;

    memset(&inj, 0, sizeof(inj));
    default_settings(&settings);

    ga_log_open();
    ga_log("GyroAim starting (portal port %d)", GYROAIM_PORT);
#ifdef __PROSPERO__
    uint32_t fw = kernel_get_fw_version();
    /* Packed as hex digits: 0x13200006 is 13.20. */
    ga_log("firmware %x.%02x (0x%08x), pid %d", (fw >> 24) & 0xff,
           (fw >> 16) & 0xff, fw, (int)getpid());
#endif

    install_signal_handlers();

    if (ga_proc_self_check() != 0) {
        ga_log("refusing to continue: the process table is not readable");
        ga_log_close();
        return 1;
    }

    int portal = claim_single_instance();
    if (portal < 0) {
        ga_log("refusing to continue without the portal port");
        ga_log_close();
        return 1;
    }
    write_pidfile();

    if (!ga_kstuff_available()) {
        ga_log("kstuff remote syscalls are unavailable, so the transform "
               "cannot be installed; watching only");
    }

    ga_log("ready; watching for a game");

    while (!g_quit) {
        pid_t found[4];
        unsigned n = ga_proc_find(GAME_PROC, found, 4);

        if (n == 1 && found[0] != game) {
            game = found[0];
            if (ga_proc_title_id(game, title) != 0)
                snprintf(title, sizeof(title), "none");
            ga_log("game started: %s, pid %d", title, (int)game);

            attached = attach(game, title, &inj) == 0;
            if (attached) {
                ga_inject_publish(&inj, &settings);
                selftest(&inj, &settings);
                ga_log("%s: transform live", title);
                next_report = 0;
            }
        } else if (n == 0 && game != -1) {
            ga_log("game stopped: %s, pid %d", title, (int)game);
            /* The process is gone; there is nothing left to restore. */
            memset(&inj, 0, sizeof(inj));
            attached = 0;
            game = -1;
            title[0] = '\0';
        }

        if (attached) {
            /* Publishing also clears the staleness counter. If the service
             * stops, the stub stops modifying input shortly after. */
            if (ga_inject_publish(&inj, &settings) != 0) {
                ga_log("%s: lost contact with the shared page", title);
                attached = 0;
            } else if (ms >= next_report &&
                       ga_inject_read(&inj, &telemetry) == 0) {
                report(&telemetry);
                next_report = ms + REPORT_MS;
            }
        }

        usleep(TICK_US);
        ms += TICK_US / 1000;
    }

    ga_log("stopping on signal %d", (int)g_quit);
    if (attached && ga_proc_alive(inj.pid))
        ga_inject_remove(&inj);
    close(portal);
    unlink(PIDFILE);
    ga_log_close();
    return 0;
}
