/* SPDX-License-Identifier: GPL-3.0-or-later
 * GyroAim control panel.
 *
 * A title cannot do the service's job: launching a game tears a title down,
 * and a title lives in a sandbox. So this is a control panel, not the thing
 * being controlled. It installs and starts the service, says whether it is
 * running, points at the phone-facing settings page, and stops it again.
 *
 * Anything that blocks runs on a worker thread. The draw callback is called
 * once per frame and only reads the latest state, because a frame spent
 * waiting on a socket is a frame the console does not get.
 *
 * The bundled font is uppercase, digits, and a handful of marks, so every
 * string here is written to suit it.
 */
#include "service.hpp"
#include "vendor/demo_renderer.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
/* Nothing on the console reads a title's stderr, so progress goes to the
 * kernel log, which can be read from another machine. Without it a panel that
 * misbehaves is just a wrong picture on a television. */
int sceKernelDebugOutText(int channel, const char *text);

int32_t scePadInit();
int32_t scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int32_t scePadReadState(int32_t handle, void *data);
int32_t sceUserServiceInitialize(const void *params);
int32_t sceUserServiceGetLoginUserIdList(int32_t list[4]);
}

namespace
{
using ps5::demo::Canvas;
using ps5::demo::Color;

constexpr Color rgb(unsigned r, unsigned g, unsigned b) noexcept
{
    /* The framebuffer is little-endian RGBA, so the bytes run R, G, B, A. */
    return static_cast<Color>(0xff000000u | (b << 16) | (g << 8) | r);
}

void panel_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void panel_log(const char *fmt, ...)
{
    char line[256];
    va_list args;

    va_start(args, fmt);
    int n = std::vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "\n");
    sceKernelDebugOutText(0, line);
}

const Color ink = rgb(236, 236, 241);
const Color dim = rgb(140, 140, 152);
const Color accent = rgb(90, 140, 255);
const Color good = rgb(86, 194, 113);
const Color bad = rgb(232, 145, 60);
const Color panel = rgb(27, 27, 33);
const Color back = rgb(16, 17, 22);

/* ---- pad ---------------------------------------------------------------- */

constexpr unsigned pad_sample_size = 120;
constexpr uint32_t button_up = 0x10;
constexpr uint32_t button_right = 0x20;
constexpr uint32_t button_down = 0x40;
constexpr uint32_t button_left = 0x80;
constexpr uint32_t button_cross = 0x4000;
constexpr uint32_t button_circle = 0x2000;

int32_t pad_handle = -1;

void pad_open()
{
    int32_t users[4] = { -1, -1, -1, -1 };

    sceUserServiceInitialize(nullptr);
    scePadInit();
    if (sceUserServiceGetLoginUserIdList(users) != 0)
        return;
    for (int i = 0; i < 4 && pad_handle < 0; i++) {
        if (users[i] < 0)
            continue;
        int32_t h = scePadOpen(users[i], 0, 0, nullptr);
        if (h >= 0)
            pad_handle = h;
    }
    panel_log("[GAPANEL] pad handle %d", pad_handle);
}

/* Buttons that went down since the last look. */
uint32_t pad_pressed()
{
    static uint32_t previous = 0;
    unsigned char sample[pad_sample_size];

    if (pad_handle < 0)
        return 0;
    std::memset(sample, 0, sizeof(sample));
    if (scePadReadState(pad_handle, sample) != 0)
        return 0;

    uint32_t buttons;
    std::memcpy(&buttons, sample, sizeof(buttons));
    uint32_t edge = buttons & ~previous;
    previous = buttons;
    return edge;
}

/* ---- shared state ------------------------------------------------------- */

