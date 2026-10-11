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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQ_MAX  2048
#define RESP_MAX 8192

static const char k_page[] =
"<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>GyroAim</title><style>"
":root{--bg:#f6f6f7;--fg:#16161a;--mut:#5c5c66;--card:#fff;--line:#e2e2e6;--acc:#2f6fed}"
"@media(prefers-color-scheme:dark){:root{--bg:#131316;--fg:#ececf0;--mut:#9a9aa6;"
"--card:#1d1d22;--line:#2e2e36;--acc:#5b8cff}}"
"*{box-sizing:border-box}body{margin:0;padding:16px;background:var(--bg);color:var(--fg);"
"font:15px/1.45 system-ui,-apple-system,sans-serif;max-width:720px;margin:0 auto}"
"h1{font-size:19px;margin:0 0 4px}.sub{color:var(--mut);font-size:13px;margin-bottom:16px}"
"fieldset{border:1px solid var(--line);border-radius:10px;background:var(--card);"
"margin:0 0 14px;padding:12px 14px}legend{padding:0 6px;color:var(--mut);font-size:12px;"
"text-transform:uppercase;letter-spacing:.06em}"
"label{display:flex;align-items:center;justify-content:space-between;gap:12px;"
"padding:7px 0;border-bottom:1px solid var(--line)}label:last-child{border-bottom:0}"
"label span{flex:0 0 auto}input[type=range]{flex:1;min-width:90px;accent-color:var(--acc)}"
"select,input[type=number]{background:var(--bg);color:var(--fg);border:1px solid var(--line);"
"border-radius:7px;padding:5px 7px;font:inherit}"
"output{font-variant-numeric:tabular-nums;color:var(--mut);min-width:3.5em;text-align:right}"
".live{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}"
".cell{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px 10px}"
".cell b{display:block;font-size:11px;color:var(--mut);text-transform:uppercase;"
"letter-spacing:.05em;font-weight:600}.cell span{font-variant-numeric:tabular-nums;font-size:15px}"
".on{color:#1a9850;font-weight:600}.off{color:var(--mut)}"
"#st{padding:8px 10px;border-radius:8px;background:var(--card);border:1px solid var(--line);"
"font-size:13px;color:var(--mut);margin-bottom:14px}"
"</style>"
"<h1>GyroAim</h1><div class='sub'>Gyro aiming, tunable while the game runs.</div>"
"<div id='st'>connecting</div>"
"<fieldset><legend>Live</legend><div class='live'>"
"<div class='cell'><b>State</b><span id='act'>-</span></div>"
"<div class='cell'><b>Yaw rate</b><span id='ry'>-</span></div>"
"<div class='cell'><b>Pitch rate</b><span id='rp'>-</span></div>"
"<div class='cell'><b>Stick in</b><span id='si'>-</span></div>"
"<div class='cell'><b>Stick out</b><span id='so'>-</span></div>"
"<div class='cell'><b>Trigger</b><span id='tg'>-</span></div>"
"</div></fieldset>"
"<fieldset><legend>Aim</legend>"
"<label><span>On</span><input type=checkbox id=enabled></label>"
"<label><span>Sensitivity, yaw</span><input type=range id=sens_yaw min=0 max=200 step=1>"
"<output id=o_sens_yaw></output></label>"
"<label><span>Sensitivity, pitch</span><input type=range id=sens_pitch min=0 max=200 step=1>"
"<output id=o_sens_pitch></output></label>"
"<label><span>Invert yaw</span><input type=checkbox id=invert_yaw></label>"
"<label><span>Invert pitch</span><input type=checkbox id=invert_pitch></label>"
"<label><span>Controls</span><select id=target>"
"<option value=0>Right stick</option><option value=1>Left stick</option></select></label>"
"<label><span>Stick deadzone</span><input type=range id=stick_deadzone min=0 max=120 step=1>"
"<output id=o_stick_deadzone></output></label>"
"</fieldset>"
"<fieldset><legend>When it applies</legend>"
"<label><span>Condition</span><select id=activation>"
"<option value=0>Always</option><option value=2>Trigger held</option>"
"<option value=1>Button held</option><option value=3>Never</option></select></label>"
"<label><span>Trigger</span><select id=trigger_right>"
"<option value=0>L2</option><option value=1>R2</option></select></label>"
"<label><span>Trigger threshold</span><input type=range id=trigger_threshold min=1 max=255 step=1>"
"<output id=o_trigger_threshold></output></label>"
"</fieldset>"
"<fieldset><legend>Axes</legend>"
"<label><span>Yaw comes from</span><select id=axis_yaw>"
"<option value=0>Gyro X</option><option value=1>Gyro Y</option>"
"<option value=2>Gyro Z</option></select></label>"
"<label><span>Pitch comes from</span><select id=axis_pitch>"
"<option value=0>Gyro X</option><option value=1>Gyro Y</option>"
"<option value=2>Gyro Z</option></select></label>"
"</fieldset>"
"<script>"
"var K=['enabled','sens_yaw','sens_pitch','invert_yaw','invert_pitch','target',"
"'stick_deadzone','activation','trigger_right','trigger_threshold','axis_yaw','axis_pitch'];"
"var busy=0,pending=0;"
"function el(i){return document.getElementById(i)}"
"function put(){if(busy){pending=1;return}busy=1;"
"var q=K.map(function(k){var e=el(k);"
"return k+'='+(e.type==='checkbox'?(e.checked?1:0):e.value)}).join('&');"
"fetch('/api/set?'+q).then(function(r){return r.json()}).then(function(j){apply(j.config)})"
".catch(function(){}).then(function(){busy=0;if(pending){pending=0;put()}})}"
"function apply(c){K.forEach(function(k){var e=el(k);if(document.activeElement===e)return;"
"if(e.type==='checkbox')e.checked=!!c[k];else e.value=c[k];"
"var o=el('o_'+k);if(o)o.textContent=e.value})}"
"function fmt(v){return (v<0?'':'+')+v.toFixed(2)}"
"function tick(){fetch('/api/state').then(function(r){return r.json()}).then(function(j){"
"apply(j.config);var L=j.live;"
"el('st').textContent=j.status;"
"if(!L){['act','ry','rp','si','so','tg'].forEach(function(i){el(i).textContent='-'});return}"
"var a=el('act');a.textContent=L.active?'aiming':'idle';a.className=L.active?'on':'off';"
"el('ry').textContent=fmt(L.yaw)+' rad/s';el('rp').textContent=fmt(L.pitch)+' rad/s';"
"el('si').textContent=L.in_x+', '+L.in_y;el('so').textContent=L.out_x+', '+L.out_y;"
"el('tg').textContent='L2 '+L.l2+'  R2 '+L.r2;"
"}).catch(function(){el('st').textContent='no connection to the service'})}"
"K.forEach(function(k){var e=el(k);e.addEventListener('input',function(){"
"var o=el('o_'+k);if(o)o.textContent=e.value;put()});e.addEventListener('change',put)});"
"tick();setInterval(tick,250);"
"</script>";

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
    char cfg[512];
    int w;

    ga_config_json(&st->cfg, cfg, sizeof(cfg));

    w = snprintf(out, n, "{\"config\":%s,\"status\":\"%s\",\"live\":", cfg,
                 st->status);
    if (!st->have_live || !st->attached) {
        w += snprintf(out + w, n - (size_t)w, "null}");
        return w;
    }

    const ga_args_t *l = &st->live;
    w += snprintf(out + w, n - (size_t)w,
                  "{\"active\":%u,\"yaw\":%.4f,\"pitch\":%.4f,"
                  "\"in_x\":%u,\"in_y\":%u,\"out_x\":%u,\"out_y\":%u,"
                  "\"l2\":%u,\"r2\":%u,\"buttons\":%u,"
                  "\"calls\":%llu,\"applied\":%llu}}",
                  l->active, (double)l->used_yaw, (double)l->used_pitch,
                  l->in_rx, l->in_ry, l->out_rx, l->out_ry,
                  l->last_l2, l->last_r2, l->last_buttons,
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

    if (!strcmp(path, "/api/state") || !strcmp(path, "/api/set")) {
        char *body = malloc(RESP_MAX);
        if (!body) {
            reply(fd, 404, "text/plain", "", 0);
            return;
        }
        pthread_mutex_lock(&st->lock);
        if (!strcmp(path, "/api/set") && query)
            apply_query(st, query);
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
