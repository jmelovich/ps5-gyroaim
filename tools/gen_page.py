#!/usr/bin/env python3
"""Turn the portal's HTML into a C string literal.

    tools/gen_page.py payload/web/portal.html payload/web_page.h

Authoring the page as a real file beats escaping it by hand inside C. Blank
lines and indentation go away to keep the payload small; the newlines that
remain matter, because the page's script relies on them.
"""
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as fh:
        lines = [ln.strip() for ln in fh]

    kept = [ln for ln in lines if ln]

    out = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/* Generated from web/portal.html by tools/gen_page.py. Do not edit. */",
        "#pragma once",
        "",
        "static const char k_page[] =",
    ]
    for ln in kept:
        esc = ln.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'    "{esc}\\n"')
    out.append("    ;")
    out.append("")

    with open(dst, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))

    size = sum(len(ln) + 1 for ln in kept)
    print(f"{dst}: {len(kept)} lines, {size} bytes of page")
    return 0


if __name__ == "__main__":
    sys.exit(main())
