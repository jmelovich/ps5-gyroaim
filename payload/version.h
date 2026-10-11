/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Reported by the portal so the control panel can tell whether the service
 * running on the console matches the one it has bundled. */
#define GA_VERSION "0.5.0"

/* The control panel is a title like any other, so it appears as a running
 * game and would otherwise get the transform installed into it. Hooking our
 * own front end is never wanted: holding the activation button would steer
 * its menu with the gyro. */
#define GA_PANEL_TITLE_ID "PPSA99510"
