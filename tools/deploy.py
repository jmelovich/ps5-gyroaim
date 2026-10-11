#!/usr/bin/env python3
"""Send a payload ELF to the PS5's ELF loader.

    tools/deploy.py <host> <file> [port]

The loader reads until the connection closes, so this writes the whole file
and then shuts the socket down for writing before waiting for the close.
"""
import socket
import sys


def main() -> int:
    if not 3 <= len(sys.argv) <= 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    host, path = sys.argv[1], sys.argv[2]
    port = int(sys.argv[3]) if len(sys.argv) == 4 else 9021

    with open(path, "rb") as fh:
        blob = fh.read()

    with socket.create_connection((host, port), timeout=15) as sock:
        sock.sendall(blob)
        sock.shutdown(socket.SHUT_WR)
        sock.settimeout(10)
        try:
            while sock.recv(4096):
                pass
        except OSError:
            pass

    print(f"sent {len(blob)} bytes of {path} to {host}:{port}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
