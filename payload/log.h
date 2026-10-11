/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

/* Opens the log file under /data/gyroaim/, falling back to klog only.
 * Safe to call more than once. */
void ga_log_open(void);

/* Writes one line to the kernel log (prefixed "[GyroAim]") and to the log
 * file. A trailing newline is added if the caller leaves it out. */
void ga_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies the most recent log lines into buf, oldest first, for the portal.
 * Returns the number of bytes written. */
size_t ga_log_tail(char *buf, size_t n);

void ga_log_close(void);
