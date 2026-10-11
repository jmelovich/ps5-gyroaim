/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define GA_PROC_NAME_MAX 20
#define GA_TITLE_ID_MAX  10
#define GA_PROC_MAX      256

typedef struct {
    pid_t pid;
    char  name[GA_PROC_NAME_MAX];      /* ki_comm, e.g. "eboot.bin" */
    char  title_id[GA_TITLE_ID_MAX];   /* "" unless the process is a title */
} ga_proc_t;

/* Snapshots the process table. Returns the count written, at most max. */
unsigned ga_proc_list(ga_proc_t *out, unsigned max);

/* Finds processes whose command name matches exactly, ignoring our own.
 * Returns the count written. */
unsigned ga_proc_find(const char *name, pid_t *out, unsigned max);

/* True if the pid exists. */
int ga_proc_alive(pid_t pid);

/* The title id of a running title, e.g. "PPSA99350". Returns 0 on success. */
int ga_proc_title_id(pid_t pid, char out[GA_TITLE_ID_MAX]);

/* Confirms the process table is readable and that we can see ourselves in it.
 * Returns 0 on success, logging what it found either way. */
int ga_proc_self_check(void);

/* Our own command name, for diagnostics only. Never match payloads on it:
 * the ELF loader names every payload "payload.elf". */
int ga_proc_self_name(char *out, size_t n);
