/* SPDX-License-Identifier: GPL-3.0-or-later
 * service.cpp - talking to the service, and to the console's ELF loader.
 *
 * Everything here happens over loopback or on plain files. The panel needs no
 * elevation: handing a payload to the loader is an ordinary socket write, and
 * the directories pldmgr reads are world-writable.
 */
#include "service.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gyroaim
{
namespace
{
constexpr int connect_timeout_ms = 1500;

int dial(int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    timeval tv{};
    tv.tv_sec = connect_timeout_ms / 1000;
    tv.tv_usec = (connect_timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool write_all(int fd, const char *data, std::size_t len)
{
    while (len) {
        ssize_t w = ::send(fd, data, len, 0);
        if (w <= 0)
            return false;
        data += w;
        len -= static_cast<std::size_t>(w);
    }
    return true;
}

/* One GET, with the whole response collected. */
int http_get(const char *path, char *out, std::size_t out_len)
{
    int fd = dial(portal_port);
    if (fd < 0)
        return -1;

    char request[256];
    int n = std::snprintf(request, sizeof(request),
                          "GET %s HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n", path);
    if (!write_all(fd, request, static_cast<std::size_t>(n))) {
        ::close(fd);
        return -1;
    }

    std::size_t got = 0;
    while (got + 1 < out_len) {
        ssize_t r = ::recv(fd, out + got, out_len - 1 - got, 0);
        if (r <= 0)
            break;
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    out[got] = '\0';
    return static_cast<int>(got);
}

/* Minimal lookups into a flat JSON object. The service's replies are written
 * by the service itself, so a full parser would be weight for nothing. */
const char *find_value(const char *json, const char *key)
{
    char needle[48];
    std::snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = std::strstr(json, needle);
    return at ? at + std::strlen(needle) : nullptr;
}

bool json_bool(const char *json, const char *key)
{
    const char *v = find_value(json, key);
    return v && (*v == '1' || *v == 't');
}

void json_string(const char *json, const char *key, char *out,
                 std::size_t out_len)
{
    out[0] = '\0';
    const char *v = find_value(json, key);
    if (!v || *v != '"')
        return;
    v++;
    std::size_t i = 0;
    while (*v && *v != '"' && i + 1 < out_len)
        out[i++] = *v++;
    out[i] = '\0';
}
} // namespace

bool query(Status &out)
{
    char body[4096];

    std::memset(&out, 0, sizeof(out));
    if (http_get("/api/state", body, sizeof(body)) <= 0)
        return false;
    if (!std::strstr(body, "200 OK"))
        return false;

    out.reachable = true;
    out.kstuff = json_bool(body, "kstuff");
    out.attached = json_bool(body, "attached");
    json_string(body, "version", out.version, sizeof(out.version));
    json_string(body, "title", out.game, sizeof(out.game));
    json_string(body, "status", out.text, sizeof(out.text));
    return true;
}

bool stop_service()
{
    char body[512];
    return http_get("/api/quit", body, sizeof(body)) > 0;
}

bool start_service(char *why, std::size_t why_len)
{
    int file = ::open(bundled_service, O_RDONLY);
    if (file < 0) {
        std::snprintf(why, why_len, "the app is missing its copy of the service");
        return false;
    }

    int fd = dial(loader_port);
    if (fd < 0) {
        ::close(file);
        std::snprintf(why, why_len,
                      "nothing is listening on port %d, so the console has no "
                      "payload loader running", loader_port);
        return false;
    }

    char buffer[32768];
    bool ok = true;
    ssize_t r;
    std::size_t sent = 0;
    while ((r = ::read(file, buffer, sizeof(buffer))) > 0) {
        if (!write_all(fd, buffer, static_cast<std::size_t>(r))) {
            ok = false;
            break;
        }
        sent += static_cast<std::size_t>(r);
    }
    ::close(file);
    /* The loader reads until the connection closes. */
    ::shutdown(fd, SHUT_WR);
    ::close(fd);

    if (!ok || sent == 0) {
        std::snprintf(why, why_len, "sending the service to the loader failed");
        return false;
    }
    std::snprintf(why, why_len, "sent %zu bytes to the loader", sent);
    return true;
}

/* ---- starting with the console ----------------------------------------- */

namespace
{
bool copy_file(const char *from, const char *to, char *why, std::size_t why_len)
{
    int in = ::open(from, O_RDONLY);
    if (in < 0) {
        std::snprintf(why, why_len, "cannot read %s", from);
        return false;
    }
    int out = ::open(to, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) {
        ::close(in);
        std::snprintf(why, why_len, "cannot write %s", to);
        return false;
    }

    char buffer[32768];
    ssize_t r;
    bool ok = true;
    while ((r = ::read(in, buffer, sizeof(buffer))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = ::write(out, buffer + off, static_cast<std::size_t>(r - off));
            if (w <= 0) {
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok)
            break;
    }
    ::close(in);
    ::close(out);
    if (!ok)
        std::snprintf(why, why_len, "writing %s failed part way", to);
    return ok;
}

bool list_contains_us(char *buf, std::size_t len, std::size_t *out_len)
{
    FILE *f = std::fopen(autostart_list, "r");
    if (!f)
        return false;
    std::size_t n = std::fread(buf, 1, len - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    if (out_len)
        *out_len = n;
    return std::strstr(buf, "gyroaim.elf") != nullptr;
}
} // namespace

bool autostart_enabled()
{
    char list[4096];
    struct stat st{};

    if (::stat(autostart_elf, &st) != 0)
        return false;
    return list_contains_us(list, sizeof(list), nullptr);
}

bool enable_autostart(char *why, std::size_t why_len)
{
    struct stat st{};

    if (::stat("/data", &st) != 0) {
        std::snprintf(why, why_len,
                      "/data is not visible to this app, so ShadowMount+ is "
                      "too old to mount it");
        return false;
    }

    ::mkdir("/data/pldmgr", 0777);
    ::mkdir("/data/pldmgr/payloads", 0777);
    ::mkdir(autostart_dir, 0777);

    if (!copy_file(bundled_service, autostart_elf, why, why_len))
        return false;

    char list[4096];
    std::size_t len = 0;
    bool present = list_contains_us(list, sizeof(list), &len);
    if (present) {
        std::snprintf(why, why_len, "already set to start with the console");
        return true;
    }

    /* Appended last so the services it depends on are loaded first. */
    FILE *f = std::fopen(autostart_list, "a");
    if (!f) {
        std::snprintf(why, why_len, "cannot add it to %s", autostart_list);
        return false;
    }
    if (len && list[len - 1] != '\n')
        std::fputc('\n', f);
    std::fputs("gyroaim.elf\n", f);
    std::fclose(f);

    std::snprintf(why, why_len, "it will now start with the console");
    return true;
}

bool disable_autostart(char *why, std::size_t why_len)
{
    char list[4096];
    std::size_t len = 0;

    if (!list_contains_us(list, sizeof(list), &len)) {
        ::unlink(autostart_elf);
        std::snprintf(why, why_len, "it was not set to start with the console");
        return true;
    }

    /* Rewrite the list without our line, leaving every other payload alone. */
    FILE *f = std::fopen(autostart_list, "w");
    if (!f) {
        std::snprintf(why, why_len, "cannot rewrite %s", autostart_list);
        return false;
    }
    char *line = list;
    while (line && *line) {
        char *end = std::strchr(line, '\n');
        std::size_t l = end ? static_cast<std::size_t>(end - line) : std::strlen(line);
        if (l && std::strncmp(line, "gyroaim.elf", 11) != 0) {
            std::fwrite(line, 1, l, f);
            std::fputc('\n', f);
        }
        line = end ? end + 1 : nullptr;
    }
    std::fclose(f);
    ::unlink(autostart_elf);

    std::snprintf(why, why_len, "it will no longer start with the console");
    return true;
}

/* ---- taking it off the console ----------------------------------------- */

bool remove_service(bool with_settings, char *why, std::size_t why_len)
{
    char ignored[96];
    Status state{};

    /* The copy pldmgr starts at boot, and its line in pldmgr's list. Done
     * first, so a failure later still leaves nothing to come back at boot. */
    bool autostart_gone = disable_autostart(ignored, sizeof(ignored));

    bool running = query(state);

    if (!running && with_settings) {
        /* Nothing is running to do the directory work, so start the copy this
         * app carries, purely to have it remove the rest and exit. */
        if (!start_service(ignored, sizeof(ignored))) {
            std::snprintf(why, why_len,
                          "removed the startup copy, but could not start the "
                          "service to clear its settings");
            return autostart_gone;
        }
        for (int i = 0; i < 20 && !running; i++) {
            usleep(300000);
            running = query(state);
        }
    }

    if (!running) {
        std::snprintf(why, why_len,
                      "removed: it will not start with the console again");
        return true;
    }

    char body[512];
    int rc = http_get(with_settings ? "/api/uninstall?settings=1"
                                    : "/api/uninstall",
                      body, sizeof(body));
    if (rc <= 0) {
        std::snprintf(why, why_len,
                      "removed the startup copy, but the running service did "
                      "not answer");
        return autostart_gone;
    }

    /* It unhooks any running game, clears up, and exits. */
    for (int i = 0; i < 20; i++) {
        usleep(300000);
        Status after{};
        if (!query(after)) {
            std::snprintf(why, why_len, "removed%s",
                          with_settings ? ", settings included"
                                        : ", settings kept");
            return true;
        }
    }
    std::snprintf(why, why_len, "asked it to remove itself but it is still up");
    return false;
}

/* ---- the console's address --------------------------------------------- *
 *
 * Deliberately plain sockets. The obvious call, sceNetCtlGetInfo, lives in a
 * module a native title does not load, and calling it faults on an address
 * that was never resolved. The boilerplate's own notes say the same of
 * getaddrinfo and friends.
 *
 * So: point a UDP socket at a documented test address and ask the kernel
 * which of our own addresses it would use. Nothing is sent, because connect
 * on a datagram socket only chooses a route.
 */
bool local_address(char *out, std::size_t out_len)
{
    sockaddr_in remote{};
    sockaddr_in mine{};
    socklen_t len = sizeof(mine);

    out[0] = '\0';
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;

    remote.sin_family = AF_INET;
    remote.sin_port = htons(9);                 /* discard */
    remote.sin_addr.s_addr = htonl(0xc0000201); /* 192.0.2.1, TEST-NET-1 */

    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&remote),
                  sizeof(remote)) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&mine), &len) == 0 &&
        mine.sin_addr.s_addr != 0) {
        /* Formatted by hand rather than with inet_ntop, which is one more
         * symbol that might not be there. */
        uint32_t host = ntohl(mine.sin_addr.s_addr);
        std::snprintf(out, out_len, "%u.%u.%u.%u", (host >> 24) & 0xff,
                      (host >> 16) & 0xff, (host >> 8) & 0xff, host & 0xff);
        ok = out[0] != '\0' && std::strcmp(out, "127.0.0.1") != 0;
    }
    ::close(fd);
    return ok;
}
} // namespace gyroaim
