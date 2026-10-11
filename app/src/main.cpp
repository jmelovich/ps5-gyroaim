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

extern "C" {
#include "vendor/qrcodegen.h"
}

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
    action_remove,
    action_remove_all,
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

void qr_prepare(const char *text);   /* defined with the code screen below */

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

    char url[64];
    if (address[0]) {
        std::snprintf(url, sizeof(url), "HTTP://%s:%d", address,
                      gyroaim::portal_port);
        qr_prepare(url);
    } else {
        panel_log("[GATEST] no address, so no settings code to build");
    }

    bool sent = gyroaim::start_service(why, sizeof(why));
    panel_log("[GATEST] hand payload to the loader: %d (%s)", sent, why);
    bool back = false;
    for (int i = 0; i < 20 && !back; i++) {
        usleep(500000);
        gyroaim::Status s;
        if (gyroaim::query(s)) {
            panel_log("[GATEST] service answered again after %d ms, "
                      "version %s", (i + 1) * 500, s.version);
            back = true;
        }
    }
    if (!back)
        panel_log("[GATEST] service did not come back");

#if GA_PANEL_SELFTEST >= 2
    /* Removal, settings and all. Destructive, so it is behind a second level
     * of the flag and never part of the ordinary self test. */
    panel_log("[GATEST] removing the service, settings included");
    bool gone = gyroaim::remove_service(true, why, sizeof(why));
    panel_log("[GATEST] remove: %d (%s)", gone, why);

    gyroaim::Status after{};
    panel_log("[GATEST] service still answering: %d",
              gyroaim::query(after) ? 1 : 0);
    panel_log("[GATEST] still set to start with the console: %d",
              gyroaim::autostart_enabled() ? 1 : 0);
    panel_log("[GATEST] startup copy still there: %d",
              ::stat(gyroaim::autostart_elf, &st) == 0 ? 1 : 0);
    panel_log("[GATEST] settings directory still there: %d",
              ::stat("/data/gyroaim", &st) == 0 ? 1 : 0);
    panel_log("[GATEST] the app still carries its own copy: %d",
              ::stat(gyroaim::bundled_service, &st) == 0 ? 1 : 0);

    /* Second phase: install again, then remove the other way, which has to
     * leave the tuning behind for someone who means to reinstall. */
    panel_log("[GATEST] installing again to test keeping settings");
    gyroaim::start_service(why, sizeof(why));
    bool up = false;
    for (int i = 0; i < 20 && !up; i++) {
        usleep(500000);
        gyroaim::Status s;
        up = gyroaim::query(s);
    }
    panel_log("[GATEST] service back up: %d", up);
    panel_log("[GATEST] settings file recreated: %d",
              ::stat("/data/gyroaim/settings.txt", &st) == 0 ? 1 : 0);
    gyroaim::enable_autostart(why, sizeof(why));

    panel_log("[GATEST] removing the service, keeping settings");
    gone = gyroaim::remove_service(false, why, sizeof(why));
    panel_log("[GATEST] remove: %d (%s)", gone, why);
    panel_log("[GATEST] service still answering: %d",
              gyroaim::query(after) ? 1 : 0);
    panel_log("[GATEST] startup copy still there: %d",
              ::stat(gyroaim::autostart_elf, &st) == 0 ? 1 : 0);
    panel_log("[GATEST] settings KEPT: %d",
              ::stat("/data/gyroaim/settings.txt", &st) == 0 ? 1 : 0);
#endif
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
            case action_remove:
            case action_remove_all:
                set_message("REMOVING THE SERVICE");
                gyroaim::remove_service(todo == action_remove_all, why,
                                        sizeof(why));
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

/* ---- the settings code -------------------------------------------------- *
 *
 * The panel cannot show the settings while a game is running, because a title
 * is torn down when a game launches. A phone can, so the panel's job is to
 * get the address onto the phone without anyone typing it.
 *
 * The code is encoded once and kept, rather than rebuilt every frame: the address
 * only changes when the network does.
 */
char qr_text[64] = "";
uint8_t qr_code[qrcodegen_BUFFER_LEN_MAX];
bool qr_valid = false;

void qr_prepare(const char *text)
{
    static uint8_t scratch[qrcodegen_BUFFER_LEN_MAX];

    if (!std::strcmp(qr_text, text))
        return;
    std::snprintf(qr_text, sizeof(qr_text), "%s", text);
    qr_valid = qrcodegen_encodeText(qr_text, scratch, qr_code,
                                    qrcodegen_Ecc_MEDIUM,
                                    qrcodegen_VERSION_MIN,
                                    qrcodegen_VERSION_MAX,
                                    qrcodegen_Mask_AUTO, true);
    panel_log("[GAPANEL] settings code for %s: %s", qr_text,
              qr_valid ? "ready" : "failed");
}

void qr_draw(Canvas &canvas, unsigned left, unsigned top, unsigned target)
{
    if (!qr_valid)
        return;

    const unsigned quiet = 4;
    const unsigned modules = static_cast<unsigned>(qrcodegen_getSize(qr_code));
    const unsigned across = modules + quiet * 2;
    unsigned scale = target / across;

    if (scale < 1)
        scale = 1;
    const unsigned side = across * scale;

    /* Quiet zone included: a code without one often will not scan. */
    canvas.rectangle(left, top, side, side, rgb(255, 255, 255));
    for (unsigned y = 0; y < modules; y++)
        for (unsigned x = 0; x < modules; x++)
            if (qrcodegen_getModule(qr_code, static_cast<int>(x),
                                    static_cast<int>(y)))
                canvas.rectangle(left + (quiet + x) * scale,
                                 top + (quiet + y) * scale, scale, scale,
                                 rgb(0, 0, 0));
}

