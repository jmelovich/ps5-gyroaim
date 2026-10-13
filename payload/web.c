/* SPDX-License-Identifier: GPL-3.0-or-later
 * web.c - the phone-facing portal.
 *
 * A small HTTP server so settings can be changed while a game is running,
 * which is the only practical way to tune aiming: it has to be felt, and the
 * console's own screen is busy showing the game.
 *
 * There is no authentication. Anyone who can reach the console on the network
 * can change these settings.
 */
#include "web.h"
#include "log.h"
#include "version.h"
#include "web_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQ_MAX  2048
#define RESP_MAX 12288

/* How long one client may take over its own connection, and how many may be
 * in flight at once.
 *
 * Both exist because of a bug worth remembering: the portal served one
 * connection at a time with a blocking read and no timeout, so a client that
 * connected and sent nothing stopped the portal dead. Browsers do that
 * routinely with speculative connections, and a phone that sleeps mid-request
 * leaves one half open. Aiming carried on, because that runs from the service
 * loop, so the page looked like it had lost the service while the gyro still
 * worked. */
#define CLIENT_TIMEOUT_S 3
#define MAX_CLIENTS      6

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
            return;
        p += (size_t)w;
        n -= (size_t)w;
    }
}

static void reply(int fd, int code, const char *type, const char *body,
                  size_t len)
{
    char head[192];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, code == 200 ? "OK" : "Not Found", type, len);
    send_all(fd, head, (size_t)n);
    if (len)
        send_all(fd, body, len);
}

/* Percent-decoding is not needed: every value the page sends is a number. */
static void apply_query(ga_web_state_t *st, char *query)
{
    char *save = NULL;
    int changed = 0;

    for (char *tok = strtok_r(query, "&", &save); tok;
         tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (ga_config_set(&st->cfg, tok, eq + 1) == 0)
            changed = 1;
    }
    if (changed) {
        ga_config_clamp(&st->cfg);
        st->cfg_dirty = 1;
    }
}

static int state_json(ga_web_state_t *st, char *out, size_t n)
{
    char cfg[1536];
    int w;

    ga_config_json(&st->cfg, cfg, sizeof(cfg));

    w = snprintf(out, n, "{\"config\":%s,\"status\":\"%s\",\"title\":\"%s\","
                 "\"path\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
                 "\"kstuff\":%d,"
                 "\"attached\":%d,\"live\":",
                 cfg, st->status, st->title, st->path, GA_VERSION, GA_BUILD,
                 st->kstuff ? 1 : 0, st->attached ? 1 : 0);
    if (!st->have_live || !st->attached) {
        w += snprintf(out + w, n - (size_t)w, "null}");
        return w;
    }

    const ga_args_t *l = &st->live;
    w += snprintf(out + w, n - (size_t)w,
                  "{\"active\":%u,\"saturated\":%u,"
                  "\"rate\":[%.2f,%.2f,%.2f],"
                  "\"want\":[[%.2f,%.2f],[%.2f,%.2f]],"
                  "\"out_frac\":%.4f,"
                  "\"in\":[%u,%u,%u,%u],\"out\":[%u,%u,%u,%u],"
                  "\"l2\":%u,\"r2\":%u,\"buttons\":%u,"
                  "\"held\":[%.1f,%.1f,%.1f],"
                  "\"hold_abs\":[%u,%u,%u],"
                  "\"cal_count\":%u,\"cal_active\":%u,\"rumble_level\":%u,"
                  "\"calls\":%llu,\"applied\":%llu}}",
                  l->active, l->saturated,
                  (double)l->rate[GA_SRC_YAW], (double)l->rate[GA_SRC_PITCH],
                  (double)l->rate[GA_SRC_ROLL],
                  (double)l->want[0][0], (double)l->want[0][1],
                  (double)l->want[1][0], (double)l->want[1][1],
                  (double)l->out_frac,
                  l->in_rx, l->in_ry, l->in_lx, l->in_ly,
                  l->out_rx, l->out_ry, l->out_lx, l->out_ly,
                  l->last_l2, l->last_r2, l->last_buttons,
                  (double)l->held[GA_SRC_YAW], (double)l->held[GA_SRC_PITCH],
                  (double)l->held[GA_SRC_ROLL],
                  l->hold_abs[GA_SRC_YAW], l->hold_abs[GA_SRC_PITCH],
                  l->hold_abs[GA_SRC_ROLL],
                  l->cal_count, l->cal_was_active, l->rumble_last_level,
                  (unsigned long long)l->calls,
                  (unsigned long long)l->applied);
    return w;
}

