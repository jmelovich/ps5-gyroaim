/* SPDX-License-Identifier: GPL-3.0-or-later
 * qr_test.c - checks how the panel uses the QR encoder.
 *
 * The encoder itself is a widely used library pinned by content hash, so what
 * is worth testing is this project's use of it: that the settings URL encodes,
 * that the result is the size and shape a scanner expects, and that the three
 * finder patterns really are where they should be. A code rendered with the
 * wrong orientation or without a quiet zone will not scan, and neither
 * failure is visible from a kernel log.
 *
 * It also writes the code out as a PGM at the scale the panel draws it, so a
 * person can scan it with a phone and settle the question properly.
 */
#include "../app/src/vendor/qrcodegen.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *what, bool ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* A finder pattern is a 7x7 ring: filled border, gap, filled 3x3 centre. */
static bool finder_at(const uint8_t *qr, int ox, int oy)
{
    static const char expect[7][8] = {
        "#######", "#.....#", "#.###.#", "#.###.#",
        "#.###.#", "#.....#", "#######",
    };

    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++) {
            bool want = expect[y][x] == '#';
            if (qrcodegen_getModule(qr, ox + x, oy + y) != want)
                return false;
        }
    return true;
}

static void write_pgm(const uint8_t *qr, const char *path, int scale)
{
    const int quiet = 4;
    int modules = qrcodegen_getSize(qr);
    int across = (modules + quiet * 2) * scale;
    FILE *f = fopen(path, "wb");

    if (!f)
        return;
    fprintf(f, "P5\n%d %d\n255\n", across, across);
    for (int y = 0; y < across; y++)
        for (int x = 0; x < across; x++) {
            int mx = x / scale - quiet;
            int my = y / scale - quiet;
            bool dark = mx >= 0 && my >= 0 && mx < modules && my < modules &&
                        qrcodegen_getModule(qr, mx, my);
            fputc(dark ? 0 : 255, f);
        }
    fclose(f);
    printf("  wrote %s, %d by %d pixels\n", path, across, across);
}

int main(void)
{
    /* What the panel shows: the font is uppercase, and a URL scheme is
     * case insensitive, so this is what a phone actually receives. */
    const char *url = "HTTP://192.168.0.192:6080";
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t scratch[qrcodegen_BUFFER_LEN_MAX];

    printf("encoding %s\n", url);
    bool ok = qrcodegen_encodeText(url, scratch, qr, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    check("the settings URL encodes", ok);
    if (!ok)
        return 1;

    int size = qrcodegen_getSize(qr);
    printf("  %d by %d modules\n", size, size);
    check("size is a valid QR size", size >= 21 && size <= 177 &&
                                     (size - 21) % 4 == 0);

    /* The panel draws into a 620 pixel box with a 4 module quiet zone. A
     * scale below about 6 would be hard for a phone to read across a room. */
    int across = size + 8;
    int scale = 620 / across;
    printf("  drawn at %d pixels per module, %d across\n", scale,
           across * scale);
    check("modules are big enough to scan", scale >= 6);

    check("finder pattern, top left", finder_at(qr, 0, 0));
    check("finder pattern, top right", finder_at(qr, size - 7, 0));
    check("finder pattern, bottom left", finder_at(qr, 0, size - 7));
    /* The fourth corner must NOT have one; that is how a scanner finds the
     * orientation. */
    check("no finder pattern, bottom right",
          !finder_at(qr, size - 7, size - 7));

    /* A longer address must still fit, since not every network is a /24. */
    ok = qrcodegen_encodeText("HTTP://192.168.100.200:6080", scratch, qr,
                              qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
                              true);
    check("a longer address also encodes", ok);

    ok = qrcodegen_encodeText(url, scratch, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true);
    if (ok)
        write_pgm(qr, "qr-sample.pgm", scale);

    printf(failures ? "\n%d failure(s)\n" : "\nall QR checks passed\n",
           failures);
    return failures ? 1 : 0;
}