enum Action {
    action_none = 0,
    action_start,
    action_stop,
    action_autostart_on,
    action_autostart_off,
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
gyroaim::Status status;
char address[32] = "";
bool autostart = false;
char message[96] = "LOOKING FOR THE SERVICE";
std::atomic<int> queued{ action_none };
std::atomic<bool> working{ false };

void set_message(const char *text)
{
    pthread_mutex_lock(&lock);
    std::snprintf(message, sizeof(message), "%s", text);
    pthread_mutex_unlock(&lock);
}

/* Uppercase in place, since the font has no lower case. */
void shout(char *text)
{
    for (char *at = text; *at; at++)
        if (*at >= 'a' && *at <= 'z')
            *at = static_cast<char>(*at - 'a' + 'A');
}

#if GA_PANEL_SELFTEST
/* Exercises everything the panel does that could fail inside the sandbox:
 * writing where pldmgr looks, editing its list, and handing the payload to
 * the loader. Buttons cannot be pressed from a development machine, so this
 * is how those paths get covered. */
void selftest()
{
    char why[96];
    struct stat st;

    panel_log("[GATEST] /data visible: %d", ::stat("/data", &st) == 0);
    panel_log("[GATEST] bundled service readable: %d",
              ::stat(gyroaim::bundled_service, &st) == 0);

    bool on = gyroaim::enable_autostart(why, sizeof(why));
    panel_log("[GATEST] enable autostart: %d (%s)", on, why);
    panel_log("[GATEST] payload in place: %d, listed: %d",
              ::stat(gyroaim::autostart_elf, &st) == 0,
              gyroaim::autostart_enabled());

    bool off = gyroaim::disable_autostart(why, sizeof(why));
    panel_log("[GATEST] disable autostart: %d (%s)", off, why);
    panel_log("[GATEST] still listed: %d", gyroaim::autostart_enabled());

    bool sent = gyroaim::start_service(why, sizeof(why));
    panel_log("[GATEST] hand payload to the loader: %d (%s)", sent, why);
    for (int i = 0; i < 20; i++) {
        usleep(500000);
        gyroaim::Status s;
        if (gyroaim::query(s)) {
            panel_log("[GATEST] service answered again after %d ms, "
                      "version %s", (i + 1) * 500, s.version);
            return;
        }
    }
    panel_log("[GATEST] service did not come back");
}
#endif

void *worker(void *)
{
    char why[96];

    panel_log("[GAPANEL] worker started");
    bool have_address = gyroaim::local_address(address, sizeof(address));
    panel_log("[GAPANEL] address %s", have_address ? address : "unknown");

    bool said = false;
    for (;;) {
        int todo = queued.exchange(action_none);

        if (todo != action_none) {
            working = true;
            switch (todo) {
            case action_start:
                set_message("STARTING THE SERVICE");
                if (gyroaim::start_service(why, sizeof(why))) {
                    /* The loader needs a moment, and a service already
                     * running needs to hand over the port. */
                    usleep(1500000);
                    std::snprintf(why, sizeof(why), "SERVICE STARTED");
                }
                shout(why);
                set_message(why);
                break;
            case action_stop:
                set_message("STOPPING THE SERVICE");
                gyroaim::stop_service();
                usleep(800000);
                set_message("SERVICE STOPPED");
                break;
            case action_autostart_on:
                gyroaim::enable_autostart(why, sizeof(why));
                shout(why);
                set_message(why);
                break;
            case action_autostart_off:
                gyroaim::disable_autostart(why, sizeof(why));
                shout(why);
                set_message(why);
                break;
            default:
                break;
            }
            working = false;
        }

        gyroaim::Status fresh;
        bool ok = gyroaim::query(fresh);
        if (!said)
            panel_log("[GAPANEL] first status poll done, reachable %d", ok);
        bool auto_on = gyroaim::autostart_enabled();
        if (!said)
            panel_log("[GAPANEL] autostart check done, %d", auto_on);

        if (!said || todo != action_none) {
            panel_log("[GAPANEL] service %s, kstuff %d, attached %d, "
                      "version %s, game %s, autostart %d",
                      ok ? "up" : "down", fresh.kstuff, fresh.attached,
                      fresh.version[0] ? fresh.version : "-",
                      fresh.game[0] ? fresh.game : "-", auto_on);
            said = true;
#if GA_PANEL_SELFTEST
            selftest();
#endif
        }

        pthread_mutex_lock(&lock);
        status = fresh;
        status.reachable = ok;
        autostart = auto_on;
        shout(status.version);
        shout(status.game);
        shout(status.text);
        pthread_mutex_unlock(&lock);

        usleep(500000);
    }
    return nullptr;
}

/* ---- the menu ----------------------------------------------------------- */

constexpr int menu_items = 4;
int selected = 0;

void activate(int item)
{
    if (working)
        return;

    switch (item) {
    case 0:
        queued = action_start;
        break;
    case 1:
        queued = action_stop;
        break;
    case 2:
        queued = autostart ? action_autostart_off : action_autostart_on;
        break;
    default:
        break;
    }
}

void handle_input()
{
    uint32_t pressed = pad_pressed();

    if (pressed & (button_down | button_right))
        selected = (selected + 1) % menu_items;
    if (pressed & (button_up | button_left))
        selected = (selected + menu_items - 1) % menu_items;
    if (pressed & button_cross)
        activate(selected);
    if (pressed & button_circle)
        queued = action_none;
}

/* ---- drawing ------------------------------------------------------------ */

void label(Canvas &canvas, unsigned x, unsigned y, const char *text,
           unsigned scale, Color color)
{
    canvas.text(x, y, text, scale, color);
}

void draw(Canvas &canvas) noexcept
{
    gyroaim::Status now;
    char note[96];
    char addr[32];
    bool auto_on;

    pthread_mutex_lock(&lock);
    now = status;
    std::snprintf(note, sizeof(note), "%s", message);
    std::snprintf(addr, sizeof(addr), "%s", address);
    auto_on = autostart;
    pthread_mutex_unlock(&lock);

    handle_input();

    canvas.clear(back);
    canvas.rectangle(0, 0, 1920, 8, accent);

    label(canvas, 96, 72, "GYROAIM", 7, ink);
    label(canvas, 96, 150, "CONTROL PANEL", 2, dim);

    /* ---- what the console is doing ---- */
    canvas.rectangle(96, 210, 1728, 220, panel);

    char line[96];
    unsigned y = 242;

    if (now.reachable)
        std::snprintf(line, sizeof(line), "SERVICE: RUNNING   VERSION %s",
                      now.version);
    else
        std::snprintf(line, sizeof(line), "SERVICE: NOT RUNNING");
    label(canvas, 128, y, line, 3, now.reachable ? good : bad);
    y += 44;

    if (!now.reachable)
        std::snprintf(line, sizeof(line),
                      "PRESS START THE SERVICE BELOW TO BEGIN");
    else if (!now.kstuff)
        std::snprintf(line, sizeof(line),
                      "KSTUFF IS NOT LOADED - AIMING CANNOT BE INSTALLED");
    else if (now.attached)
        std::snprintf(line, sizeof(line), "AIMING IS LIVE IN %s",
                      now.game[0] ? now.game : "A GAME");
    else
        std::snprintf(line, sizeof(line), "READY - WAITING FOR A GAME");
    label(canvas, 128, y, line, 2,
          (now.reachable && !now.kstuff) ? bad : dim);
    y += 38;

    if (addr[0] && now.reachable)
        std::snprintf(line, sizeof(line), "SETTINGS: HTTP://%s:%d", addr,
                      gyroaim::portal_port);
    else if (now.reachable)
        std::snprintf(line, sizeof(line),
                      "SETTINGS: PORT %d ON THIS CONSOLE", gyroaim::portal_port);
    else
        line[0] = '\0';
    label(canvas, 128, y, line, 2, ink);
    y += 38;

    std::snprintf(line, sizeof(line), "STARTS WITH THE CONSOLE: %s",
                  auto_on ? "YES" : "NO");
    label(canvas, 128, y, line, 2, dim);

    /* ---- the menu ---- */
    static const char *const names[menu_items] = {
        "START OR RESTART THE SERVICE",
        "STOP THE SERVICE",
        "START WITH THE CONSOLE",
        "SETTINGS ON YOUR PHONE",
    };

    unsigned row = 496;
    for (int i = 0; i < menu_items; i++) {
        bool on = i == selected;
        if (on) {
            canvas.rectangle(96, row - 14, 1728, 68, panel);
            canvas.rectangle(96, row - 14, 8, 68, accent);
        }
        char text[96];
        if (i == 2)
            std::snprintf(text, sizeof(text), "%s: %s", names[i],
                          auto_on ? "ON" : "OFF");
        else
            std::snprintf(text, sizeof(text), "%s", names[i]);
        label(canvas, 136, row, text, 3, on ? ink : dim);
        row += 88;
    }

    /* ---- the message and the hint ---- */
    label(canvas, 96, 904, note, 2, working ? bad : dim);
    label(canvas, 96, 980, "CROSS: CHOOSE    UP AND DOWN: MOVE", 2, dim);
    label(canvas, 96, 1018,
          "CLOSING THIS APP DOES NOT STOP THE SERVICE", 2, dim);
}
} // namespace

int main()
{
    pthread_t thread;

    panel_log("[GAPANEL] control panel starting");
    pad_open();
    if (pthread_create(&thread, nullptr, worker, nullptr) == 0)
        pthread_detach(thread);

    ps5::demo::run_frames(draw, "GYROAIM CONTROL PANEL");
}
