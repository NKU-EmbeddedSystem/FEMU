#!/bin/bash
# femu-restart.sh [ssd_size_mb] — restart FEMU inside the dedicated 'femu'
# tmux session so the QEMU console is always attachable:  tmux attach -t femu
#
# Requires the NOPASSWD grants in /etc/sudoers.d/99-liz-femu (qemu binary,
# pkill, the sysctl tee's and the ivshmem chmods used by run-cxlssd.sh).
#
# Flow: kill any running QEMU (sudo pkill, works even when guest SSH is
# dead after a crash) -> wait for the old tmux window's pipeline to unwind
# -> (re)create the 'femu' session with an interactive shell -> type the
# launch command into it. The session keeps its shell after QEMU exits, so
# the console state stays inspectable after a crash.
SZ=${1:-49152}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

sudo -n pkill -x qemu-system-x86 2>/dev/null
for i in $(seq 1 50); do
    pgrep -x qemu-system-x86 >/dev/null || break
    sleep 0.2
done
if pgrep -x qemu-system-x86 >/dev/null; then
    echo "femu-restart: old QEMU still alive after 10s, aborting" >&2
    exit 1
fi

if ! tmux has-session -t femu 2>/dev/null; then
    tmux new-session -d -s femu -c "$SCRIPT_DIR"
    sleep 0.5
else
    # window may still be unwinding its `sudo qemu | tee log` pipeline
    sleep 1
fi

tmux send-keys -t femu "./run-cxlssd.sh $SZ" C-m
echo "FEMU restarting in tmux session 'femu' — attach: tmux attach -t femu"