/* ---- the menu ----------------------------------------------------------- */

constexpr int menu_items = 5;
int selected = 0;
bool showing_code = false;

/* The remove screen. Destructive, so it sits behind a question with cancel
 * as the resting choice. */
bool asking_remove = false;
int remove_choice = 2;
constexpr int remove_choices = 3;

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
    case 3:
        showing_code = true;
        break;
    case 4:
        asking_remove = true;
        remove_choice = 2;      /* cancel, until someone moves off it */
        break;
    default:
        break;
    }
}

void handle_input()
{
    uint32_t pressed = pad_pressed();

    if (showing_code) {
        if (pressed & (button_circle | button_cross))
            showing_code = false;
        return;
    }

    if (asking_remove) {
        if (pressed & button_down)
            remove_choice = (remove_choice + 1) % remove_choices;
        if (pressed & button_up)
            remove_choice = (remove_choice + remove_choices - 1) %
                            remove_choices;
        if (pressed & button_circle)
            asking_remove = false;
        if (pressed & button_cross) {
            if (remove_choice == 0)
                queued = action_remove;
            else if (remove_choice == 1)
                queued = action_remove_all;
            asking_remove = false;
        }
        return;
    }

    if (pressed & (button_down | button_right))
        selected = (selected + 1) % menu_items;
    if (pressed & (button_up | button_left))
        selected = (selected + menu_items - 1) % menu_items;
    if (pressed & button_cross)
        activate(selected);
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

    char url[64];
    if (addr[0])
        std::snprintf(url, sizeof(url), "HTTP://%s:%d", addr,
                      gyroaim::portal_port);
    else
        url[0] = '\0';

    if (showing_code) {
        label(canvas, 96, 72, "SETTINGS ON YOUR PHONE", 4, ink);
        if (url[0] && now.reachable) {
            qr_prepare(url);
            qr_draw(canvas, 96, 190, 620);
            label(canvas, 790, 260, "SCAN THIS WITH YOUR PHONE", 3, ink);
            label(canvas, 790, 320, "OR OPEN", 2, dim);
            label(canvas, 790, 366, url, 3, accent);
            label(canvas, 790, 440, "THE PHONE MUST BE ON THE SAME", 2, dim);
            label(canvas, 790, 478, "NETWORK AS THIS CONSOLE.", 2, dim);
            label(canvas, 790, 540, "SETTINGS CHANGE WHILE YOU PLAY.", 2, dim);
            label(canvas, 790, 578, "THERE IS NO PASSWORD: ANYONE ON", 2, dim);
            label(canvas, 790, 616, "YOUR NETWORK CAN OPEN IT.", 2, dim);
        } else if (!now.reachable) {
            label(canvas, 96, 240, "THE SERVICE IS NOT RUNNING.", 3, bad);
            label(canvas, 96, 300, "START IT FIRST.", 2, dim);
        } else {
            label(canvas, 96, 240, "THIS CONSOLE HAS NO NETWORK ADDRESS.", 3,
                  bad);
            label(canvas, 96, 300, "CONNECT IT TO YOUR NETWORK.", 2, dim);
        }
        label(canvas, 96, 980, "CIRCLE: BACK", 2, dim);
        return;
    }

    if (asking_remove) {
        label(canvas, 96, 72, "REMOVE THE SERVICE", 4, ink);
        label(canvas, 96, 170, "THIS STOPS IT AND TAKES IT OFF THE CONSOLE,", 2,
              dim);
        label(canvas, 96, 208, "INCLUDING STARTING WITH THE CONSOLE.", 2, dim);
        label(canvas, 96, 266, "THIS APP KEEPS ITS OWN COPY, SO YOU CAN", 2,
              dim);
        label(canvas, 96, 304, "ALWAYS START IT AGAIN FROM HERE.", 2, dim);

        static const char *const choices[remove_choices] = {
            "REMOVE IT. KEEP MY SETTINGS",
            "REMOVE IT AND MY SETTINGS TOO",
            "CANCEL",
        };
        unsigned at = 420;
        for (int i = 0; i < remove_choices; i++) {
            bool on = i == remove_choice;
            if (on) {
                canvas.rectangle(96, at - 14, 1200, 68, panel);
                canvas.rectangle(96, at - 14, 8, 68,
                                 i == 2 ? accent : bad);
            }
            label(canvas, 136, at, choices[i], 3,
                  on ? (i == 2 ? ink : bad) : dim);
            at += 88;
        }
        label(canvas, 96, 760, "YOUR SETTINGS ARE THE TUNING AND THE", 2, dim);
        label(canvas, 96, 798, "PER GAME PROFILES YOU HAVE SAVED.", 2, dim);
        label(canvas, 96, 980, "CROSS: CHOOSE    CIRCLE: BACK", 2, dim);
        return;
    }

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

    if (url[0] && now.reachable)
        std::snprintf(line, sizeof(line), "SETTINGS: %s", url);
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
        "REMOVE THE SERVICE FROM THIS CONSOLE",
    };

    unsigned row = 470;
    for (int i = 0; i < menu_items; i++) {
        bool on = i == selected;
        bool destructive = i == menu_items - 1;

        /* The last one is set apart: it undoes everything the others do. */
        if (destructive)
            row += 34;

        if (on) {
            canvas.rectangle(96, row - 14, 1728, 68, panel);
            canvas.rectangle(96, row - 14, 8, 68, destructive ? bad : accent);
        }
        char text[96];
        if (i == 2)
            std::snprintf(text, sizeof(text), "%s: %s", names[i],
                          auto_on ? "ON" : "OFF");
        else
            std::snprintf(text, sizeof(text), "%s", names[i]);
        label(canvas, 136, row, text, 3,
              on ? (destructive ? bad : ink) : dim);
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
