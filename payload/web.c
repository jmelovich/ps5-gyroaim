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
#define RESP_MAX 12288

static const char k_page[] =
"<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>GyroAim</title><style>"
":root{--bg:#f6f6f7;--fg:#16161a;--mut:#5c5c66;--card:#fff;--line:#e2e2e6;--acc:#2f6fed}"
"@media(prefers-color-scheme:dark){:root{--bg:#131316;--fg:#ececf0;--mut:#9a9aa6;"
"--card:#1d1d22;--line:#2e2e36;--acc:#5b8cff}}"
"*{box-sizing:border-box}body{margin:0 auto;padding:16px;background:var(--bg);"
"color:var(--fg);font:15px/1.45 system-ui,-apple-system,sans-serif;max-width:760px}"
"h1{font-size:19px;margin:0 0 2px}.sub{color:var(--mut);font-size:13px;margin-bottom:14px}"
"fieldset{border:1px solid var(--line);border-radius:10px;background:var(--card);"
"margin:0 0 12px;padding:10px 14px}legend{padding:0 6px;color:var(--mut);font-size:12px;"
"text-transform:uppercase;letter-spacing:.06em}"
"label{display:flex;align-items:center;justify-content:space-between;gap:10px;"
"padding:7px 0;border-bottom:1px solid var(--line)}label:last-child{border-bottom:0}"
"label>span:first-child{flex:1 1 auto;min-width:9em}"
"input[type=range]{flex:2 1 110px;min-width:90px;accent-color:var(--acc)}"
"select{background:var(--bg);color:var(--fg);border:1px solid var(--line);"
"border-radius:7px;padding:5px 7px;font:inherit}"
"output{font-variant-numeric:tabular-nums;color:var(--mut);min-width:3.6em;text-align:right}"
".live{display:grid;grid-template-columns:repeat(auto-fit,minmax(118px,1fr));gap:8px}"
".cell{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:7px 9px}"
".cell b{display:block;font-size:11px;color:var(--mut);text-transform:uppercase;"
"letter-spacing:.05em;font-weight:600}.cell span{font-variant-numeric:tabular-nums}"
".on{color:#1a9850;font-weight:600}.off{color:var(--mut)}.warn{color:#d9730d;font-weight:600}"
"#st{padding:8px 10px;border-radius:8px;background:var(--card);border:1px solid var(--line);"
"font-size:13px;color:var(--mut);margin-bottom:12px}"
"button{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:9px 13px;"
"font:inherit;font-weight:600}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".hint{color:var(--mut);font-size:12px;padding:6px 0 0}"
"</style>"
"<h1>GyroAim</h1><div class='sub'>Gyro aiming, tunable while the game runs.</div>"
"<div id='st'>connecting</div>"
"<fieldset><legend>Live</legend><div class='live'>"
"<div class='cell'><b>State</b><span id='l_act'>-</span></div>"
"<div class='cell'><b>Hand yaw</b><span id='l_ry'>-</span></div>"
"<div class='cell'><b>Hand pitch</b><span id='l_rp'>-</span></div>"
"<div class='cell'><b>Camera yaw</b><span id='l_cy'>-</span></div>"
"<div class='cell'><b>Camera pitch</b><span id='l_cp'>-</span></div>"
"<div class='cell'><b>Deflection</b><span id='l_df'>-</span></div>"
"<div class='cell'><b>Stick in</b><span id='l_si'>-</span></div>"
"<div class='cell'><b>Stick out</b><span id='l_so'>-</span></div>"
"<div class='cell'><b>Trigger</b><span id='l_tg'>-</span></div>"
"<div class='cell'><b>Resting offset</b><span id='l_bias'>-</span></div>"
"</div><div class='hint' id='l_cal'></div></fieldset>"
"<div id='form'></div>"
"<fieldset><legend>Profile</legend><div class='row'>"
"<button id='saveTitle'>Keep these settings for this game</button>"
"<span class='hint' id='l_path'></span></div></fieldset>"
"<script>"
"var SPEC=["
"['Aim',["
"['enabled','On','bool'],"
"['target','Controls','sel',[[0,'Right stick'],[1,'Left stick']]],"
"['sens_min','Sensitivity, slow','num',0,8,0.05],"
"['sens_max','Sensitivity, fast','num',0,8,0.05],"
"['sens_lo_rate','Slow below','num',0,200,1],"
"['sens_hi_rate','Fast above','num',0,400,1],"
"['vert_scale','Vertical scale','num',0.1,3,0.05],"
"['invert_yaw','Invert left and right','bool'],"
"['invert_pitch','Invert up and down','bool']]],"
"[\"The game's camera\",["
"['max_turn','Full-stick turn rate','num',20,1200,5],"
"['game_deadzone','Stick deadzone','num',0,0.6,0.01],"
"['game_outer','Outer deadzone','num',0,0.3,0.01],"
"['game_power','Response curve','num',0.5,4,0.05]]],"
"['When it applies',["
"['activation','Condition','sel',[[0,'Always'],[2,'Trigger held'],"
"[1,'Button held'],[3,'Never']]],"
"['trigger_right','Trigger','sel',[[0,'L2'],[1,'R2']]],"
"['trigger_threshold','Trigger threshold','num',1,255,1]]],"
"['Steadiness',["
"['tighten','Ignore below','num',0,40,0.5],"
"['smooth_rate','Smooth below','num',0,80,0.5],"
"['carry_cap','Flick carry','num',0,1.5,0.05]]],"
"['The physical stick',["
"['merge_stick','Combine with the stick','bool'],"
"['stick_deadzone','Stick counts to ignore','num',0,120,1]]],"
"['Axes',["
"['space','Gyro space','sel',[[0,'As held'],[1,'Level with the world']]],"
"['axis_yaw','Yaw from','sel',[[0,'Gyro X'],[1,'Gyro Y'],[2,'Gyro Z']]],"
"['axis_pitch','Pitch from','sel',[[0,'Gyro X'],[1,'Gyro Y'],[2,'Gyro Z']]],"
"['axis_roll','Roll from','sel',[[0,'Gyro X'],[1,'Gyro Y'],[2,'Gyro Z']]]]],"
"['Drift',[['calibrate','Learn the resting offset','bool']]],"
"['Rumble',["
"['rumble','Rumble while turning','bool'],"
"['rumble_scale','Rumble strength','num',0,1.5,0.05],"
"['rumble_period_ms','Rumble interval','num',16,400,4]]]"
"];"
"var K=[],busy=0,pending=0;"
"function el(i){return document.getElementById(i)}"
"function build(){var h='';SPEC.forEach(function(g){"
"h+='<fieldset><legend>'+g[0]+'</legend>';"
"g[1].forEach(function(f){var k=f[0];K.push(k);"
"h+=\"<label><span>\"+f[1]+'</span>';"
"if(f[2]==='bool')h+=\"<input type=checkbox id='\"+k+\"'>\";"
"else if(f[2]==='sel'){h+=\"<select id='\"+k+\"'>\";"
"f[3].forEach(function(o){h+=\"<option value='\"+o[0]+\"'>\"+o[1]+'</option>'});"
"h+='</select>'}"
"else h+=\"<input type=range id='\"+k+\"' min='\"+f[3]+\"' max='\"+f[4]+"
"\"' step='\"+f[5]+\"'><output id='o_\"+k+\"'></output>\";"
"h+='</label>'});h+='</fieldset>'});"
"el('form').innerHTML=h;"
"K.forEach(function(k){var e=el(k);"
"e.addEventListener('input',function(){var o=el('o_'+k);"
"if(o)o.textContent=e.value;put()});e.addEventListener('change',put)})}"
"function put(){if(busy){pending=1;return}busy=1;"
"var q=K.map(function(k){var e=el(k);"
"return k+'='+(e.type==='checkbox'?(e.checked?1:0):e.value)}).join('&');"
"fetch('/api/set?'+q).then(function(r){return r.json()}).then(function(j){apply(j.config)})"
".catch(function(){}).then(function(){busy=0;if(pending){pending=0;put()}})}"
"function apply(c){K.forEach(function(k){var e=el(k);if(document.activeElement===e)return;"
"if(e.type==='checkbox')e.checked=!!c[k];else e.value=c[k];"
"var o=el('o_'+k);if(o)o.textContent=e.value})}"
"function sgn(v,d){return (v<0?'':'+')+v.toFixed(d===undefined?1:d)}"
"function tick(){fetch('/api/state').then(function(r){return r.json()}).then(function(j){"
"apply(j.config);el('st').textContent=j.status;"
"el('l_path').textContent='saved in '+j.path;"
"var c=j.config;"
"el('l_bias').textContent=sgn(c.bias_y*57.3,2)+' deg/s';"
"var L=j.live;"
"if(!L){['l_act','l_ry','l_rp','l_cy','l_cp','l_df','l_si','l_so','l_tg']"
".forEach(function(i){el(i).textContent='-'});el('l_cal').textContent='';return}"
"var a=el('l_act');a.textContent=L.saturated?'at the limit':(L.active?'aiming':'idle');"
"a.className=L.saturated?'warn':(L.active?'on':'off');"
"el('l_ry').textContent=sgn(L.rate_yaw)+' deg/s';"
"el('l_rp').textContent=sgn(L.rate_pitch)+' deg/s';"
"el('l_cy').textContent=sgn(L.cam_yaw)+' deg/s';"
"el('l_cp').textContent=sgn(L.cam_pitch)+' deg/s';"
"el('l_df').textContent=(L.out_frac*100).toFixed(1)+'%';"
"el('l_si').textContent=L.in_x+', '+L.in_y;"
"el('l_so').textContent=L.out_x+', '+L.out_y;"
"el('l_tg').textContent='L2 '+L.l2+'  R2 '+L.r2;"
"el('l_cal').textContent=L.cal_active?"
"'Stillness window unusable: aiming was active.':"
"('Stillness window: '+L.cal_count+' samples.');"
"}).catch(function(){el('st').textContent='no connection to the service'})}"
"build();el('saveTitle').addEventListener('click',function(){"
"fetch('/api/save?scope=title').then(tick)});"
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
                  "\"cal_count\":%u,\"cal_active\":%u,"
                  "\"calls\":%llu,\"applied\":%llu}}",
                  l->active, l->saturated,
                  (double)l->rate_yaw, (double)l->rate_pitch,
                  (double)l->cam_yaw, (double)l->cam_pitch,
                  (double)l->out_frac,
                  l->in_rx, l->in_ry, l->out_rx, l->out_ry,
                  l->last_l2, l->last_r2, l->last_buttons,
                  l->cal_count, l->cal_was_active,
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
        !strcmp(path, "/api/save")) {
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
