/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * config_test.c - the settings in force for a game, built on the host.
 *
 * It exists because of a bug it would have caught, reported from a console:
 * a game was launched, the page showed that game's id, and the settings in
 * use were the previous game's. Two faults, both in the same few lines.
 *
 * A game's profile was loaded straight over whatever was already live, so
 * every key the profile did not name kept the last game's value. Profiles
 * written by older builds name fewer keys with every setting added since, so
 * the leak widened with each release. And a game with no profile at all kept
 * the previous game's settings entirely, then saved them over the general
 * ones the next time anything changed.
 *
 * There is no console here and none is needed: the layering is ordinary file
 * reading, and GA_DIR points at a temporary directory for the duration.
 */
#include "../payload/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

static int failures;

static void check(const char *what, int ok, double got, double want)
{
    if (ok) {
        printf("  ok   %-52s %g\n", what, got);
        return;
    }
    printf("  FAIL %-52s got %g, wanted %g\n", what, got, want);
    failures++;
}

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        printf("  cannot write %s\n", path);
        exit(1);
    }
    fputs(body, f);
    fclose(f);
}

int main(void)
{
    ga_config_t c;
    char path[GA_PATH_MAX];

    mkdir(GA_DIR, 0777);

    /* The general settings: a complete file, as the service writes them. */
    ga_config_defaults(&c);
    c.max_turn = 300.0f;
    c.strength[GA_SRC_YAW] = 2.0f;
    c.full_angle[GA_SRC_YAW] = 20.0f;
    c.activation = GA_ACT_ALWAYS;
    ga_config_save(&c, GA_CONFIG_PATH);

    /* One game with a full profile of its own. */
    write_file(GA_DIR "/PPSA00001.txt",
               "max_turn=90.0\nstrength_yaw=1.55\nfull_angle_yaw=30.0\n"
               "activation=0\nread_yaw=1\nroute_yaw_lx=-1\n");

    /* And one with a sparse profile, as an older build would have left it:
     * it names the condition and nothing else. */
    write_file(GA_DIR "/PPSA00002.txt", "activation=2\ntrigger_right=1\n");

    printf("a game's own profile takes effect\n");
    int own = ga_config_for_title(&c, "PPSA00001", path, sizeof(path));
    check("the game's own profile is reported as in force", own == 1, own, 1);
    check("and is the file changes are kept in",
          !strcmp(path, GA_DIR "/PPSA00001.txt"), 1, 1);
    check("a value it names comes from it", fabsf(c.max_turn - 90.0f) < 0.01f,
          c.max_turn, 90.0);

    printf("switching games leaves nothing of the last one behind\n");

    /* Straight from the game above into the sparse one. Its profile names the
     * condition only, so everything else must come from the general settings,
     * never from the game just left. This is the reported fault exactly. */
    own = ga_config_for_title(&c, "PPSA00002", path, sizeof(path));
    check("the sparse profile is still in force", own == 1, own, 1);
    check("what it names comes from it",
          c.activation == GA_ACT_TRIGGER && c.trigger_right == 1,
          c.activation, GA_ACT_TRIGGER);
    check("what it leaves out falls back to the general settings",
          fabsf(c.max_turn - 300.0f) < 0.01f, c.max_turn, 300.0);
    check("and not to the game just left",
          fabsf(c.strength[GA_SRC_YAW] - 2.0f) < 0.01f,
          c.strength[GA_SRC_YAW], 2.0);
    check("including how a rotation is read",
          c.read_mode[GA_SRC_YAW] == GA_READ_FLOW,
          c.read_mode[GA_SRC_YAW], GA_READ_FLOW);
    check("and where it is routed", c.route[GA_SRC_YAW][GA_OUT_LEFT_X] == 0,
          c.route[GA_SRC_YAW][GA_OUT_LEFT_X], 0);

    printf("a game with no profile gets the general settings\n");

    /* The other half of the fault: with nothing saved for this game, the
     * previous game's settings stayed live and the next change wrote them
     * over the general ones. */
    ga_config_for_title(&c, "PPSA00001", path, sizeof(path));
    own = ga_config_for_title(&c, "PPSA09999", path, sizeof(path));
    check("no profile of its own is reported", own == 0, own, 0);
    check("changes are kept in the general settings",
          !strcmp(path, GA_CONFIG_PATH), 1, 1);
    check("and the general settings are what is in force",
          fabsf(c.max_turn - 300.0f) < 0.01f, c.max_turn, 300.0);
    check("not the settings of the game just left",
          fabsf(c.full_angle[GA_SRC_YAW] - 20.0f) < 0.01f,
          c.full_angle[GA_SRC_YAW], 20.0);

    printf("a key in no file at all falls back to its default\n");
    {
        ga_config_t d;

        ga_config_defaults(&d);
        /* hold_correct is in neither file above, as it would not be in any
         * file written before it existed. */
        ga_config_for_title(&c, "PPSA00002", path, sizeof(path));
        check("the default is used", fabsf(c.hold_correct - d.hold_correct)
              < 0.001f, c.hold_correct, d.hold_correct);
    }

    printf(failures ? "\nconfig checks FAILED\n"
                    : "\nall config checks passed\n");
    return failures ? 1 : 0;
}
