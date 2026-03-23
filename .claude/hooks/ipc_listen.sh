#!/bin/bash
# IPC wake listener — blocks on FIFO, drains spool when woken.
# Launched as a background Bash task by Claude. Task-notification fires on exit.
# Usage: ipc_listen.sh <role>
ROLE="${1:-coder}"
IPC_DIR="/tmp/urho_claude"
FIFO="$IPC_DIR/wake_${ROLE}"
SPOOL="$IPC_DIR/spool/to_${ROLE}"

# Ensure FIFO and spool exist
[ -p "$FIFO" ] || mkfifo "$FIFO" 2>/dev/null
mkdir -p "$SPOOL"

# Block until Manager writes to wake FIFO (or timeout after 60s for re-arm)
read -t 60 line < "$FIFO" 2>/dev/null

# Drain spool — all pending messages, in sequence order
FOUND=0
for msg in $(ls -1 "$SPOOL"/*.msg 2>/dev/null | sort); do
    FOUND=1
    # Print body (everything after first --- line)
    sed '1,/^---$/d' "$msg"
    echo "---"
    rm -f "$msg"
done

if [ "$FOUND" -eq 0 ]; then
    # Fallback: legacy drop-file (transition period)
    MSG_FILE="$IPC_DIR/to_${ROLE}.msg"
    if [ -f "$MSG_FILE" ]; then
        cat "$MSG_FILE"
        rm -f "$MSG_FILE"
    else
        echo "[wake: no pending messages]"
    fi
fi
