/* SPDX-License-Identifier: GPL-3.0-or-later
 * log.c - kernel log plus a file under /data, plus a small ring the portal
 * can show without reading the file back.
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/klog.h>
#else
#define klog_puts(s) fputs((s), stderr)
#endif

#define GA_DIR  "/data/gyroaim"
#define GA_LOG  GA_DIR "/log.txt"

/* Enough to explain a failed hook install without growing without bound. */
#define RING_LINES 128
#define RING_WIDTH 160

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_file;
static int g_opened;
static char g_ring[RING_LINES][RING_WIDTH];
static unsigned g_ring_head;   /* next slot to write */
static unsigned g_ring_count;

static double uptime_s(void)
{
    static struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!start.tv_sec)
        start = now;
    return (double)(now.tv_sec - start.tv_sec) +
           (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

void ga_log_open(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_opened) {
        g_opened = 1;
        uptime_s();                       /* anchor the clock */
        mkdir(GA_DIR, 0777);
        /* Truncate: a previous run's log is not worth keeping, and an
         * unbounded file on the console's flash is worse. */
        g_file = fopen(GA_LOG, "w");
        if (g_file)
            setvbuf(g_file, NULL, _IOLBF, 0);
    }
    pthread_mutex_unlock(&g_lock);
}

void ga_log(const char *fmt, ...)
{
    char body[RING_WIDTH];
    char line[RING_WIDTH + 32];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    /* One trailing newline, however the caller wrote it. */
    size_t bl = strlen(body);
    while (bl && (body[bl - 1] == '\n' || body[bl - 1] == '\r'))
        body[--bl] = '\0';

    snprintf(line, sizeof(line), "[GyroAim] %7.2f %s\n", uptime_s(), body);

    pthread_mutex_lock(&g_lock);
    klog_puts(line);
    if (g_file)
        fputs(line, g_file);
    snprintf(g_ring[g_ring_head], RING_WIDTH, "%7.2f %s", uptime_s(), body);
    g_ring_head = (g_ring_head + 1) % RING_LINES;
    if (g_ring_count < RING_LINES)
        g_ring_count++;
    pthread_mutex_unlock(&g_lock);
}

size_t ga_log_tail(char *buf, size_t n)
{
    size_t w = 0;

    if (!buf || n == 0)
        return 0;
    buf[0] = '\0';

    pthread_mutex_lock(&g_lock);
    unsigned first = (g_ring_head + RING_LINES - g_ring_count) % RING_LINES;
    for (unsigned i = 0; i < g_ring_count; i++) {
        const char *s = g_ring[(first + i) % RING_LINES];
        size_t l = strlen(s);
        if (w + l + 2 > n)
            break;
        memcpy(buf + w, s, l);
        w += l;
        buf[w++] = '\n';
    }
    buf[w] = '\0';
    pthread_mutex_unlock(&g_lock);
    return w;
}

void ga_log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
