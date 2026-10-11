/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>

namespace gyroaim
{
/* Where the service listens, and where the console's ELF loader listens.
 *
 * Both are reached over loopback. A title cannot run the service itself, but
 * it can hand the loader a payload to run, which is how the panel starts it. */
constexpr int portal_port = 6080;
constexpr int loader_port = 9021;

/* The copy of the service that ships inside this title. */
constexpr const char *bundled_service = "/app0/gyroaim.elf";

/* Where pldmgr looks at boot, and the list it reads. */
constexpr const char *autostart_dir = "/data/pldmgr/payloads/gyroaim";
constexpr const char *autostart_elf =
    "/data/pldmgr/payloads/gyroaim/gyroaim.elf";
constexpr const char *autostart_list = "/data/pldmgr/autoload.txt";

struct Status {
    bool  reachable;        /* the service answered */
    bool  kstuff;           /* the transform can be installed at all */
    bool  attached;         /* it is hooked into a running game */
    char  version[16];
    char  game[16];
    char  text[96];         /* the service's own one-line summary */
};

/* Asks the service how it is. Fills `out` and returns true if it answered. */
bool query(Status &out);

/* Hands the bundled payload to the console's ELF loader. A service already
 * running stands down on its own when the new one takes the port, so this is
 * both "start" and "restart". */
bool start_service(char *why, std::size_t why_len);

/* Asks a running service to unhook and exit. */
bool stop_service();

/* Copies the bundled payload where pldmgr will find it at boot and adds it to
 * the list. Both steps are safe to repeat. */
bool enable_autostart(char *why, std::size_t why_len);
bool disable_autostart(char *why, std::size_t why_len);
bool autostart_enabled();

/* Takes the service off the console.
 *
 * The autostart copy and pldmgr's list are handled here, because they are
 * plain files and the panel may be the only thing running. Everything under
 * /data/gyroaim is handled by the service itself, which runs outside the
 * sandbox where a directory can be read; if it is not running it is started
 * for the purpose and exits once done.
 *
 * `with_settings` also removes tuning and per-game profiles. */
bool remove_service(bool with_settings, char *why, std::size_t why_len);

/* The console's address on the network, for the settings page's link. */
bool local_address(char *out, std::size_t out_len);
} // namespace gyroaim
