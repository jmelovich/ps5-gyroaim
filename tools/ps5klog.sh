#!/usr/bin/env bash
# Capture a jailbroken PS5's kernel log (klogsrv, TCP 3232) to a file.
#
# klogsrv drops the connection periodically, so this reconnects on its own.
# Run it detached so it survives a single tool call:
#   setsid nohup ps5klog.sh 192.168.0.192 /tmp/klog.txt >/dev/null 2>&1 </dev/null &
#
# Then watch only what matters, e.g.:
#   tail -n 0 -F /tmp/klog.txt | grep -E "\[MYAPP\]|signal:|proc name:|reason:"
set -u
IP=${1:?usage: ps5klog.sh <ps5-ip> <output-file>}
OUT=${2:?usage: ps5klog.sh <ps5-ip> <output-file>}
while true; do
    timeout 3600 bash -c "cat < /dev/tcp/$IP/3232" >> "$OUT" 2>/dev/null
    echo "--- klog reconnect $(date +%H:%M:%S) ---" >> "$OUT"
    sleep 2
done
