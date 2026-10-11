/* SPDX-License-Identifier: GPL-3.0-or-later
 * GyroAim - gyro aiming for any PS5 game.
 *
 * This is the resident service. It runs as an ordinary root process outside
 * any title sandbox, loaded by the console's ELF loader, and it stays alive
 * across game launches. Phase 0 does no input work at all: it proves the
 * daemon lives, that the process table is readable, and that it notices a
 * game coming and going.
 */
#include "log.h"
#include "proc.h"

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
#define POLL_US     250000
#define PIDFILE     "/data/gyroaim/pid"

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

    /* Writing to a closed portal socket must not kill the service. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/* Single instance.
 *
 * The console's ELF loader names EVERY payload "payload.elf", so matching on
 * the process name would sweep up etaHEN, the store and anything else the
 * owner has running. An early version of this file did exactly that and
 * killed two unrelated payloads. Never match payloads by name.
 *
 * The portal's listening socket is the lock instead: only our own process can
 * be holding it. The pidfile is just a note saying who to ask to leave, and we
 * only ever signal a pid we ourselves recorded, and only when the port really
 * is taken.
 */
static int bind_portal(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    /* Deliberately no SO_REUSEADDR: the bind failing is the signal we want. */
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

/* Returns the portal socket, or -1 if an older instance would not give it up. */
static int claim_single_instance(void)
{
    int fd = bind_portal();

    if (fd >= 0)
        return fd;

    pid_t older = read_pidfile();

    if (older <= 0 || older == getpid()) {
        ga_log("port %d is already in use and %s names no owner; something "
               "else on the console has the port", GYROAIM_PORT, PIDFILE);
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

static void log_environment(void)
{
#ifdef __PROSPERO__
    uint32_t fw = kernel_get_fw_version();
    /* The version is packed as hex digits: 0x13200006 is 13.20. */
    ga_log("firmware %x.%02x (0x%08x), pid %d",
           (fw >> 24) & 0xff, (fw >> 16) & 0xff, fw, (int)getpid());
#else
    ga_log("host build, pid %d", (int)getpid());
#endif
}

int main(void)
{
    pid_t game = -1;
    char title[GA_TITLE_ID_MAX] = "";

    ga_log_open();
    ga_log("GyroAim starting (portal port %d)", GYROAIM_PORT);
    log_environment();

    install_signal_handlers();

    if (ga_proc_self_check() != 0) {
        ga_log("refusing to continue: the process table is not readable, so "
               "the service could never find a game");
        ga_log_close();
        return 1;
    }

    int portal = claim_single_instance();
    if (portal < 0) {
        ga_log("refusing to continue without the portal port; another "
               "instance is probably already running");
        ga_log_close();
        return 1;
    }
    write_pidfile();

    ga_log("ready; portal port %d held, watching for a game", GYROAIM_PORT);

    while (!g_quit) {
        pid_t found[4];
        unsigned n = ga_proc_find(GAME_PROC, found, 4);

        if (n > 1) {
            /* Not expected on a console that runs one game at a time. Say so
             * rather than picking one at random. */
            static unsigned warned;
            if (!warned++)
                ga_log("%u processes named %s; waiting for one",
                       n, GAME_PROC);
        } else if (n == 1 && found[0] != game) {
            game = found[0];
            if (ga_proc_title_id(game, title) != 0)
                snprintf(title, sizeof(title), "none");
            ga_log("game started: %s, pid %d", title, (int)game);
        } else if (n == 0 && game != -1) {
            ga_log("game stopped: %s, pid %d", title, (int)game);
            game = -1;
            title[0] = '\0';
        }

        usleep(POLL_US);
    }

    ga_log("stopping on signal %d", (int)g_quit);
    close(portal);
    unlink(PIDFILE);
    ga_log_close();
    return 0;
}
