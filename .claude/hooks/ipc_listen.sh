#!/bin/bash
# IPC wake listener — blocks on FIFO, drains spool when woken.
# Launched as a background Bash task by Claude. Task-notification fires on exit.
# Loops internally on timeout — only exits when a real message arrives.
# Usage: ipc_listen.sh <role>
ROLE="${1:-coder}"
IPC_DIR="/tmp/urho_claude"
FIFO="$IPC_DIR/wake_${ROLE}"
SPOOL="$IPC_DIR/spool/to_${ROLE}"

# Ensure FIFO and spool exist
[ -p "$FIFO" ] || mkfifo "$FIFO" 2>/dev/null
mkdir -p "$SPOOL"

while true; do
    # Block until Manager writes to wake FIFO (or timeout after 300s)
    read -t 300 line < "$FIFO" 2>/dev/null

    # Check spool for messages
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
            FOUND=1
        fi
    fi

    # Only exit (delivering output to Claude) when we have a message
    if [ "$FOUND" -eq 1 ]; then
        break
    fi
    # No message — loop back and re-block on FIFO
done
