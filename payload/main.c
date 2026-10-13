/* SPDX-License-Identifier: GPL-3.0-or-later
 * GyroAim - gyro aiming for any PS5 game.
 *
 * The resident service. It runs as an ordinary root process outside any title
 * sandbox, loaded by the console's ELF loader, and stays alive across game
 * launches. When a game starts it resolves the game's pad entry points, finds
 * the import slots that reach them, and installs the transform.
 */
#include "config.h"
#include "hook.h"
#include "inject.h"
#include "kstuff.h"
#include "log.h"
#include "pad.h"
#include "proc.h"
#include "stub.h"
#include "version.h"
#include "web.h"

#include <errno.h>
#include <math.h>
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
    /* A scan in progress must give up, or a shutdown waits minutes for it and
     * a replacement instance gives up and leaves two of us running. */
    ga_hook_cancel = 1;
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

    /* SO_REUSEADDR, but never SO_REUSEPORT.
     *
     * The bind failing is how a second instance learns the first is running,
     * so two processes must never be able to listen at once, which is what
     * SO_REUSEPORT would allow. SO_REUSEADDR only waives the wait for
     * connections left in TIME_WAIT by closed portal requests. Without it a
     * replacement instance cannot rebind for a minute or so after anyone has
     * used the portal, which is exactly when a replacement is most likely. */
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

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

static pid_t read_pidfile_value(void);

/* The pidfile is the authority on which instance is in charge.
 *
 * The listening socket alone turned out not to be enough. Two instances were
 * found running at once: an old one still hooked into a game and publishing to
 * it, and a new one holding the portal and unable to recognise the slot the
 * old one owned. Aiming worked, from the old one, while the portal reported
 * that it could not attach. However that came about, an instance that has been
 * superseded has no business still driving a game, so each one checks that the
 * pidfile still names it and stands down if it does not. */
static int superseded(void)
{
    pid_t owner = read_pidfile_value();

    if (owner <= 0 || owner == getpid())
        return 0;
    /* Someone else's pid is in the file. If they are alive, they are in
     * charge; if not, the file is stale and ours to reclaim. */
    return ga_proc_alive(owner);
}

static pid_t read_pidfile_value(void)
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

static pid_t read_pidfile(void)
{
    return read_pidfile_value();
}

/* Only remove the pidfile if it still names us.
 *
 * It did not check, and that broke the lock: an older instance, asked to
 * leave, deleted the file a newer one had already written, after which no
 * instance could tell who was in charge. A later start then found the port
 * held and the file empty, refused to run, and left the stuck instance in
 * place. */
