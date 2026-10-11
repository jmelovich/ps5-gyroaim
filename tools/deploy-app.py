#!/usr/bin/env python3
"""Send the built title to the console over FTP.

    tools/deploy-app.py <host> <title-dir> [port]

Uploads into /data/homebrew/<TITLEID>, creating directories as needed. The
title must not be running: the console will not let its files be replaced.
"""
import ftplib
import os
import sys


def main() -> int:
    if not 3 <= len(sys.argv) <= 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    host, local = sys.argv[1], sys.argv[2].rstrip("/")
    port = int(sys.argv[3]) if len(sys.argv) == 4 else 2121
    title = os.path.basename(local)
    base = f"/data/homebrew/{title}"

    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=60)
    ftp.login()

    def ensure(path: str) -> None:
        try:
            ftp.mkd(path)
        except ftplib.error_perm:
            pass

    ensure(base)
    sent = 0
    for dirpath, _dirs, files in os.walk(local):
        rel = os.path.relpath(dirpath, local)
        remote = base if rel == "." else f"{base}/{rel.replace(os.sep, '/')}"
        if rel != ".":
            ensure(remote)
        for name in sorted(files):
            path = os.path.join(dirpath, name)
            with open(path, "rb") as fh:
                ftp.storbinary(f"STOR {remote}/{name}", fh)
            sent += os.path.getsize(path)
            print(f"  {remote}/{name}")

    ftp.quit()
    print(f"sent {sent} bytes to {host}:{base}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