static void serve(int fd, ga_web_state_t *st)
{
    char req[REQ_MAX];
    size_t got = 0;

    /* Headers only; nothing here reads a body. The socket carries a receive
     * timeout, so a client that stops mid-request ends here rather than
     * holding the connection. */
    while (got < sizeof(req) - 1) {
        ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0)
            break;
        got += (size_t)r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    if (!got)
        return;
    req[got] = '\0';

    if (strncmp(req, "GET ", 4) != 0) {
        reply(fd, 404, "text/plain", "", 0);
        return;
    }

    char *path = req + 4;
    char *end = strpbrk(path, " \r\n");
    if (!end)
        return;
    *end = '\0';

    char *query = strchr(path, '?');
    if (query)
        *query++ = '\0';

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        reply(fd, 200, "text/html; charset=utf-8", k_page, sizeof(k_page) - 1);
        return;
    }

    if (!strcmp(path, "/api/state") || !strcmp(path, "/api/set") ||
        !strcmp(path, "/api/save") || !strcmp(path, "/api/reload") ||
        !strcmp(path, "/api/quit") || !strcmp(path, "/api/uninstall") ||
        !strcmp(path, "/api/rest")) {
        char *body = malloc(RESP_MAX);
        if (!body) {
            reply(fd, 404, "text/plain", "", 0);
            return;
        }
        pthread_mutex_lock(&st->lock);
        if (!strcmp(path, "/api/set") && query)
            apply_query(st, query);
        else if (!strcmp(path, "/api/save"))
            st->save_as_title = 1;
        else if (!strcmp(path, "/api/reload"))
            st->reload_request = 1;
        else if (!strcmp(path, "/api/rest"))
            st->rest_request = 1;
        else if (!strcmp(path, "/api/quit"))
            st->quit_request = 1;
        else if (!strcmp(path, "/api/uninstall")) {
            /* Settings are kept unless asked for explicitly: someone who
             * removes the service to reinstall it should not lose the tuning
             * they spent an evening on. */
            st->uninstall = (query && strstr(query, "settings=1")) ? 2 : 1;
            st->quit_request = 1;
        }
        int n = state_json(st, body, RESP_MAX);
        pthread_mutex_unlock(&st->lock);
        reply(fd, 200, "application/json", body, (size_t)(n > 0 ? n : 0));
        free(body);
        return;
    }

    reply(fd, 404, "text/plain", "not found", 9);
}

/* One connection, on its own thread, so a slow client delays only itself. */
typedef struct {
    int             fd;
    ga_web_state_t *state;
} client_t;

static volatile int g_clients;

static void *client_thread(void *arg)
{
    client_t *client = arg;

    serve(client->fd, client->state);
    close(client->fd);
    __atomic_sub_fetch(&g_clients, 1, __ATOMIC_RELAXED);
    free(client);
    return NULL;
}

static void *web_thread(void *arg)
{
    ga_web_state_t *st = arg;

    for (;;) {
        int fd = accept(st->listen_fd, NULL, NULL);

        if (fd < 0) {
            /* Do not spin if the listening socket is unhappy. */
            usleep(20000);
            continue;
        }

        /* Bounded before anything is read, so even a client that is refused
         * below cannot hold the socket open indefinitely. */
        struct timeval tv;
        tv.tv_sec = CLIENT_TIMEOUT_S;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (__atomic_load_n(&g_clients, __ATOMIC_RELAXED) >= MAX_CLIENTS) {
            /* Busy. Closing is kinder than queueing behind a stall. */
            close(fd);
            continue;
        }

        client_t *client = malloc(sizeof(*client));
        if (!client) {
            close(fd);
            continue;
        }
        client->fd = fd;
        client->state = st;

        __atomic_add_fetch(&g_clients, 1, __ATOMIC_RELAXED);

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, client) != 0) {
            __atomic_sub_fetch(&g_clients, 1, __ATOMIC_RELAXED);
            close(fd);
            free(client);
            continue;
        }
        pthread_detach(thread);
    }
    return NULL;
}

int ga_web_start(int listen_fd, ga_web_state_t *state)
{
    pthread_t th;

    state->listen_fd = listen_fd;
    if (pthread_create(&th, NULL, web_thread, state) != 0) {
        ga_log("portal: could not start its thread");
        return -1;
    }
    pthread_detach(th);
    ga_log("portal: serving on port %d", GYROAIM_PORT);
    return 0;
}