static void clear_pidfile(void)
{
    if (read_pidfile_value() == getpid())
        unlink(PIDFILE);
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

/* ---- self test -------------------------------------------------------- *
 *
 * Feeds the transform an artificial resting offset so it sees a steady turn
 * with nobody touching the pad, then checks that the camera was asked to move
 * in the right direction by roughly the sensitivity, and that a deflection
 * came out. It deliberately does not recompute the whole pipeline: that would
 * only test the test.
 */
__attribute__((unused))
static void selftest(const ga_inject_t *inj, const ga_args_t *base,
                     const ga_config_t *cfg)
{
    ga_args_t probe = *base;
    ga_args_t got;

    probe.activation = GA_ACT_ALWAYS;
    probe.bias[1] = -1.0f;              /* the yaw axis defaults to gyro Y */
    probe.tighten = 0.0f;
    probe.smooth_rate = 0.0f;
    probe.settings_seq = base->settings_seq + 1;

    if (ga_inject_publish(inj, &probe) != 0) {
        ga_log("self test: could not publish");
        return;
    }
    usleep(600000);

    if (ga_inject_read(inj, &got) != 0) {
        ga_log("self test: could not read back");
        ga_inject_publish(inj, base);
        return;
    }

    /* One radian a second is 57.3 degrees a second. */
    float want_rate = 57.29578f;
    int rate_ok = fabsf(got.rate[GA_SRC_YAW] - want_rate) < 3.0f;
    int moved = got.out_frac > 0.01f;

    ga_log("self test: yaw rate % .1f deg/s (want % .1f) %s",
           (double)got.rate[GA_SRC_YAW], (double)want_rate,
           rate_ok ? "ok" : "WRONG");
    ga_log("self test: right stick asked for % .1f, % .1f deg/s",
           (double)got.want[0][0], (double)got.want[0][1]);
    ga_log("self test: deflection %.3f, right stick %u,%u -> %s",
           (double)got.out_frac, got.out_rx, got.out_ry,
           moved ? "PASS" : "FAIL");

    (void)cfg;
    ga_inject_publish(inj, base);
}

/* ---- the service ------------------------------------------------------ */

static void report(const ga_args_t *t)
{
    ga_log("pad: L2 %3u  right %3u,%3u -> %3u,%3u  left %3u,%3u -> %3u,%3u  "
           "%s%s",
           t->last_l2, t->in_rx, t->in_ry, t->out_rx, t->out_ry,
           t->in_lx, t->in_ly, t->out_lx, t->out_ly,
           t->active ? "aiming" : "idle",
           t->saturated ? " (at the limit)" : "");
    ga_log("  turn % .1f  tilt % .1f  lean % .1f deg/s   deflection %.3f",
           (double)t->rate[GA_SRC_YAW], (double)t->rate[GA_SRC_PITCH],
           (double)t->rate[GA_SRC_ROLL], (double)t->out_frac);
    ga_log("  rumble level %u (mode set at %llu)", t->rumble_last_level,
           (unsigned long long)t->rumble_mode_ts);
    ga_log("  pad read returned 0x%x, handle %d, connected %u",
           (unsigned)t->last_result, t->last_handle, t->connected);
    /* Whether aiming was engaged no longer decides anything: a window is
     * judged by how small the offset in it is. Saying "not usable" here was
     * left over from when it did, and with the condition set to Always it
     * labelled every window that way while the offset was in fact being
     * learned from all of them. */
    ga_log("  reads %llu, applied %llu, stillness window %u sample(s)%s",
           (unsigned long long)t->calls, (unsigned long long)t->applied,
           t->cal_count, t->cal_was_active ? " (while aiming)" : "");
}

/* One attempt at hooking a game.
 *
 * Returns 0 once installed, 1 while the game is not ready yet, and -1 when an
 * install was tried and failed. "Not ready" is the normal case for the first
 * second or two of a launch: the pad library is loaded a moment after the
 * process appears, and its import slots are not bound until something calls
 * through them. A game can also reach that point much later than it starts,
 * which is why this is retried rather than attempted once.
 *
 * `deep` allows the value scan, which reads a module end to end. That is the
 * only way to find slots in a module built without a PLT, but on a large
 * retail executable it is expensive, so the caller only asks for it
 * occasionally.
 */
static int attach_once(pid_t game, const char *title, int deep,
                       ga_scan_cursor_t *cursor, ga_inject_t *inj)
{
    ga_target_t target;
    ga_slot_t slots[GA_MAX_SLOTS];
    int n;

    if (ga_hook_inspect(game, &target) != 0)
        return 1;               /* no pad library yet */

    /* An earlier run may already be wired into this game: killed or displaced
     * before it could restore the slots, it leaves them pointing at gateways
     * rather than Sony's exports. The finders below notice that in passing.
     * Taking such an installation over costs nothing and touches none of the
     * game's memory; without it, this game could never be attached to again,
     * while its stub carried on running with nobody feeding it. */
    ga_orphan_t orphan;

    n = ga_hook_find_slots(&target, slots, GA_MAX_SLOTS, &orphan);

    /* The sweep is for modules built without a PLT, which have no relocations
     * to walk. A large one is read across several passes, so once started it
     * continues on every attempt rather than waiting for the slow cadence. */
    if (n <= 0 && !orphan.args && (deep || cursor->mapbase))
        n = ga_hook_scan_slots(&target, slots, GA_MAX_SLOTS, &orphan, cursor);

    if (orphan.args)
        return ga_inject_adopt(&target, &orphan, inj) == 0 ? 0 : -1;

    if (n <= 0)
        return 1;               /* not bound yet, still reading, or none */

    if (ga_inject_install(&target, slots, (unsigned)n, inj) != 0) {
        ga_log("%s: install failed; the game is untouched", title);
        ga_kstuff_diagnose(game);
        return -1;
    }
    return 0;
}

/* Keeps the portal's view of the world current. */
static void publish_status(ga_web_state_t *web, int attached,
                           const char *title, const char *status,
                           const ga_args_t *live, int have_live)
{
    pthread_mutex_lock(&web->lock);
    web->attached = attached;
    snprintf(web->title, sizeof(web->title), "%s", title ? title : "");
    snprintf(web->status, sizeof(web->status), "%s", status);
    if (live && have_live) {
        web->live = *live;
        web->have_live = 1;
    }
    pthread_mutex_unlock(&web->lock);
}

int main(void)
{
    pid_t game = -1;
    char title[GA_TITLE_ID_MAX] = "";
    char active_path[GA_PATH_MAX];
    uint32_t cal_epoch = 1;
    ga_inject_t inj;
    ga_args_t settings;
    ga_args_t telemetry;
    ga_web_state_t web;
    int attached = 0;
    int64_t next_report = 0;
    int64_t next_attach = 0;
    int64_t game_since = 0;
    int64_t next_deep = 0;
    ga_scan_cursor_t cursor;
    int attach_state = 0;       /* last reported, so it is logged once */
    int64_t ms = 0;

    memset(&inj, 0, sizeof(inj));
    memset(&cursor, 0, sizeof(cursor));
    memset(&settings, 0, sizeof(settings));
    memset(&web, 0, sizeof(web));
    pthread_mutex_init(&web.lock, NULL);
    ga_config_defaults(&web.cfg);

    ga_log_open();
    ga_log("GyroAim %s starting (portal port %d)", GA_VERSION, GYROAIM_PORT);
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

    ga_config_for_title(&web.cfg, NULL, active_path, sizeof(active_path));
    /* Written back whole. A file from an older build gains the keys added
     * since, rather than leaving them to the defaults on every run. These are
     * the base layer, so a complete set is exactly what is wanted here; a
     * game's own profile stays sparse and overrides only what it names. */
    if (ga_config_save(&web.cfg, active_path) == 0)
        ga_log("settings kept in %s", active_path);
    ga_config_to_args(&web.cfg, &settings);
    settings.cal_epoch = cal_epoch;
    snprintf(web.path, sizeof(web.path), "%s", active_path);

    ga_web_start(portal, &web);

    web.kstuff = ga_kstuff_available();
    if (!web.kstuff)
        ga_log("kstuff remote syscalls are unavailable, so the transform "
               "cannot be installed; watching only");

    ga_log("ready; watching for a game");
    publish_status(&web, 0, "", "waiting for a game to start", NULL, 0);

    while (!g_quit) {
        pid_t found[4];
        unsigned n = ga_proc_find(GAME_PROC, found, 4);

        /* Another instance has taken over: unhook and go, rather than two of
         * us driving the same game. */
        if (superseded()) {
            ga_log("another instance owns %s now; standing down", PIDFILE);
            break;
        }

        /* Settings changed in the portal: fold them in and remember them. */
        pthread_mutex_lock(&web.lock);
        if (web.quit_request) {
            int uninstall = web.uninstall;
            web.quit_request = 0;
            web.uninstall = 0;
            pthread_mutex_unlock(&web.lock);
            if (uninstall) {
                int gone = ga_uninstall(uninstall == 2);
                ga_log("the portal asked the service to uninstall itself; "
                       "%d file(s) removed, settings %s", gone,
                       uninstall == 2 ? "removed too" : "kept");
            } else {
                ga_log("the portal asked the service to stop");
            }
            break;      /* the shutdown path below unhooks the game */
        }
        if (web.save_as_title && title[0]) {
            ga_config_title_path(title, active_path, sizeof(active_path));
            snprintf(web.path, sizeof(web.path), "%s", active_path);
            web.save_as_title = 0;
            web.cfg_dirty = 1;
            ga_log("settings will now be kept in %s", active_path);
        }
        if (web.rest_request) {
            web.rest_request = 0;
            settings.rest_epoch++;
            ga_log("rest position set where the pad is now");
        }
        if (web.reload_request) {
            web.reload_request = 0;
            if (ga_config_for_title(&web.cfg, title, active_path,
                                    sizeof(active_path)) >= 0) {
                ga_log("settings reread from %s", active_path);
                web.cfg_dirty = 1;
            }
        }
        if (web.cfg_dirty) {
            ga_config_to_args(&web.cfg, &settings);
            settings.cal_epoch = cal_epoch;
            web.cfg_dirty = 0;
            ga_config_save(&web.cfg, active_path);
        }
        pthread_mutex_unlock(&web.lock);

        if (n == 1 && found[0] != game) {
            game = found[0];
            if (ga_proc_title_id(game, title) != 0)
                snprintf(title, sizeof(title), "none");
            ga_log("game started: %s, pid %d", title, (int)game);

            if (!strcmp(title, GA_PANEL_TITLE_ID)) {
                ga_log("%s is our own control panel; leaving it alone", title);
                publish_status(&web, 0, title,
                               "the control panel is open", NULL, 0);
                attached = 0;
                usleep(TICK_US);
                ms += TICK_US / 1000;
                continue;
            }

            /* Rebuilt from the defaults and the general settings up, so
             * nothing of the game just left can survive into this one. */
            pthread_mutex_lock(&web.lock);
            int own = ga_config_for_title(&web.cfg, title, active_path,
                                          sizeof(active_path));
            ga_log(own == 1 ? "using the profile saved for %s"
                            : "no profile saved for %s; general settings",
                   title);
            snprintf(web.path, sizeof(web.path), "%s", active_path);
            ga_config_to_args(&web.cfg, &settings);
            settings.cal_epoch = cal_epoch;
            pthread_mutex_unlock(&web.lock);

            publish_status(&web, 0, title, "attaching", NULL, 0);
            attached = 0;
            attach_state = 0;
            game_since = ms;
            next_attach = ms;
            next_deep = ms;
            memset(&cursor, 0, sizeof(cursor));
        } else if (n == 0 && game != -1) {
            ga_log("game stopped: %s, pid %d", title, (int)game);
            /* The process is gone; there is nothing left to restore, and the
             * mapping went with it. */
            ga_inject_forget(game);
            memset(&inj, 0, sizeof(inj));
            attached = 0;
            game = -1;
            title[0] = '\0';
            publish_status(&web, 0, "", "waiting for a game to start",
                           NULL, 0);
        }

        /* Keep trying while a game is running and the transform is not in.
         * A game can load its pad library, and bind the imports that reach
         * it, well after the process appears. */
        if (!attached && game != -1 && ms >= next_attach && !superseded()) {
            /* Checked again here, not only at the top of the loop: attaching
             * walks another process's memory with primitives that are not safe
             * to use from two instances at once. */
            int deep = ms >= next_deep;
            int rc = attach_once(game, title, deep, &cursor, &inj);

            if (deep)
                next_deep = ms + 15000;

            if (rc == 0) {
                attached = 1;
                ga_inject_publish(&inj, &settings);
                if (GA_SELFTEST)
                    selftest(&inj, &settings, &web.cfg);
                ga_log("%s: transform live%s", title,
                       ms - game_since > 2000 ? " (it took a moment)" : "");
                publish_status(&web, 1, title, "transform live", NULL, 0);
                next_report = 0;
            } else {
                /* Often for the first second or two of a launch, so the retry
                 * is quick at first and then eases off rather than reading a
                 * large executable over and over. */
                /* Mid-sweep, come straight back; otherwise ease off. */
                next_attach = cursor.mapbase
                    ? ms + 100
                    : ms + (ms - game_since < 60000 ? 1000 : 15000);

                if (rc != attach_state) {
                    attach_state = rc;
                    if (rc == 1)
                        ga_log("%s: waiting for the game to load its "
                               "controller code", title);
                }
                publish_status(&web, 0, title,
                               rc == 1 ? "waiting for the game to be ready"
                                       : "could not attach to this game",
                               NULL, 0);
            }
        }

        if (attached) {
            /* Publishing also clears the staleness counter. If the service
             * stops, the stub stops modifying input shortly after. */
            if (ga_inject_publish(&inj, &settings) != 0) {
                ga_log("%s: lost contact with the shared page", title);
                attached = 0;
                publish_status(&web, 0, title,
                               "lost contact with the transform", NULL, 0);
            } else if (ga_inject_read(&inj, &telemetry) == 0) {
                publish_status(&web, 1, title, "transform live", &telemetry, 1);

                /* Learn the gyro's resting offset while the pad is still. */
                pthread_mutex_lock(&web.lock);
                uint32_t before = cal_epoch;
                int learned = ga_config_calibrate(&web.cfg, &telemetry,
                                                  &cal_epoch);
                if (learned) {
                    for (int i = 0; i < 3; i++)
                        settings.bias[i] = web.cfg.bias[i];
                    ga_config_save(&web.cfg, active_path);
                }
                if (cal_epoch != before)
                    settings.cal_epoch = cal_epoch;
                pthread_mutex_unlock(&web.lock);

                if (ms >= next_report) {
                    report(&telemetry);
                    next_report = ms + REPORT_MS;
                }
            }
        }

        usleep(TICK_US);
        ms += TICK_US / 1000;
    }

    if (g_quit)
        ga_log("stopping on signal %d", (int)g_quit);
    else
        ga_log("stopping as asked");
    if (attached && ga_proc_alive(inj.pid))
        ga_inject_remove(&inj);
    close(portal);
    clear_pidfile();
    ga_log_close();
    return 0;
}
