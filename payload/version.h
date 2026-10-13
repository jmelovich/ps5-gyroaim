/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Reported by the portal so the control panel can tell whether the service
 * running on the console matches the one it has bundled.
 *
 * The version alone is not enough. Two builds of the same version are
 * different binaries, and comparing only the version let an older one survive
 * an app update: the panel installed the copy it carried, saw the versions
 * agree, and left it. GA_BUILD comes from the source tree, so any change to
 * it is visible even when the version has not moved. */
#define GA_VERSION "1.0.2"

/* The control panel is a title like any other, so it appears as a running
 * game and would otherwise get the transform installed into it. Hooking our
 * own front end is never wanted: holding the activation button would steer
 * its menu with the gyro. */
#ifndef GA_BUILD
#define GA_BUILD "unknown"
#endif

#define GA_PANEL_TITLE_ID "PPSA99510"
