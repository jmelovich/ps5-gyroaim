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
#include "web_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQ_MAX  2048
#define RESP_MAX 12288

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
                 "\"path\":\"%s\",\"live\":", cfg, st->status, st->title,
                 st->path);
    if (!st->have_live || !st->attached) {
        w += snprintf(out + w, n - (size_t)w, "null}");
        return w;
    }

    const ga_args_t *l = &st->live;
    w += snprintf(out + w, n - (size_t)w,
                  "{\"active\":%u,\"saturated\":%u,"
                  "\"rate_yaw\":%.2f,\"rate_pitch\":%.2f,"
                  "\"cam_yaw\":%.2f,\"cam_pitch\":%.2f,\"out_frac\":%.4f,"
                  "\"in_x\":%u,\"in_y\":%u,\"out_x\":%u,\"out_y\":%u,"
                  "\"l2\":%u,\"r2\":%u,\"buttons\":%u,"
                  "\"cal_count\":%u,\"cal_active\":%u,\"rumble_level\":%u,"
                  "\"calls\":%llu,\"applied\":%llu}}",
                  l->active, l->saturated,
                  (double)l->rate_yaw, (double)l->rate_pitch,
                  (double)l->cam_yaw, (double)l->cam_pitch,
                  (double)l->out_frac,
                  l->in_rx, l->in_ry, l->out_rx, l->out_ry,
                  l->last_l2, l->last_r2, l->last_buttons,
                  l->cal_count, l->cal_was_active, l->rumble_last_level,
                  (unsigned long long)l->calls,
                  (unsigned long long)l->applied);
    return w;
}

static void serve(int fd, ga_web_state_t *st)
{
    char req[REQ_MAX];
    size_t got = 0;

    /* Headers only; nothing here reads a body. */
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
        !strcmp(path, "/api/save") || !strcmp(path, "/api/reload")) {
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
        int n = state_json(st, body, RESP_MAX);
        pthread_mutex_unlock(&st->lock);
        reply(fd, 200, "application/json", body, (size_t)(n > 0 ? n : 0));
        free(body);
        return;
    }

    reply(fd, 404, "text/plain", "not found", 9);
}

static void *web_thread(void *arg)
{
    ga_web_state_t *st = arg;

    for (;;) {
        int fd = accept(st->listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        serve(fd, st);
        close(fd);
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
